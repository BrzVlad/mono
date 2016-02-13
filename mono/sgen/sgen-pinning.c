/*
 * sgen-pinning.c: The pin queue.
 *
 * Copyright 2001-2003 Ximian, Inc
 * Copyright 2003-2010 Novell, Inc.
 * Copyright (C) 2012 Xamarin Inc
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License 2.0 as published by the Free Software Foundation;
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License 2.0 along with this library; if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "config.h"
#ifdef HAVE_SGEN_GC

#include <string.h>

#include "mono/sgen/sgen-gc.h"
#include "mono/sgen/sgen-pinning.h"
#include "mono/sgen/sgen-protocol.h"
#include "mono/sgen/sgen-pointer-queue.h"
#include "mono/sgen/sgen-client.h"

static SgenPointerQueue pin_queue, forced_pin_queue;
static size_t last_num_pinned = 0;
static MonoCoopMutex pin_queue_mutex;
static gboolean forced_cement_enabled = TRUE;
static gboolean forced_pin = FALSE;

#define PIN_HASH_SIZE 1024
static void *pin_hash_filter [PIN_HASH_SIZE];

void
sgen_init_pinning (int generation)
{
	if (forced_cement_enabled && generation == GENERATION_NURSERY)
		mono_coop_mutex_lock (&pin_queue_mutex);
	memset (pin_hash_filter, 0, sizeof (pin_hash_filter));
	pin_queue.mem_type = INTERNAL_MEM_PIN_QUEUE;
	if (forced_pin)
		sgen_pointer_queue_addq (&pin_queue, &forced_pin_queue);
	else
		sgen_pointer_queue_clear (&forced_pin_queue);
}

void
sgen_finish_pinning (int generation)
{
	last_num_pinned = pin_queue.next_slot;
	sgen_pointer_queue_clear (&pin_queue);
	if (generation == GENERATION_OLD && forced_pin) {
		sgen_pointer_queue_clear (&forced_pin_queue);
		forced_pin = FALSE;
	}
	if (forced_cement_enabled && generation == GENERATION_NURSERY)
		mono_coop_mutex_unlock (&pin_queue_mutex);
}

void
sgen_pin_stage_ptr (void *ptr)
{
	/*very simple multiplicative hash function, tons better than simple and'ng */ 
	int hash_idx = ((mword)ptr * 1737350767) & (PIN_HASH_SIZE - 1);
	if (pin_hash_filter [hash_idx] == ptr)
		return;

	pin_hash_filter [hash_idx] = ptr;

	sgen_pointer_queue_add (&pin_queue, ptr);
}

gboolean
sgen_find_optimized_pin_queue_area (void *start, void *end, size_t *first_out, size_t *last_out)
{
	size_t first = sgen_pointer_queue_search (&pin_queue, start);
	size_t last = sgen_pointer_queue_search (&pin_queue, end);
	SGEN_ASSERT (0, last == pin_queue.next_slot || pin_queue.data [last] >= end, "Pin queue search gone awry");
	*first_out = first;
	*last_out = last;
	return first != last;
}

void**
sgen_pinning_get_entry (size_t index)
{
	SGEN_ASSERT (0, index <= pin_queue.next_slot, "Pin queue entry out of range");
	return &pin_queue.data [index];
}

void
sgen_find_section_pin_queue_start_end (GCMemSection *section)
{
	SGEN_LOG (6, "Pinning from section %p (%p-%p)", section, section->data, section->end_data);

	sgen_find_optimized_pin_queue_area (section->data, section->end_data,
			&section->pin_queue_first_entry, &section->pin_queue_last_entry);

	SGEN_LOG (6, "Found %zd pinning addresses in section %p",
			section->pin_queue_last_entry - section->pin_queue_first_entry, section);
}

/*This will setup the given section for the while pin queue. */
void
sgen_pinning_setup_section (GCMemSection *section)
{
	section->pin_queue_first_entry = 0;
	section->pin_queue_last_entry = pin_queue.next_slot;
}

void
sgen_pinning_trim_queue_to_section (GCMemSection *section)
{
	SGEN_ASSERT (0, section->pin_queue_first_entry == 0, "Pin queue trimming assumes the whole pin queue is used by the nursery");
	pin_queue.next_slot = section->pin_queue_last_entry;
}


/*
 * We freeze the force_pinned queue and all the objects in the queue will
 * remain pinned untile the end of the next major collection. This is used
 * with concurrent collections.
 */
void
sgen_pinning_force_pinned (void)
{
	mono_coop_mutex_lock (&pin_queue_mutex);
	forced_pin = TRUE;
	mono_coop_mutex_unlock (&pin_queue_mutex);
}

void
sgen_pinning_register_pinned_in_nursery (GCObject *obj)
{
	if (!forced_pin)
		sgen_pointer_queue_add (&forced_pin_queue, obj);
}

