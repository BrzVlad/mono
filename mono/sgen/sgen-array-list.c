/*
 * sgen-array-list.c: A pointer array list that doesn't require reallocs
 *
 * Copyright (C) 2016 Xamarin Inc
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

#ifdef HAVE_SGEN_GC

#include <string.h>

#include "mono/sgen/sgen-gc.h"
#include "mono/sgen/sgen-array-list.h"

static void
sgen_array_list_grow (SgenArrayList *array)
{
	const size_t old_capacity = array->capacity;
	const size_t new_bucket = sgen_array_list_index_bucket (old_capacity);
	const size_t growth = sgen_array_list_bucket_size (new_bucket);
	const size_t new_capacity = old_capacity + growth;

	array->entries [new_bucket] = (gpointer*) sgen_alloc_internal_dynamic (sizeof (gpointer) * growth, array->mem_type, TRUE);

	mono_memory_write_barrier ();
	array->capacity = new_capacity;
}


void
sgen_array_list_add (SgenArrayList *array, gpointer ptr)
{
	size_t bucket, offset;
	if ((array->max_index + 1) >= array->capacity)
		sgen_array_list_grow (array);

	sgen_array_list_bucketize (array->max_index + 1, &bucket, &offset);
	array->entries [bucket][offset] = ptr;

	mono_memory_write_barrier ();
	array->max_index++;
}

/*
 * Removes all NULL pointers from the array.
 */
void
sgen_array_list_remove_nulls (SgenArrayList *array)
{
	size_t start = 0;
	volatile gpointer *entry;

	SGEN_ARRAY_LIST_FOREACH_ENTRY (array, entry) {
		if (*entry)
			*sgen_array_list_get_slot_address (array, start++) = *entry;
	} SGEN_ARRAY_LIST_END_FOREACH_ENTRY;

	mono_memory_write_barrier ();
	array->max_index = start - 1;
}

/*
 * Does a linear search through the pointer array to find `ptr`.  Returns the index if
 * found, otherwise (size_t)-1.
 */
size_t
sgen_array_list_find (SgenArrayList *array, gpointer ptr)
{
	volatile gpointer *entry;

	SGEN_ARRAY_LIST_FOREACH_ENTRY (array, entry) {
		if (*entry == ptr)
			return __index;
	} SGEN_ARRAY_LIST_END_FOREACH_ENTRY;
	return (size_t)-1;
}

#endif
