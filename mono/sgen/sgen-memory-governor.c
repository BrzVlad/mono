/*
 * sgen-memory-governor.c: When to schedule collections based on
 * memory usage.
 *
 * Author:
 * 	Rodrigo Kumpera (rkumpera@novell.com)
 *
 * Copyright 2001-2003 Ximian, Inc
 * Copyright 2003-2010 Novell, Inc.
 * Copyright 2011 Xamarin Inc (http://www.xamarin.com)
 * Copyright (C) 2012 Xamarin Inc
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include "config.h"
#ifdef HAVE_SGEN_GC

#include <stdlib.h>

#include "mono/sgen/sgen-gc.h"
#include "mono/sgen/sgen-memory-governor.h"
#include "mono/sgen/sgen-thread-pool.h"
#include "mono/sgen/sgen-client.h"

#define MIN_MINOR_COLLECTION_ALLOWANCE	((mword)(DEFAULT_NURSERY_SIZE * default_allowance_nursery_size_ratio))

static SgenPointerQueue log_entries = SGEN_POINTER_QUEUE_INIT (INTERNAL_MEM_TEMPORARY);
static MonoCoopMutex log_entries_mutex;

mword total_promoted_size = 0;
mword total_allocated_major = 0;
static mword total_promoted_size_start;
static mword total_allocated_major_end;

/*Heap limits and allocation knobs*/
static mword max_heap_size = ((mword)0)- ((mword)1);
static mword soft_heap_limit = ((mword)0) - ((mword)1);

static double default_allowance_nursery_size_ratio = SGEN_DEFAULT_ALLOWANCE_NURSERY_SIZE_RATIO;
static double save_target_ratio = SGEN_DEFAULT_SAVE_TARGET_RATIO;

/**/
static mword allocated_heap;
static mword total_alloc = 0;
static mword total_alloc_max = 0;

static SGEN_TV_DECLARE(last_minor_start);
static SGEN_TV_DECLARE(last_major_start);

/* GC triggers. */

static gboolean debug_print_allowance = FALSE;


/* use this to tune when to do a major/minor collection */
static mword major_collection_trigger_size;

static mword major_pre_sweep_heap_size;
static mword major_start_heap_size;

static gboolean need_calculate_minor_collection_allowance;

/* The size of the LOS after the last major collection, after sweeping. */
static mword last_collection_los_memory_usage = 0;
static mword last_used_slots_size = 0;

static mword sgen_memgov_available_free_space (void);


/* GC trigger heuristics. */

static void
sgen_memgov_calculate_minor_collection_allowance (void)
{
	size_t new_major, new_heap_size, allowance_target, allowance;
	size_t decrease;

	if (!need_calculate_minor_collection_allowance)
		return;

	SGEN_ASSERT (0, major_collector.have_swept (), "Can only calculate allowance if heap is swept");

	new_major = major_collector.get_bytes_survived_last_sweep ();
	new_heap_size = new_major + last_collection_los_memory_usage;

	/*
	 * We allow the heap to grow by one third its current size before we start the next
	 * major collection.
	 */
	allowance_target = new_heap_size * SGEN_DEFAULT_ALLOWANCE_HEAP_SIZE_RATIO;

	allowance = MAX (allowance_target, MIN_MINOR_COLLECTION_ALLOWANCE);

	/*
	 * For the concurrent collector, we decrease the allowance relative to the memory
	 * growth during the M&S phase, survival rate of the collection and the allowance
	 * ratio.
	 */
	decrease = (major_pre_sweep_heap_size - major_start_heap_size) * ((float)new_heap_size / major_pre_sweep_heap_size) * (SGEN_DEFAULT_ALLOWANCE_HEAP_SIZE_RATIO + 1);
	if (decrease > allowance)
		decrease = allowance;
	allowance -= decrease;

	if (new_heap_size + allowance > soft_heap_limit) {
		if (new_heap_size > soft_heap_limit)
			allowance = MIN_MINOR_COLLECTION_ALLOWANCE;
		else
			allowance = MAX (soft_heap_limit - new_heap_size, MIN_MINOR_COLLECTION_ALLOWANCE);
	}

	/* FIXME: Why is this here? */
	if (major_collector.free_swept_blocks)
		major_collector.free_swept_blocks (major_collector.get_num_major_sections () * SGEN_DEFAULT_ALLOWANCE_HEAP_SIZE_RATIO);

	major_collection_trigger_size = new_heap_size + allowance;

	need_calculate_minor_collection_allowance = FALSE;

	if (debug_print_allowance) {
		SGEN_LOG (0, "Surviving sweep: %ld bytes (%ld major, %ld LOS)", (long)new_heap_size, (long)new_major, (long)last_collection_los_memory_usage);
		SGEN_LOG (0, "Allowance: %ld bytes", (long)allowance);
		SGEN_LOG (0, "Trigger size: %ld bytes", (long)major_collection_trigger_size);
	}
}

