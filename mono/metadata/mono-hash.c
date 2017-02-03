/*
 * ghashtable.c: Hashtable implementation
 *
 * Author:
 *   Miguel de Icaza (miguel@novell.com)
 *
 * (C) 2006 Novell, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <config.h>
#include <stdio.h>
#include <math.h>
#include <glib.h>
#include "mono-hash.h"
#include "metadata/gc-internals.h"
#include <mono/utils/checked-build.h>
#include <mono/utils/mono-threads-coop.h>

#ifdef HAVE_BOEHM_GC
#define mg_new0(type,n)  ((type *) GC_MALLOC(sizeof(type) * (n)))
#define mg_new(type,n)   ((type *) GC_MALLOC(sizeof(type) * (n)))
#define mg_free(x)       do { } while (0)
#else
#define mg_new0(x,n)     g_new0(x,n)
#define mg_new(type,n)   g_new(type,n)
#define mg_free(x)       g_free(x)
#endif

typedef struct _Slot Slot;

/* Synchronized with MonoGHashTableHelpers.cs */
struct _Slot {
	MonoObject *key;
	MonoObject *value;
};

struct _MonoGHashTable {
	GHashFunc      hash_func;
	GEqualFunc     key_equal_func;

	MonoArray *table;
	/*
	 * We use this during resizes so we can easily keep both arrays alive.
	 * It is null otherwise. We could instead allocate a pinned handle.
	 */
	MonoArray *old_table;

	int   in_use;
	GDestroyNotify value_destroy_func, key_destroy_func;
	MonoGHashGCType gc_type;
	MonoGCRootSource source;
	const char *msg;
};

static MonoGHashTable *
mono_g_hash_table_new (GHashFunc hash_func, GEqualFunc key_equal_func);

#ifdef HAVE_SGEN_GC
static MonoGCDescriptor table_hash_descr = MONO_GC_DESCRIPTOR_NULL;

static void mono_g_hash_mark (void *addr, MonoGCMarkFunc mark_func, void *gc_data);
#endif

#if UNUSED
static gboolean
test_prime (int x)
{
	if ((x & 1) != 0) {
		int n;
		for (n = 3; n< (int)sqrt (x); n += 2) {
			if ((x % n) == 0)
				return FALSE;
		}
		return TRUE;
	}
	// There is only one even prime - 2.
	return (x == 2);
}

static int
calc_prime (int x)
{
	int i;
	
	for (i = (x & (~1))-1; i< G_MAXINT32; i += 2) {
		if (test_prime (i))
			return i;
	}
	return x;
}
#endif

#define HASH_TABLE_MAX_LOAD_FACTOR 0.7f
#define HASH_TABLE_MIN_LOAD_FACTOR 0.05f /* We didn't really do compaction before, keep it lenient for now */

static inline void mono_g_hash_table_key_store (MonoGHashTable *hash, int slot, MonoObject* key)
{
	MonoObject **key_addr = &((Slot*)hash->table->vector) [slot].key;
	if (hash->gc_type & MONO_HASH_KEY_GC)
		mono_gc_wbarrier_generic_store (key_addr, key);
	else
		*key_addr = key;
}

static inline void mono_g_hash_table_value_store (MonoGHashTable *hash, int slot, MonoObject* value)
{
	MonoObject **value_addr = &((Slot*)hash->table->vector) [slot].value;
	if (hash->gc_type & MONO_HASH_VALUE_GC)
		mono_gc_wbarrier_generic_store (value_addr, value);
	else
		*value_addr = value;
}

static inline MonoClass* mono_g_hash_table_get_element_class (MonoGHashTable *hash)
{
	switch (hash->gc_type) {
		case MONO_HASH_KEY_GC:
			return mono_defaults.hash_table_slot_k;
		case MONO_HASH_VALUE_GC:
			return mono_defaults.hash_table_slot_v;
		case MONO_HASH_KEY_VALUE_GC:
			return mono_defaults.hash_table_slot_kv;
		default:
			return mono_defaults.hash_table_slot_nogc;
	}
}

/* Returns position of key or of an empty slot for it */
static inline int mono_g_hash_table_find_slot (MonoGHashTable *hash, const MonoObject *key)
{
	guint i = ((*hash->hash_func) (key)) % mono_array_length_fast (hash->table);
	GEqualFunc equal = hash->key_equal_func;
	Slot *table = (Slot*) hash->table->vector;

	while (table [i].key && !(*equal) (table [i].key, key))
		i = (i + 1) % mono_array_length_fast (hash->table);
	return i;
}


MonoGHashTable *
mono_g_hash_table_new_type (GHashFunc hash_func, GEqualFunc key_equal_func, MonoGHashGCType type, MonoGCRootSource source, const char *msg)
{
	MonoGHashTable *hash = mono_g_hash_table_new (hash_func, key_equal_func);

	hash->gc_type = type;
	hash->source = source;
	hash->msg = msg;

	if (type > MONO_HASH_KEY_VALUE_GC)
		g_error ("wrong type for gc hashtable");

#ifdef HAVE_SGEN_GC
	/*
	 * We use a user defined marking function to avoid having to register a GC root for
	 * each hash node.
	 */
	if (!table_hash_descr)
		table_hash_descr = mono_gc_make_root_descr_user (mono_g_hash_mark);
	mono_gc_register_root_wbarrier ((char*)hash, sizeof (MonoGHashTable), table_hash_descr, source, msg);
#endif

	return hash;
}