void
sgen_pinning_scan_forced (ScanCopyContext ctx)
{
	ScanObjectFunc scan_func = ctx.ops->scan_object;
	int i;

	SGEN_ASSERT (0, forced_pin, "The set of forced pinned objects must be freezed before scanning");

	mono_coop_mutex_lock (&pin_queue_mutex);
	for (i = 0; i < forced_pin_queue.next_slot; ++i) {
		GCObject *obj = (GCObject *)forced_pin_queue.data [i];
		scan_func (obj, sgen_obj_get_descriptor_safe (obj), ctx.queue, NULL);
	}
	mono_coop_mutex_unlock (&pin_queue_mutex);
}

/*
 * This is called when we've run out of memory during a major collection.
 *
 * After collecting potential pin entries and sorting the array, this is what it looks like:
 *
 * +--------------------+---------------------------------------------+--------------------+
 * | major heap entries |               nursery entries               | major heap entries |
 * +--------------------+---------------------------------------------+--------------------+
 *
 * Of course there might not be major heap entries before and/or after the nursery entries,
 * depending on where the major heap sections are in the address space, and whether there
 * were any potential pointers there.
 *
 * When we pin nursery objects, we compact the nursery part of the pin array, which leaves
 * discarded entries after the ones that actually pointed to nursery objects:
 *
 * +--------------------+-----------------+---------------------------+--------------------+
 * | major heap entries | nursery entries | discarded nursery entries | major heap entries |
 * +--------------------+-----------------+---------------------------+--------------------+
 *
 * When, due to being out of memory, we late pin more objects, the pin array looks like
 * this:
 *
 * +--------------------+-----------------+---------------------------+--------------------+--------------+
 * | major heap entries | nursery entries | discarded nursery entries | major heap entries | late entries |
 * +--------------------+-----------------+---------------------------+--------------------+--------------+
 *
 * This function gets rid of the discarded nursery entries by nulling them out.  Note that
 * we can late pin objects not only in the nursery but also in the major heap, which happens
 * when evacuation fails.
 */
void
sgen_pin_queue_clear_discarded_entries (GCMemSection *section, size_t max_pin_slot)
{
	void **start = sgen_pinning_get_entry (section->pin_queue_last_entry);
	void **end = sgen_pinning_get_entry (max_pin_slot);
	void *addr;

	for (; start < end; ++start) {
		addr = *start;
		if ((char*)addr < section->data || (char*)addr > section->end_data)
			break;
		*start = NULL;
	}
}

/* reduce the info in the pin queue, removing duplicate pointers and sorting them */
void
sgen_optimize_pin_queue (void)
{
	sgen_pointer_queue_sort_uniq (&pin_queue);
}

size_t
sgen_get_pinned_count (void)
{
	return pin_queue.next_slot;
}

void
sgen_dump_pin_queue (void)
{
	int i;

	for (i = 0; i < last_num_pinned; ++i) {
		GCObject *ptr = (GCObject *)pin_queue.data [i];
		SGEN_LOG (3, "Bastard pinning obj %p (%s), size: %zd", ptr, sgen_client_vtable_get_name (SGEN_LOAD_VTABLE (ptr)), sgen_safe_object_get_size (ptr));
	}
}

typedef struct _CementHashEntry CementHashEntry;
struct _CementHashEntry {
	GCObject *obj;
	unsigned int count;
	gboolean forced;
};

static CementHashEntry cement_hash [SGEN_CEMENT_HASH_SIZE];

static gboolean cement_enabled = TRUE;

void
sgen_cement_init (gboolean enabled, gboolean forced_enabled)
{
	cement_enabled = enabled;
	if (!cement_enabled)
		forced_cement_enabled = FALSE;
	else
		forced_cement_enabled = forced_enabled;

	if (forced_cement_enabled)
		mono_coop_mutex_init (&pin_queue_mutex);
}

void
sgen_cement_reset (void)
{
	int i;
	for (i = 0; i < SGEN_CEMENT_HASH_SIZE; i++) {
		if (cement_hash [i].forced) {
			cement_hash [i].forced = FALSE;
		} else {
			cement_hash [i].obj = NULL;
			cement_hash [i].count = 0;
		}
	}
	binary_protocol_cement_reset ();
}


/*
 * Objects that were pinned at the time of the last collection and present
 * in the cement hash will be marked as forced. This means they will remain
 * cemented even after we reset the hash, losing the forced flag. The pin
 * queue is not allowed to change while we call this function.
 */