static inline size_t
get_heap_size (void)
{
	return major_collector.get_num_major_sections () * major_collector.section_size + los_memory_usage;
}

/*
 * If the heap exceeds the soft limit, we should try triggering compacting majors as often
 * as possible. These collections should also be serial, to avoid as much as possible further
 * heap growth and memory usage.
 */
gboolean
sgen_need_compacting_collection (void)
{
	return ((mword)get_heap_size ()) > soft_heap_limit;
}

gboolean
sgen_need_major_collection (mword space_needed)
{
	size_t heap_size;

	if (sgen_concurrent_collection_in_progress ()) {
		heap_size = get_heap_size ();

		if (heap_size <= major_collection_trigger_size)
			return FALSE; 

		/*
		 * The more the heap grows, the more we need to decrease the allowance above,
		 * in order to have similar trigger sizes as the synchronous collector.
		 * If the heap grows so much that we would need to have a negative allowance,
		 * we force the finishing of the collection, to avoid increased memory usage.
		 */
		if ((heap_size - major_start_heap_size) > major_start_heap_size * SGEN_DEFAULT_ALLOWANCE_HEAP_SIZE_RATIO)
			return TRUE;
		return FALSE;
	}

	/* FIXME: This is a cop-out.  We should have some way of figuring this out. */
	if (!major_collector.have_swept ())
		return FALSE;

	if (space_needed > sgen_memgov_available_free_space ())
		return TRUE;

	sgen_memgov_calculate_minor_collection_allowance ();

	heap_size = get_heap_size ();

	return heap_size > major_collection_trigger_size;
}

void
sgen_memgov_minor_collection_start (void)
{
	total_promoted_size_start = total_promoted_size;
	SGEN_TV_GETTIME (last_minor_start);
}

static void
sgen_add_log_entry (SgenLogEntry *log_entry)
{
	mono_coop_mutex_lock (&log_entries_mutex);
	sgen_pointer_queue_add (&log_entries, log_entry);
	mono_coop_mutex_unlock (&log_entries_mutex);
}

void
sgen_memgov_minor_collection_end (const char *reason)
{
	if (mono_trace_is_traced (G_LOG_LEVEL_INFO, MONO_TRACE_GC)) {
		SgenLogEntry *log_entry = (SgenLogEntry*)sgen_alloc_internal (INTERNAL_MEM_LOG_ENTRY);
		SGEN_TV_DECLARE (current_time);
		SGEN_TV_GETTIME (current_time);

		log_entry->type = SGEN_LOG_NURSERY;
		log_entry->reason = reason;
		log_entry->time = SGEN_TV_ELAPSED (last_minor_start, current_time);
		log_entry->promoted_size = total_promoted_size - total_promoted_size_start;
		log_entry->major_size = major_collector.get_num_major_sections () * major_collector.section_size;
		log_entry->major_size_in_use = last_used_slots_size + total_allocated_major - total_allocated_major_end;
		log_entry->los_size = los_memory_usage_total;
		log_entry->los_size_in_use = los_memory_usage;

		sgen_add_log_entry (log_entry);
	}
}

