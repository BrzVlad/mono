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
#include <malloc.h>
#include <execinfo.h>

size_t total_malloc_memory = 0;
size_t total_alloced_memory = 0;
size_t total_freed_memory = 0;

size_t verbose_malloc_memory = 0;

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

#define G_FREE_INTERNAL sGMemVTable.free
#define G_REALLOC_INTERNAL sGMemVTable.realloc
#define G_CALLOC_INTERNAL sGMemVTable.calloc
#define G_MALLOC_INTERNAL sGMemVTable.malloc
#else

typedef struct {
	const char *filename;
	gint64 balance;
} MallocEntry;

MallocEntry malloc_entries [2048];
int num_malloc_entries = 0;

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

void print_malloc_entries (void)
{
	qsort (malloc_entries, num_malloc_entries, sizeof (MallocEntry), comparer);
	gint64 total_balance = 0;
	for (int i = 0; i < num_malloc_entries; i++) {
		fprintf (stderr, "Entry %d, name %s, balance %ld\n", i, malloc_entries [i].filename, malloc_entries [i].balance);
		total_balance += malloc_entries [i].balance;
	}

	fprintf (stderr, "Total balance %ld\n", total_balance);
}

void
g_mem_set_vtable (GMemVTable* vtable)
{
}

static void report_balance (const char *name, int balance)
{
	for (int i = 0; i < num_malloc_entries; i++) {
		if (strcmp (name, malloc_entries [i].filename) == 0) {
			malloc_entries [i].balance += balance;
			return;
		}
	}
	malloc_entries [num_malloc_entries].filename = name;
	malloc_entries [num_malloc_entries].balance = balance;
	num_malloc_entries++;
}

#define G_FREE_INTERNAL(_ptr) do {	\
		size_t usable_size = malloc_usable_size (_ptr);	\
		report_balance (filename, -usable_size);	\
		__sync_fetch_and_sub (&verbose_malloc_memory, usable_size); \
		free (_ptr);		\
	} while (0)
#define G_REALLOC_INTERNAL(_obj,_size,_ret) do {	\
		size_t usable_size = malloc_usable_size (_obj);	\
		report_balance (filename, -usable_size);	\
		__sync_fetch_and_sub (&verbose_malloc_memory, usable_size); \
		_ret = realloc (_obj, _size);		\
		usable_size = malloc_usable_size (_ret);	\
		report_balance (filename, usable_size);	\
		__sync_fetch_and_add (&verbose_malloc_memory, usable_size); \
	} while (0)
#define G_CALLOC_INTERNAL(_n,_s,_ret) do {	\
		_ret = calloc (_n, _s);		\
		size_t usable_size = malloc_usable_size (_ret);	\
		report_balance (filename, usable_size);	\
		__sync_fetch_and_add (&verbose_malloc_memory, usable_size); \
	} while (0)
#define G_MALLOC_INTERNAL(_size,_ret) do {	\
		_ret = malloc (_size);		\
		size_t usable_size = malloc_usable_size (_ret);	\
		report_balance (filename, usable_size);	\
		__sync_fetch_and_add (&verbose_malloc_memory, usable_size); \
	} while (0)
#endif

void
g_free (void *ptr)
{
	const char *filename = "gmem.c:free";
	__sync_fetch_and_sub (&total_malloc_memory, malloc_usable_size (ptr));
	__sync_fetch_and_add (&total_freed_memory, malloc_usable_size (ptr));

	if (ptr != NULL)
		G_FREE_INTERNAL (ptr);
}

void
g_free_verbose (void *ptr, const char *filename)
{
	__sync_fetch_and_sub (&total_malloc_memory, malloc_usable_size (ptr));
	__sync_fetch_and_add (&total_freed_memory, malloc_usable_size (ptr));

	if (ptr != NULL)
		G_FREE_INTERNAL (ptr);
}

gpointer
g_memdup (gconstpointer mem, guint byte_size)
{
	gpointer ptr;

	if (mem == NULL)
		return NULL;

	ptr = g_malloc_verbose (byte_size, "gmem.c:memdup");
	if (ptr != NULL)
		memcpy (ptr, mem, byte_size);

	return ptr;
}