void
sgen_cement_force_pinned (void)
{
	int i;

	if (!forced_cement_enabled)
		return;

	/* If we don't lock on the pin queue we can miss some pinned objects */
	mono_coop_mutex_lock (&pin_queue_mutex);
	for (i = 0; i < last_num_pinned; ++i) {
		GCObject *obj = (GCObject *)pin_queue.data [i];
		guint hv = sgen_aligned_addr_hash (obj);
		int k = SGEN_CEMENT_HASH (hv);

		if (!cement_hash [k].obj)
			continue;
		if (cement_hash [k].obj != obj)
			continue;
		if (cement_hash [k].count < SGEN_CEMENT_THRESHOLD)
			continue;
		cement_hash [k].forced = TRUE;
	}
	mono_coop_mutex_unlock (&pin_queue_mutex);
}

void
sgen_cement_scan_forced (ScanCopyContext ctx)
{
	ScanObjectFunc scan_func = ctx.ops->scan_object;
	int i;

	if (!forced_cement_enabled)
		return;

	for (i = 0; i < SGEN_CEMENT_HASH_SIZE; ++i) {
		if (!cement_hash [i].forced)
			continue;
		scan_func (cement_hash [i].obj, sgen_obj_get_descriptor_safe (cement_hash [i].obj), ctx.queue, NULL);
	}
}

gboolean
sgen_cement_is_forced (GCObject *obj)
{
	guint hv = sgen_aligned_addr_hash (obj);
	int i = SGEN_CEMENT_HASH (hv);

	SGEN_ASSERT (5, sgen_ptr_in_nursery (obj), "Looking up cementing for non-nursery objects makes no sense");

	if (!forced_cement_enabled)
		return FALSE;

	if (!cement_hash [i].obj)
		return FALSE;
	if (cement_hash [i].obj != obj)
		return FALSE;

	return cement_hash [i].forced;
}

gboolean
sgen_cement_lookup (GCObject *obj)
{
	guint hv = sgen_aligned_addr_hash (obj);
	int i = SGEN_CEMENT_HASH (hv);

	SGEN_ASSERT (5, sgen_ptr_in_nursery (obj), "Looking up cementing for non-nursery objects makes no sense");

	if (!cement_enabled)
		return FALSE;

	if (!cement_hash [i].obj)
		return FALSE;
	if (cement_hash [i].obj != obj)
		return FALSE;

	return cement_hash [i].count >= SGEN_CEMENT_THRESHOLD;
}

gboolean
sgen_cement_lookup_or_register (GCObject *obj)
{
	guint hv;
	int i;
	CementHashEntry *hash = cement_hash;

	if (!cement_enabled)
		return FALSE;

	hv = sgen_aligned_addr_hash (obj);
	i = SGEN_CEMENT_HASH (hv);

	SGEN_ASSERT (5, sgen_ptr_in_nursery (obj), "Can only cement pointers to nursery objects");

	if (!hash [i].obj) {
		SGEN_ASSERT (5, !hash [i].count, "Cementing hash inconsistent");
		hash [i].obj = obj;
	} else if (hash [i].obj != obj) {
		return FALSE;
	}

	if (hash [i].count >= SGEN_CEMENT_THRESHOLD)
		return TRUE;

	++hash [i].count;
	if (hash [i].count == SGEN_CEMENT_THRESHOLD) {
		SGEN_ASSERT (9, sgen_get_current_collection_generation () >= 0, "We can only cement objects when we're in a collection pause.");
		SGEN_ASSERT (9, SGEN_OBJECT_IS_PINNED (obj), "Can only cement pinned objects");
		SGEN_CEMENT_OBJECT (obj);

		binary_protocol_cement (obj, (gpointer)SGEN_LOAD_VTABLE (obj),
				(int)sgen_safe_object_get_size (obj));
	}

	return FALSE;
}

static void
pin_from_hash (CementHashEntry *hash, gboolean has_been_reset)
{
	int i;
	for (i = 0; i < SGEN_CEMENT_HASH_SIZE; ++i) {
		if (!hash [i].count)
			continue;

		if (has_been_reset)
			SGEN_ASSERT (5, hash [i].count >= SGEN_CEMENT_THRESHOLD, "Cementing hash inconsistent");

		sgen_pin_stage_ptr (hash [i].obj);
		binary_protocol_cement_stage (hash [i].obj);
		/* FIXME: do pin stats if enabled */

		SGEN_CEMENT_OBJECT (hash [i].obj);
	}
}

void
sgen_pin_cemented_objects (void)
{
	pin_from_hash (cement_hash, TRUE);
}

void
sgen_cement_clear_below_threshold (void)
{
	int i;
	for (i = 0; i < SGEN_CEMENT_HASH_SIZE; ++i) {
		if (cement_hash [i].count < SGEN_CEMENT_THRESHOLD) {
			cement_hash [i].obj = NULL;
			cement_hash [i].count = 0;
		}
	}
}

#endif /* HAVE_SGEN_GC */