void
sgen_memgov_major_pre_sweep (void)
{
	if (sgen_concurrent_collection_in_progress ()) {
		major_pre_sweep_heap_size = get_heap_size ();
	} else {
		/* We decrease the allowance only in the concurrent case */
		major_pre_sweep_heap_size = major_start_heap_size;
	}
}

void
sgen_memgov_major_post_sweep (mword used_slots_size)
{
	if (mono_trace_is_traced (G_LOG_LEVEL_INFO, MONO_TRACE_GC)) {
		SgenLogEntry *log_entry = (SgenLogEntry*)sgen_alloc_internal (INTERNAL_MEM_LOG_ENTRY);

		log_entry->type = SGEN_LOG_MAJOR_SWEEP_FINISH;
		log_entry->major_size = major_collector.get_num_major_sections () * major_collector.section_size;
		log_entry->major_size_in_use = used_slots_size + total_allocated_major - total_allocated_major_end;

		sgen_add_log_entry (log_entry);
	}
	last_used_slots_size = used_slots_size;
}

void
sgen_memgov_major_collection_start (gboolean concurrent, const char *reason)
{
	need_calculate_minor_collection_allowance = TRUE;
	major_start_heap_size = get_heap_size ();

	if (debug_print_allowance) {
		SGEN_LOG (0, "Starting collection with heap size %ld bytes", (long)major_start_heap_size);
	}
	if (concurrent && mono_trace_is_traced (G_LOG_LEVEL_INFO, MONO_TRACE_GC)) {
		SgenLogEntry *log_entry = (SgenLogEntry*)sgen_alloc_internal (INTERNAL_MEM_LOG_ENTRY);

		log_entry->type = SGEN_LOG_MAJOR_CONC_START;
		log_entry->reason = reason;

		sgen_add_log_entry (log_entry);
	}
	SGEN_TV_GETTIME (last_major_start);
}

void
sgen_memgov_major_collection_end (gboolean forced, gboolean concurrent, const char *reason)
{
	if (mono_trace_is_traced (G_LOG_LEVEL_INFO, MONO_TRACE_GC)) {
		SgenLogEntry *log_entry = (SgenLogEntry*)sgen_alloc_internal (INTERNAL_MEM_LOG_ENTRY);
		SGEN_TV_DECLARE (current_time);
		SGEN_TV_GETTIME (current_time);

		if (concurrent) {
			log_entry->type = SGEN_LOG_MAJOR_CONC_FINISH;
		} else {
			log_entry->type = SGEN_LOG_MAJOR_SERIAL;
		}
		log_entry->time = SGEN_TV_ELAPSED (last_major_start, current_time);
		log_entry->reason = reason;
		log_entry->los_size = los_memory_usage_total;
		log_entry->los_size_in_use = los_memory_usage;

		sgen_add_log_entry (log_entry);
	}

	last_collection_los_memory_usage = los_memory_usage;
	total_allocated_major_end = total_allocated_major;
	if (forced) {
		sgen_get_major_collector ()->finish_sweeping ();
		sgen_memgov_calculate_minor_collection_allowance ();
	}
}

void
sgen_memgov_collection_start (int generation)
{
}