gpointer g_realloc (gpointer obj, gsize size)
{
	gpointer ptr;
	const char *filename = "gmem.c:realloc";
	if (!size) {
		g_free (obj);
		return 0;
	}
	__sync_fetch_and_sub (&total_malloc_memory, malloc_usable_size (obj));
	__sync_fetch_and_add (&total_freed_memory, malloc_usable_size (obj));
	G_REALLOC_INTERNAL (obj, size, ptr);
	__sync_fetch_and_add (&total_malloc_memory, malloc_usable_size (ptr));
	__sync_fetch_and_add (&total_alloced_memory, malloc_usable_size (ptr));
	if (ptr)
		return ptr;
	g_error ("Could not allocate %i bytes", size);
}

gpointer 
g_malloc (gsize x) 
{
	const char *filename = "gmem.c:malloc"; 
	gpointer ptr;
	if (!x)
		return 0;
	G_MALLOC_INTERNAL (x, ptr);
	__sync_fetch_and_add (&total_malloc_memory, malloc_usable_size (ptr));
	__sync_fetch_and_add (&total_alloced_memory, malloc_usable_size (ptr));
	if (ptr) 
		return ptr;
	g_error ("Could not allocate %i bytes", x);
}

gpointer
g_malloc_verbose (gsize x, const char *filename)
{
	gpointer ptr;
	if (!x)
		return 0;
	G_MALLOC_INTERNAL (x, ptr);
	__sync_fetch_and_add (&total_malloc_memory, malloc_usable_size (ptr));
	__sync_fetch_and_add (&total_alloced_memory, malloc_usable_size (ptr));
	if (ptr) 
		return ptr;
	g_error ("Could not allocate %i bytes", x);

	return ptr;
}

gpointer g_calloc (gsize n, gsize x)
{
	gpointer ptr;
	const char *filename = "gmem.c:calloc";
	if (!x || !n)
		return 0;
	G_CALLOC_INTERNAL (n, x, ptr);
	__sync_fetch_and_add (&total_malloc_memory, malloc_usable_size (ptr));
	__sync_fetch_and_add (&total_alloced_memory, malloc_usable_size (ptr));
	if (ptr)
		return ptr;
	g_error ("Could not allocate %i (%i * %i) bytes", x*n, n, x);
}

gpointer g_calloc_verbose (gsize n, gsize x, const char *filename)
{
	gpointer ptr;
	if (!x || !n)
		return 0;
	G_CALLOC_INTERNAL (n, x, ptr);
	__sync_fetch_and_add (&total_malloc_memory, malloc_usable_size (ptr));
	__sync_fetch_and_add (&total_alloced_memory, malloc_usable_size (ptr));
	if (ptr)
		return ptr;
	g_error ("Could not allocate %i (%i * %i) bytes", x*n, n, x);
}

gpointer g_malloc0 (gsize x) 
{ 
	return g_calloc (1,x);
}

gpointer g_malloc0_verbose (gsize x, const char *filename) 
{ 
	return g_calloc_verbose (1,x, filename);
}

gpointer g_try_malloc (gsize x) 
{
	if (x) {
		const char *filename = "gmem.c:try_malloc";
		gpointer ptr;
		G_MALLOC_INTERNAL (x, ptr);
		__sync_fetch_and_add (&total_malloc_memory, malloc_usable_size (ptr));
		__sync_fetch_and_add (&total_alloced_memory, malloc_usable_size (ptr));
		return ptr;
	}
	return 0;
}

gpointer g_try_malloc_verbose (gsize x, const char *filename) 
{
	if (x) {
		gpointer ptr;
		G_MALLOC_INTERNAL (x, ptr);
		__sync_fetch_and_add (&total_malloc_memory, malloc_usable_size (ptr));
		__sync_fetch_and_add (&total_alloced_memory, malloc_usable_size (ptr));
		return ptr;
	}
	return 0;
}


gpointer g_try_realloc (gpointer obj, gsize size)
{ 
	const char *filename = "gmem.c:try_realloc";
	if (!size) {
		__sync_fetch_and_sub (&total_malloc_memory, malloc_usable_size (obj));
		__sync_fetch_and_add (&total_freed_memory, malloc_usable_size (obj));
		G_FREE_INTERNAL (obj);
		return 0;
	}
	gpointer ptr;
	__sync_fetch_and_sub (&total_malloc_memory, malloc_usable_size (obj));
	__sync_fetch_and_add (&total_freed_memory, malloc_usable_size (obj));
	G_REALLOC_INTERNAL (obj, size, ptr);
	__sync_fetch_and_add (&total_malloc_memory, malloc_usable_size (ptr));
	__sync_fetch_and_add (&total_alloced_memory, malloc_usable_size (ptr));
	return ptr;
}
