/*
 * gmem.c: memory utility functions
 *
 * Author:
 * 	Gonzalo Paniagua Javier (gonzalo@novell.com)
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
#include <string.h>
#include <glib.h>
#include <eglib-remap.h> // Remove the cast macros and restore the rename macros.
#undef malloc
#undef realloc
#undef free
#undef calloc

#include <malloc.h>
#include <execinfo.h>

static char*
get_parent ()
{
        int nptrs;
        void *buffer[3];
        char **strings;
	char *ret;

        nptrs = backtrace (buffer, 3);
        strings = backtrace_symbols(buffer, nptrs);

	ret = strings [2];
	free (strings);
	return ret;
}

#if defined (ENABLE_OVERRIDABLE_ALLOCATORS)

static GMemVTable sGMemVTable = { malloc, realloc, free, calloc };

void
g_mem_set_vtable (GMemVTable* vtable)
{
	sGMemVTable.calloc = vtable->calloc ? vtable->calloc : calloc;
	sGMemVTable.realloc = vtable->realloc ? vtable->realloc : realloc;
	sGMemVTable.malloc = vtable->malloc ? vtable->malloc : malloc;
	sGMemVTable.free = vtable->free ? vtable->free : free;
}

void
g_mem_get_vtable (GMemVTable* vtable)
{
	*vtable = sGMemVTable;
}

#define G_FREE_INTERNAL sGMemVTable.free
#define G_REALLOC_INTERNAL sGMemVTable.realloc
#define G_CALLOC_INTERNAL sGMemVTable.calloc
#define G_MALLOC_INTERNAL sGMemVTable.malloc
#else

void
g_mem_get_vtable (GMemVTable* vtable)
{
	memset (vtable, 0, sizeof (*vtable));
}

void
g_mem_set_vtable (GMemVTable* vtable)
{
}

typedef struct {
	const char *filename;
	gint64 balance;
	gint64 runtime_balance;
} MallocEntry;

MallocEntry malloc_entries [2048];
int num_malloc_entries = 0;

size_t verbose_malloc_memory = 0;

static int comparer (const void *a, const void *b)
{
	MallocEntry *m1 = (MallocEntry*)a;
	MallocEntry *m2 = (MallocEntry*)b;

	if (m1->balance > m2->balance)
		return 1;
	else if (m1->balance == m2->balance)
		return 0;
	else
		return -1;
}

static int runtime_comparer (const void *a, const void *b)
{
	MallocEntry *m1 = (MallocEntry*)a;
	MallocEntry *m2 = (MallocEntry*)b;

	if (m1->runtime_balance > m2->runtime_balance)
		return 1;
	else if (m1->runtime_balance == m2->runtime_balance)
		return 0;
	else
		return -1;
}

typedef struct {
	gpointer ptr;
	const char *name;
	int size;
} AllocEntry;

#define ALLOC_ENTRIES_SIZE	10000000
long allocated_entries = 0;
AllocEntry entries [ALLOC_ENTRIES_SIZE];

static guint
mono_aligned_addr_hash (gconstpointer ptr)
{
        /* Same hashing we use for objects */
        return (GPOINTER_TO_UINT (ptr) >> 3) * 2654435761u;
}

static void report_balance (const char *name, int balance, gpointer ptr)
{
	int i;

	if (ptr == NULL || balance == 0)
		return;

	int start_index = mono_aligned_addr_hash (ptr) % ALLOC_ENTRIES_SIZE;
	for (i = 0; i < ALLOC_ENTRIES_SIZE; i++) {
		int index = (start_index + i) % ALLOC_ENTRIES_SIZE;
		if (balance > 0) {
			/* Add, looking for empty entry */
			if (!entries [index].ptr) {
				entries [index].ptr = ptr;
				entries [index].name = name;
				entries [index].size = balance;
				allocated_entries ++;
				break;
			}
		} else {
			/* Remove, looking for entry with ptr */
			if (entries [index].ptr == ptr) {
				entries [index].ptr = NULL;
				entries [index].name = NULL;
				entries [index].size = 0;
				allocated_entries --;
				break;
			}
		}
	}
	g_assert (balance < 0 || i != ALLOC_ENTRIES_SIZE);

	for (i = 0; i < num_malloc_entries; i++) {
		if (strcmp (name, malloc_entries [i].filename) == 0) {
			malloc_entries [i].balance += balance;
			return;
		}
	}

	malloc_entries [num_malloc_entries].filename = name;
	malloc_entries [num_malloc_entries].balance = balance;
	num_malloc_entries++;
}