static void
sgen_output_log_entry (SgenLogEntry *entry, gint64 stw_time, int generation)
{
	char full_timing_buff [1024];
	full_timing_buff [0] = '\0';

	sprintf (full_timing_buff, "stw %.2fms", stw_time / 10000.0f);

	switch (entry->type) {
		case SGEN_LOG_NURSERY:
			mono_trace (G_LOG_LEVEL_INFO, MONO_TRACE_GC, "GC_MINOR: (%s) time %.2fms, %s promoted %dK major size: %dK in use: %dK los size: %dK in use: %dK",
				entry->reason ? entry->reason : "",
				entry->time / 10000.0f,
				(generation == GENERATION_NURSERY) ? full_timing_buff : "",
				entry->promoted_size / 1024,
				entry->major_size / 1024,
				entry->major_size_in_use / 1024,
				entry->los_size / 1024,
				entry->los_size_in_use / 1024);
			break;
		case SGEN_LOG_MAJOR_SERIAL:
			mono_trace (G_LOG_LEVEL_INFO, MONO_TRACE_GC, "GC_MAJOR: (%s) time %.2fms, %s los size: %dK in use: %dK",
				entry->reason ? entry->reason : "",
				(int)entry->time / 10000.0f,
				full_timing_buff,
				entry->los_size / 1024,
				entry->los_size_in_use / 1024);
			break;
		case SGEN_LOG_MAJOR_CONC_START:
			mono_trace (G_LOG_LEVEL_INFO, MONO_TRACE_GC, "GC_MAJOR_CONCURRENT_START: (%s)", entry->reason ? entry->reason : "");
			break;
		case SGEN_LOG_MAJOR_CONC_FINISH:
			mono_trace (G_LOG_LEVEL_INFO, MONO_TRACE_GC, "GC_MAJOR_CONCURRENT_FINISH: (%s) time %.2fms, %s los size: %dK in use: %dK",
				entry->reason ? entry->reason : "",
				entry->time / 10000.0f,
				full_timing_buff,
				entry->los_size / 1024,
				entry->los_size_in_use / 1024);
			break;
		case SGEN_LOG_MAJOR_SWEEP_FINISH:
			mono_trace (G_LOG_LEVEL_INFO, MONO_TRACE_GC, "GC_MAJOR_SWEEP: major size: %dK in use: %dK",
				entry->major_size / 1024,
				entry->major_size_in_use / 1024);
			break;
		default:
			SGEN_ASSERT (0, FALSE, "Invalid log entry type");
			break;
	}
}

void
sgen_memgov_collection_end (int generation, gint64 stw_time)
{
	/*
	 * At this moment the world has been restarted which means we can log all pending entries
	 * without risking deadlocks.
	 */
	if (mono_trace_is_traced (G_LOG_LEVEL_INFO, MONO_TRACE_GC)) {
		size_t i;
		SGEN_ASSERT (0, !sgen_is_world_stopped (), "We can't log if the world is stopped");
		mono_coop_mutex_lock (&log_entries_mutex);
		for (i = 0; i < log_entries.next_slot; i++) {
			sgen_output_log_entry (log_entries.data [i], stw_time, generation);
			sgen_free_internal (log_entries.data [i], INTERNAL_MEM_LOG_ENTRY);
		}
		sgen_pointer_queue_clear (&log_entries);
		mono_coop_mutex_unlock (&log_entries_mutex);
	}
}

/*
Global GC memory tracking.
This tracks the total usage of memory by the GC. This includes
managed and unmanaged memory.
*/

static unsigned long
prot_flags_for_activate (int activate)
{
	unsigned long prot_flags = activate? MONO_MMAP_READ|MONO_MMAP_WRITE: MONO_MMAP_NONE;
	return prot_flags | MONO_MMAP_PRIVATE | MONO_MMAP_ANON;
}

void
sgen_assert_memory_alloc (void *ptr, size_t requested_size, const char *assert_description)
{
	if (ptr || !assert_description)
		return;
	fprintf (stderr, "Error: Garbage collector could not allocate %zu bytes of memory for %s.\n", requested_size, assert_description);
	exit (1);
}

/*
 * Allocate a big chunk of memory from the OS (usually 64KB to several megabytes).
 * This must not require any lock.
 */
void*
sgen_alloc_os_memory (size_t size, SgenAllocFlags flags, const char *assert_description, MonoMemAccountType type)
{
	void *ptr;

	g_assert (!(flags & ~(SGEN_ALLOC_HEAP | SGEN_ALLOC_ACTIVATE)));

	ptr = mono_valloc (0, size, prot_flags_for_activate (flags & SGEN_ALLOC_ACTIVATE), type);
	sgen_assert_memory_alloc (ptr, size, assert_description);
	if (ptr) {
		SGEN_ATOMIC_ADD_P (total_alloc, size);
		total_alloc_max = MAX (total_alloc_max, total_alloc);
	}
	return ptr;
}