static MonoGHashTable *
mono_g_hash_table_new (GHashFunc hash_func, GEqualFunc key_equal_func)
{
	MonoGHashTable *hash;

	if (!hash_func)
		hash_func = g_direct_hash;
	if (!key_equal_func)
		key_equal_func = g_direct_equal;
	hash = mg_new0 (MonoGHashTable, 1);

	hash->hash_func = hash_func;
	hash->key_equal_func = key_equal_func;

	hash->table = mono_array_new (mono_get_root_domain (),
				mono_g_hash_table_get_element_class (hash),
				g_spaced_primes_closest (1));
	
	return hash;
}

static void
rehash (MonoGHashTable *hash)
{
	Slot *old_table;
	int new_size = g_spaced_primes_closest (hash->in_use / HASH_TABLE_MAX_LOAD_FACTOR);
	int i;

	MONO_REQ_GC_UNSAFE_MODE; //we must run in unsafe mode to make rehash safe

	hash->old_table = hash->table;
	hash->table = mono_array_new (mono_get_root_domain (),
			mono_g_hash_table_get_element_class (hash),
			new_size);

	old_table = (Slot*)hash->old_table->vector;
	for (i = 0; i < mono_array_length_fast (hash->old_table); i++) {
		if (old_table [i].key) {
			int slot = mono_g_hash_table_find_slot (hash, old_table [i].key);
			mono_g_hash_table_key_store (hash, slot, old_table [i].key);
			mono_g_hash_table_value_store (hash, slot, old_table [i].value);
		}
	}
	hash->old_table = NULL;
}

guint
mono_g_hash_table_size (MonoGHashTable *hash)
{
	g_return_val_if_fail (hash != NULL, 0);
	
	return hash->in_use;
}

gpointer
mono_g_hash_table_lookup (MonoGHashTable *hash, gconstpointer key)
{
	gpointer orig_key, value;
	
	if (mono_g_hash_table_lookup_extended (hash, key, &orig_key, &value))
		return value;
	else
		return NULL;
}

gboolean
mono_g_hash_table_lookup_extended (MonoGHashTable *hash, gconstpointer key, gpointer *orig_key, gpointer *value)
{
	int slot;
	Slot *table;

	g_return_val_if_fail (hash != NULL, FALSE);

	slot = mono_g_hash_table_find_slot (hash, key);

	table = (Slot*)hash->table->vector;
	if (table [slot].key) {
		*orig_key = table [slot].key;
		*value = table [slot].value;
		return TRUE;
	}

	return FALSE;
}

void
mono_g_hash_table_foreach (MonoGHashTable *hash, GHFunc func, gpointer user_data)
{
	int i;
	Slot *table;
	
	g_return_if_fail (hash != NULL);
	g_return_if_fail (func != NULL);

	table = (Slot*)hash->table->vector;
	for (i = 0; i < mono_array_length_fast (hash->table); i++){
		if (table [i].key)
			(*func)(table [i].key, table [i].value, user_data);
	}
}

gpointer
mono_g_hash_table_find (MonoGHashTable *hash, GHRFunc predicate, gpointer user_data)
{
	int i;
	Slot *table;
	
	g_return_val_if_fail (hash != NULL, NULL);
	g_return_val_if_fail (predicate != NULL, NULL);

	table = (Slot*)hash->table->vector;
	for (i = 0; i < mono_array_length_fast (hash->table); i++){
		if (table [i].key && (*predicate)(table [i].key, table [i].value, user_data))
			return table [i].value;
	}
	return NULL;
}

gboolean
mono_g_hash_table_remove (MonoGHashTable *hash, gconstpointer key)
{
	int slot, last_clear_slot;
	Slot *table;

	g_return_val_if_fail (hash != NULL, FALSE);
	slot = mono_g_hash_table_find_slot (hash, key);

	table = (Slot*)hash->table->vector;
	if (!table [slot].key)
		return FALSE;

	if (hash->key_destroy_func)
		(*hash->key_destroy_func)(table [slot].key);
	table [slot].key = NULL;
	if (hash->value_destroy_func)
		(*hash->value_destroy_func)(table [slot].value);
	table [slot].value = NULL;
	hash->in_use--;

	/*
	 * When we insert in the hashtable, if the required position is occupied we
	 * consecutively try out following positions. In order to be able to find
	 * if a key exists or not in the array (without traversing the entire hash)
	 * we maintain the constraint that there can be no free slots between two
	 * entries that are hashed to the same position. This means that, at search
	 * time, when we encounter a free slot we can stop looking for collissions.
	 * Similarly, at remove time, we need to shift all following slots to their
	 * normal slot, until we reach an empty slot.
	 */
	last_clear_slot = slot;
	slot = (slot + 1) % mono_array_length_fast (hash->table);
	while (table [slot].key) {
		guint hashcode = ((*hash->hash_func)(table [slot].key)) % mono_array_length_fast (hash->table);
		/*
		 * We try to move the current element to last_clear_slot, but only if
		 * it brings it closer to its normal position (hashcode)
		 */
		if ((last_clear_slot < slot && (hashcode > slot || hashcode <= last_clear_slot)) ||
				(last_clear_slot > slot && (hashcode > slot && hashcode <= last_clear_slot))) {
			mono_g_hash_table_key_store (hash, last_clear_slot, table [slot].key);
			mono_g_hash_table_value_store (hash, last_clear_slot, table [slot].value);
			table [slot].key = NULL;
			table [slot].value = NULL;
			last_clear_slot = slot;
		}
		slot = (slot + 1) % mono_array_length_fast (hash->table);
	}
	return TRUE;
}