void
print_malloc_entries (gboolean full_logging)
{
	if (!full_logging) {
		qsort (malloc_entries, num_malloc_entries, sizeof (MallocEntry), comparer);
		gint64 total_balance = 0;
		for (int i = 0; i < num_malloc_entries; i++) {
			fprintf (stderr, "Entry %d, name %s, balance %ld\n", i, malloc_entries [i].filename, malloc_entries [i].balance);
			total_balance += malloc_entries [i].balance;
		}
		fprintf (stderr, "Total balance %ld\n", total_balance);
	} else {
		for (int i = 0; i < num_malloc_entries; i++) {
			malloc_entries [i].runtime_balance = 0;
			for (int j = 0; j < ALLOC_ENTRIES_SIZE; j++) {
				if (entries [j].ptr && strcmp (malloc_entries [i].filename, entries [j].name) == 0)
					malloc_entries [i].runtime_balance += entries [j].size;
			}
		}
		qsort (malloc_entries, num_malloc_entries, sizeof (MallocEntry), runtime_comparer);
		gint64 total_balance = 0;
		for (int i = 0; i < num_malloc_entries; i++) {
			fprintf (stderr, "Entry %d, name %s, runtime_balance %ld, total balance %ld\n", i, malloc_entries [i].filename, malloc_entries [i].runtime_balance, malloc_entries [i].balance);
			total_balance += malloc_entries [i].balance;
		}
		fprintf (stderr, "Total balance %ld\n", total_balance);
	}
}

#define G_FREE_INTERNAL(_ptr) do {	\
		size_t usable_size = malloc_usable_size (_ptr);	\
		report_balance (filename, -usable_size, _ptr);	\
		__sync_fetch_and_sub (&verbose_malloc_memory, usable_size); \
		free (_ptr);		\
	} while (0)
#define G_REALLOC_INTERNAL(_obj,_size,_ret) do {	\
		size_t usable_size = malloc_usable_size (_obj);	\
		report_balance (filename, -usable_size, _obj);	\
		__sync_fetch_and_sub (&verbose_malloc_memory, usable_size); \
		_ret = realloc (_obj, _size);		\
		usable_size = malloc_usable_size (_ret);	\
		report_balance (filename, usable_size, _ret);	\
		__sync_fetch_and_add (&verbose_malloc_memory, usable_size); \
	} while (0)
#define G_CALLOC_INTERNAL(_n,_s,_ret) do {	\
		_ret = calloc (_n, _s);		\
		size_t usable_size = malloc_usable_size (_ret);	\
		report_balance (filename, usable_size, _ret);	\
		__sync_fetch_and_add (&verbose_malloc_memory, usable_size); \
	} while (0)
#define G_MALLOC_INTERNAL(_size,_ret) do {	\
		_ret = malloc (_size);		\
		size_t usable_size = malloc_usable_size (_ret);	\
		report_balance (filename, usable_size, _ret);	\
		__sync_fetch_and_add (&verbose_malloc_memory, usable_size); \
	} while (0)

#endif

void
monoeg_free_verbose (void *ptr, const char *filename)
{
	if (ptr != NULL)
		G_FREE_INTERNAL (ptr);
}

void
monoeg_free (void *ptr)
{
	monoeg_free_verbose (ptr, __FILE__);//get_parent ());
}

gpointer
g_memdup (gconstpointer mem, guint byte_size)
{
	gpointer ptr;

	if (mem == NULL)
		return NULL;

	ptr = monoeg_malloc_verbose (byte_size, "memdup");
	if (ptr != NULL)
		memcpy (ptr, mem, byte_size);

	return ptr;
}

gpointer
monoeg_realloc_verbose (gpointer obj, gsize size, const char *filename)
{
	gpointer ptr;
	if (!size) {
		g_free (obj);
		return 0;
	}
	G_REALLOC_INTERNAL (obj, size, ptr);
	if (ptr)
		return ptr;
	g_error ("Could not allocate %i bytes", size);
}

gpointer
monoeg_realloc (gpointer obj, gsize size)
{
	return g_realloc (obj, size);
}

gpointer 
monoeg_malloc_verbose (gsize x, const char *filename)
{ 
	gpointer ptr;
	if (!x)
		return 0;
	G_MALLOC_INTERNAL (x, ptr);
	if (ptr)
		return ptr;
	g_error ("Could not allocate %i bytes", x);
}

gpointer
monoeg_malloc (gsize x)
{
	return g_malloc (x);
}

gpointer
monoeg_calloc_verbose (gsize n, gsize x, const char *filename)
{
	gpointer ptr;
	if (!x || !n)
		return 0;
	G_CALLOC_INTERNAL (n, x, ptr);
	if (ptr)
		return ptr;
	g_error ("Could not allocate %i (%i * %i) bytes", x*n, n, x);
}

gpointer
monoeg_calloc (gsize n, gsize x)
{
	return g_calloc (n, x);
}

gpointer
monoeg_malloc0_verbose (gsize x, const char *filename)
{ 
	return monoeg_calloc_verbose (1,x, filename);
}

gpointer
monoeg_try_malloc_verbose (gsize x, const char *filename)
{
	if (x) {
		gpointer ret;
		G_MALLOC_INTERNAL (x, ret);
		return ret;
	}
	return 0;
}

gpointer
monoeg_try_malloc (gsize x)
{
	return g_try_malloc (x);
}


gpointer
monoeg_try_realloc_verbose (gpointer obj, gsize size, const char *filename)
{
	gpointer ret;
	if (!size) {
		G_FREE_INTERNAL (obj);
		return 0;
	} 
	G_REALLOC_INTERNAL (obj, size, ret);
	return ret;
}

gpointer
monoeg_try_realloc (gpointer obj, gsize size)
{
	return g_try_realloc (obj, size);
}