/* size must be a power of 2 */
// FIXME: remove assert_description
void*
sgen_alloc_os_memory_aligned (size_t size, mword alignment, SgenAllocFlags flags, const char *assert_description, MonoMemAccountType type)
{
	void *ptr;

	g_assert (!(flags & ~(SGEN_ALLOC_HEAP | SGEN_ALLOC_ACTIVATE)));

	ptr = mono_valloc_aligned (size, alignment, prot_flags_for_activate (flags & SGEN_ALLOC_ACTIVATE), type);
	sgen_assert_memory_alloc (ptr, size, assert_description);
	if (ptr) {
		SGEN_ATOMIC_ADD_P (total_alloc, size);
		total_alloc_max = MAX (total_alloc_max, total_alloc);
	}
	return ptr;
}

/*
 * Free the memory returned by sgen_alloc_os_memory (), returning it to the OS.
 */
void
sgen_free_os_memory (void *addr, size_t size, SgenAllocFlags flags, MonoMemAccountType type)
{
	g_assert (!(flags & ~SGEN_ALLOC_HEAP));

	mono_vfree (addr, size, type);
	SGEN_ATOMIC_ADD_P (total_alloc, -(gssize)size);
	total_alloc_max = MAX (total_alloc_max, total_alloc);
}

size_t
sgen_gc_get_total_heap_allocation (void)
{
	return total_alloc;
}


/*
Heap Sizing limits.
This limit the max size of the heap. It takes into account
only memory actively in use to hold heap objects and not
for other parts of the GC.
 */
static mword
sgen_memgov_available_free_space (void)
{
	return max_heap_size - MIN (allocated_heap, max_heap_size);
}

void
sgen_memgov_release_space (mword size, int space)
{
	SGEN_ATOMIC_ADD_P (allocated_heap, -(gssize)size);
}

gboolean
sgen_memgov_try_alloc_space (mword size, int space)
{
	if (sgen_memgov_available_free_space () < size) {
		SGEN_ASSERT (4, !sgen_thread_pool_is_thread_pool_thread (mono_native_thread_id_get ()), "Memory shouldn't run out in worker thread");
		return FALSE;
	}

	SGEN_ATOMIC_ADD_P (allocated_heap, size);
	sgen_client_total_allocated_heap_changed (allocated_heap);
	return TRUE;
}

void
sgen_memgov_init (size_t max_heap, size_t soft_limit, gboolean debug_allowance, double allowance_ratio, double save_target)
{
	if (soft_limit)
		soft_heap_limit = soft_limit;

	debug_print_allowance = debug_allowance;
	major_collection_trigger_size = MIN_MINOR_COLLECTION_ALLOWANCE;

	mono_counters_register ("Memgov alloc", MONO_COUNTER_GC | MONO_COUNTER_WORD | MONO_COUNTER_BYTES | MONO_COUNTER_VARIABLE, (void*)&total_alloc);
	mono_counters_register ("Memgov max alloc", MONO_COUNTER_GC | MONO_COUNTER_WORD | MONO_COUNTER_BYTES | MONO_COUNTER_MONOTONIC, (void*)&total_alloc_max);

	mono_coop_mutex_init (&log_entries_mutex);

	sgen_register_fixed_internal_mem_type (INTERNAL_MEM_LOG_ENTRY, sizeof (SgenLogEntry));

	if (max_heap == 0)
		return;

	if (max_heap < soft_limit) {
		sgen_env_var_error (MONO_GC_PARAMS_NAME, "Setting to minimum.", "`max-heap-size` must be at least as large as `soft-heap-limit`.");
		max_heap = soft_limit;
	}

	if (max_heap < sgen_nursery_size * 4) {
		sgen_env_var_error (MONO_GC_PARAMS_NAME, "Setting to minimum.", "`max-heap-size` must be at least 4 times as large as `nursery size`.");
		max_heap = sgen_nursery_size * 4;
	}
	max_heap_size = max_heap - sgen_nursery_size;

	if (allowance_ratio)
		default_allowance_nursery_size_ratio = allowance_ratio;

	if (save_target)
		save_target_ratio = save_target;
}

#endif