guint
mono_g_hash_table_foreach_remove (MonoGHashTable *hash, GHRFunc func, gpointer user_data)
{
	int i;
	int count = 0;
	Slot *table;

	g_return_val_if_fail (hash != NULL, 0);
	g_return_val_if_fail (func != NULL, 0);

	table = (Slot*)hash->table->vector;
	for (i = 0; i < mono_array_length_fast (hash->table); i++){
		if (table [i].key && (*func)(table [i].key, table [i].value, user_data)) {
			mono_g_hash_table_remove (hash, table [i].key);
			count++;
		}
	}
	if (hash->in_use < mono_array_length_fast (hash->table) * HASH_TABLE_MIN_LOAD_FACTOR)
		rehash (hash);
	return count;
}

void
mono_g_hash_table_destroy (MonoGHashTable *hash)
{
	int i;
	Slot *table;

	g_return_if_fail (hash != NULL);

#ifdef HAVE_SGEN_GC
	mono_gc_deregister_root ((char*)hash);
#endif

	table = (Slot*)hash->table->vector;
	for (i = 0; i < mono_array_length_fast (hash->table); i++) {
		if (table [i].key != NULL) {
			if (hash->key_destroy_func)
				(*hash->key_destroy_func)(table [i].key);
			if (hash->value_destroy_func)
				(*hash->value_destroy_func)(table [i].value);
		}
	}
	mg_free (hash);
}

static void
mono_g_hash_table_insert_replace (MonoGHashTable *hash, gpointer key, gpointer value, gboolean replace)
{
	int slot;
	Slot *table;
	g_return_if_fail (hash != NULL);

	if (hash->in_use > (mono_array_length_fast (hash->table) * HASH_TABLE_MAX_LOAD_FACTOR + 1))
		rehash (hash);

	slot = mono_g_hash_table_find_slot (hash, key);

	table = (Slot*)hash->table->vector;
	if (table [slot].key) {
		if (replace) {
			if (hash->key_destroy_func)
				(*hash->key_destroy_func)(table [slot].key);
			mono_g_hash_table_key_store (hash, slot, (MonoObject*)key);
		}
		if (hash->value_destroy_func)
			(*hash->value_destroy_func) (table [slot].value);
		mono_g_hash_table_value_store (hash, slot, (MonoObject*)value);
	} else {
		mono_g_hash_table_key_store (hash, slot, (MonoObject*)key);
		mono_g_hash_table_value_store (hash, slot, (MonoObject*)value);
		hash->in_use++;
	}
}

void
mono_g_hash_table_insert (MonoGHashTable *h, gpointer k, gpointer v)
{
	mono_g_hash_table_insert_replace (h, k, v, FALSE);
}

void
mono_g_hash_table_replace(MonoGHashTable *h, gpointer k, gpointer v)
{
	mono_g_hash_table_insert_replace (h, k, v, TRUE);
}

void
mono_g_hash_table_print_stats (MonoGHashTable *hash)
{
	int i = 0, chain_size = 0, max_chain_size = 0;
	gboolean wrapped_around = FALSE;
	Slot *table = (Slot*)hash->table->vector;

	while (TRUE) {
		if (table [i].key) {
			chain_size++;
		} else {
			max_chain_size = MAX(max_chain_size, chain_size);
			chain_size = 0;
			if (wrapped_around)
				break;
		}

		if (i == (mono_array_length_fast (hash->table) - 1)) {
			wrapped_around = TRUE;
			i = 0;
		} else {
			i++;
		}
	}
	/* Rehash to a size that can fit the current elements */
	printf ("Size: %d Table Size: %d Max Chain Length: %d\n", hash->in_use, mono_array_length_fast (hash->table), max_chain_size);
}

#ifdef HAVE_SGEN_GC

/* GC marker function */
static void
mono_g_hash_mark (void *addr, MonoGCMarkFunc mark_func, void *gc_data)
{
	MonoGHashTable *hash = (MonoGHashTable*)addr;

	mark_func ((MonoObject**)&hash->table, gc_data);
	if (hash->old_table)
		mark_func ((MonoObject**)&hash->old_table, gc_data);
}

#endif
