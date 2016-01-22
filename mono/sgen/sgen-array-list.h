/*
 * sgen-array-list.h: A pointer array that doesn't use reallocs.
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

#ifndef __MONO_SGEN_ARRAY_LIST_H__
#define __MONO_SGEN_ARRAY_LIST_H__

#include <glib.h>

#define SGEN_ARRAY_LIST_BUCKETS (32)
#define SGEN_ARRAY_LIST_MIN_BUCKET_BITS (5)
#define SGEN_ARRAY_LIST_MIN_BUCKET_SIZE (1 << SGEN_ARRAY_LIST_MIN_BUCKET_BITS)

/*
 * 'entries' is an array of pointers to buckets of increasing size. The first
 * bucket has size 'MIN_BUCKET_SIZE', and each bucket is twice the size of the
 * previous, i.e.:
 *
 *           |-------|-- MIN_BUCKET_SIZE
 *    [0] -> xxxxxxxx
 *    [1] -> xxxxxxxxxxxxxxxx
 *    [2] -> xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
 *    ...
 *
 * 'slot_hint' denotes the position of the last allocation, so that the
 * whole array needn't be searched on every allocation.
 */

typedef struct {
        volatile gpointer *volatile entries [SGEN_ARRAY_LIST_BUCKETS];
        volatile guint32 capacity;
        volatile guint32 slot_hint;
        volatile guint32 max_index;
	int mem_type;
} SgenArrayList;

/*
 * Computes floor(log2(index + MIN_BUCKET_SIZE)) - 1, giving the index
 * of the bucket containing a slot.
 */
static inline guint32
sgen_array_list_index_bucket (guint32 index)
{
#ifdef __GNUC__
	return CHAR_BIT * sizeof (index) - __builtin_clz (index + SGEN_ARRAY_LIST_MIN_BUCKET_SIZE) - 1 - SGEN_ARRAY_LIST_MIN_BUCKET_BITS;
#else
	guint count = 0;
	index += SGEN_ARRAY_LIST_MIN_BUCKET_SIZE;
	while (index) {
		++count;
		index >>= 1;
	}
	return count - 1 - SGEN_ARRAY_LIST_MIN_BUCKET_BITS;
#endif
}

static inline guint32
sgen_array_list_bucket_size (guint32 index)
{
	return 1 << (index + SGEN_ARRAY_LIST_MIN_BUCKET_BITS);
}

static inline void
sgen_array_list_bucketize (guint32 index, guint32 *bucket, guint32 *offset)
{
	*bucket = sgen_array_list_index_bucket (index);
	*offset = index - sgen_array_list_bucket_size (*bucket) + SGEN_ARRAY_LIST_MIN_BUCKET_SIZE;
}

static inline volatile gpointer *
sgen_array_list_get_slot_address (SgenArrayList *array, guint32 index)
{
	guint32 bucket, offset;

	SGEN_ASSERT (0, index < array->capacity, "Why are we accessing an entry that is not allocated");

	sgen_array_list_bucketize (index, &bucket, &offset);
	return &(array->entries [bucket] [offset]);
}

#define SGEN_ARRAY_LIST_INIT(mem_type)	{ { NULL }, 0, 0, -1, (mem_type) }

#define SGEN_ARRAY_LIST_FOREACH_ENTRY(array, entry) {			\
	guint32 __bucket, __offset;					\
	const guint32 __max_bucket = sgen_array_list_index_bucket ((array)->capacity); \
	guint32 __index = 0;						\
	const guint32 __max_index = (array)->max_index;			\
	for (__bucket = 0; __bucket < __max_bucket; ++__bucket) {	\
		volatile gpointer *__entries = (array)->entries [__bucket]; \
		for (__offset = 0; __offset < sgen_array_list_bucket_size (__bucket); ++__offset, ++__index) { \
			if (__index > __max_index)			\
				break;					\
			entry = &__entries [__offset];

#define SGEN_ARRAY_LIST_END_FOREACH_ENTRY	} } }

#define SGEN_ARRAY_LIST_FOREACH_ENTRY_RANGE(array, begin, end, entry, index) {	\
	for (index = begin; index < end; index++) {		\
		guint32 __bucket, __offset;				\
		volatile gpointer *__entries;				\
		sgen_array_list_bucketize (index, &__bucket, &__offset); \
		__entries = (array)->entries [__bucket];		\
		entry = &__entries [__offset];

#define SGEN_ARRAY_LIST_END_FOREACH_ENTRY_RANGE	} }

#endif
