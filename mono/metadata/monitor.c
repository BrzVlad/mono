/*
 * monitor.c:  Monitor locking functions
 *
 * Author:
 *	Dick Porter (dick@ximian.com)
 *
 * Copyright 2003 Ximian, Inc (http://www.ximian.com)
 * Copyright 2004-2009 Novell, Inc (http://www.novell.com)
 */

#include <config.h>
#include <glib.h>
#include <string.h>

#include <mono/metadata/abi-details.h>
#include <mono/metadata/monitor.h>
#include <mono/metadata/threads-types.h>
#include <mono/metadata/exception.h>
#include <mono/metadata/threads.h>
#include <mono/io-layer/io-layer.h>
#include <mono/metadata/object-internals.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/gc-internal.h>
#include <mono/metadata/method-builder.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/marshal.h>
#include <mono/metadata/profiler-private.h>
#include <mono/utils/mono-time.h>
#include <mono/utils/atomic.h>
#include <mono/utils/mono-threads.h>

/*
 * Pull the list of opcodes
 */
#define OPDEF(a,b,c,d,e,f,g,h,i,j) \
	a = i,

enum {
#include "mono/cil/opcode.def"
	LAST = 0xff
};
#undef OPDEF

/*#define LOCK_DEBUG(a) do { a; } while (0)*/
#define LOCK_DEBUG(a)

typedef struct _MonitorArray MonitorArray;

struct _MonitorArray {
	MonitorArray *next;
	int num_monitors;
	MonoThreadsSync monitors [MONO_ZERO_LEN_ARRAY];
};

#define mono_monitor_allocator_lock() mono_mutex_lock (&monitor_mutex)
#define mono_monitor_allocator_unlock() mono_mutex_unlock (&monitor_mutex)
static mono_mutex_t monitor_mutex;
static MonoThreadsSync *monitor_freelist;
static MonitorArray *monitor_allocated;
static int array_size = 16;

#ifdef HAVE_KW_THREAD
static __thread int tls_small_id MONO_TLS_FAST;
#endif

static gint32 mono_monitor_inflate_owned (MonoObject *obj, gboolean acquire);
static gint32 mono_monitor_inflate (MonoObject *obj, guint32 ms, gboolean allow_interruption, gboolean acquire, int id);

static inline int
get_thread_id (void)
{
#ifdef HAVE_KW_THREAD
	return tls_small_id;
#else
	return mono_thread_info_get_small_id ();
#endif
}

static inline MonoThreadsSync*
get_inflated_lock (LockWord lw)
{
	lw.lock_word &= (~LOCK_WORD_STATUS_MASK);
	return lw.sync;
}

void
mono_monitor_init (void)
{
	mono_mutex_init_recursive (&monitor_mutex);
}
 
void
mono_monitor_cleanup (void)
{
	MonoThreadsSync *mon;
	/* MonitorArray *marray, *next = NULL; */

	/*mono_mutex_destroy (&monitor_mutex);*/

	/* The monitors on the freelist don't have weak links - mark them */
	for (mon = monitor_freelist; mon; mon = mon->data)
		mon->wait_list = (gpointer)-1;

	/* FIXME: This still crashes with sgen (async_read.exe) */
	/*
	for (marray = monitor_allocated; marray; marray = next) {
		int i;

		for (i = 0; i < marray->num_monitors; ++i) {
			mon = &marray->monitors [i];
			if (mon->wait_list != (gpointer)-1)
				mono_gc_weak_link_remove (&mon->data);
		}

		next = marray->next;
		g_free (marray);
	}
	*/
}

/*
 * mono_monitor_init_tls:
 *
 *   Setup TLS variables used by the monitor code for the current thread.
 */
void
mono_monitor_init_tls (void)
{
#if defined(HAVE_KW_THREAD)
	tls_small_id = mono_thread_internal_current ()->small_id;
#endif
}

static int
monitor_is_on_freelist (MonoThreadsSync *mon)
{
	MonitorArray *marray;
	for (marray = monitor_allocated; marray; marray = marray->next) {
		if (mon >= marray->monitors && mon < &marray->monitors [marray->num_monitors])
			return TRUE;
	}
	return FALSE;
}

/**
 * mono_locks_dump:
 * @include_untaken:
 *
 * Print a report on stdout of the managed locks currently held by
 * threads. If @include_untaken is specified, list also inflated locks
 * which are unheld.
 * This is supposed to be used in debuggers like gdb.
 */
void
mono_locks_dump (gboolean include_untaken)
{
	int i;
	int used = 0, on_freelist = 0, to_recycle = 0, total = 0, num_arrays = 0;
	MonoThreadsSync *mon;
	MonitorArray *marray;
	for (mon = monitor_freelist; mon; mon = mon->data)
		on_freelist++;
	for (marray = monitor_allocated; marray; marray = marray->next) {
		total += marray->num_monitors;
		num_arrays++;
		for (i = 0; i < marray->num_monitors; ++i) {
			mon = &marray->monitors [i];
			if (mon->data == NULL) {
				if (i < marray->num_monitors - 1)
					to_recycle++;
			} else {
				if (!monitor_is_on_freelist (mon->data)) {
					MonoObject *holder = mono_gc_weak_link_get (&mon->data);
					if (mon->owner) {
						g_print ("Lock %p in object %p held by thread %x, nest level: %d\n",
							mon, holder, mon->owner, mon->nest);
						if (mon->entry_sem)
							g_print ("\tWaiting on semaphore %p: %d\n", mon->entry_sem, mon->entry_count);
					} else if (include_untaken) {
						g_print ("Lock %p in object %p untaken\n", mon, holder);
					}
					used++;
				}
			}
		}
	}
	g_print ("Total locks (in %d array(s)): %d, used: %d, on freelist: %d, to recycle: %d\n",
		num_arrays, total, used, on_freelist, to_recycle);
}

/* LOCKING: this is called with monitor_mutex held */
static void 
mon_finalize (MonoThreadsSync *mon)
{
	LOCK_DEBUG (g_message ("%s: Finalizing sync %p", __func__, mon));

	if (mon->entry_sem != NULL) {
		CloseHandle (mon->entry_sem);
		mon->entry_sem = NULL;
	}
	/* If this isn't empty then something is seriously broken - it
	 * means a thread is still waiting on the object that owned
	 * this lock, but the object has been finalized.
	 */
	g_assert (mon->wait_list == NULL);

	mon->entry_count = 0;
	/* owner and nest are set in mon_new, no need to zero them out */

	mon->data = monitor_freelist;
	monitor_freelist = mon;
#ifndef DISABLE_PERFCOUNTERS
	mono_perfcounters->gc_sync_blocks--;
#endif
}

/* LOCKING: this is called with monitor_mutex held */
static MonoThreadsSync *
mon_new (gsize id)
{
	MonoThreadsSync *new;

	if (!monitor_freelist) {
		MonitorArray *marray;
		int i;
		/* see if any sync block has been collected */
		new = NULL;
		for (marray = monitor_allocated; marray; marray = marray->next) {
			for (i = 0; i < marray->num_monitors; ++i) {
				if (marray->monitors [i].data == NULL) {
					new = &marray->monitors [i];
					if (new->wait_list) {
						/* Orphaned events left by aborted threads */
						while (new->wait_list) {
							LOCK_DEBUG (g_message (G_GNUC_PRETTY_FUNCTION ": (%d): Closing orphaned event %d", get_thread_id (), new->wait_list->data));
							CloseHandle (new->wait_list->data);
							new->wait_list = g_slist_remove (new->wait_list, new->wait_list->data);
						}
					}
					mono_gc_weak_link_remove (&new->data, TRUE);
					new->data = monitor_freelist;
					monitor_freelist = new;
				}
			}
			/* small perf tweak to avoid scanning all the blocks */
			if (new)
				break;
		}
		/* need to allocate a new array of monitors */
		if (!monitor_freelist) {
			MonitorArray *last;
			LOCK_DEBUG (g_message ("%s: allocating more monitors: %d", __func__, array_size));
			marray = g_malloc0 (sizeof (MonoArray) + array_size * sizeof (MonoThreadsSync));
			marray->num_monitors = array_size;
			array_size *= 2;
			/* link into the freelist */
			for (i = 0; i < marray->num_monitors - 1; ++i) {
				marray->monitors [i].data = &marray->monitors [i + 1];
			}
			marray->monitors [i].data = NULL; /* the last one */
			monitor_freelist = &marray->monitors [0];
			/* we happend the marray instead of prepending so that
			 * the collecting loop above will need to scan smaller arrays first
			 */
			if (!monitor_allocated) {
				monitor_allocated = marray;
			} else {
				last = monitor_allocated;
				while (last->next)
					last = last->next;
				last->next = marray;
			}
		}
	}

	new = monitor_freelist;
	monitor_freelist = new->data;

	new->owner = id;
	new->nest = 1;
	new->data = NULL;
	
#ifndef DISABLE_PERFCOUNTERS
	mono_perfcounters->gc_sync_blocks++;
#endif
	return new;
}

#define MONO_OBJECT_ALIGNMENT_SHIFT	3

/*
 * mono_object_hash:
 * @obj: an object
 *
 * Calculate a hash code for @obj that is constant while @obj is alive.
 */
int
mono_object_hash (MonoObject* obj)
{
#ifdef HAVE_MOVING_COLLECTOR
	LockWord lw;
	guint32 hash;
	int id;
	if (!obj)
		return 0;
	lw.sync = obj->synchronisation;

	LOCK_DEBUG (g_message("%s: (%d) Get hash for object %p; LW = %p", __func__, get_thread_id (), obj, obj->synchronisation));

	if ((lw.lock_word & LOCK_WORD_STATUS_MASK) == LOCK_WORD_THIN_HASH) {
		return lw.lock_word >> LOCK_WORD_HASH_SHIFT;
	}

	if ((lw.lock_word & LOCK_WORD_STATUS_MASK) == LOCK_WORD_FAT_HASH) {
		return get_inflated_lock (lw)->hash_code;
	}

	hash = (GPOINTER_TO_UINT (obj) >> MONO_OBJECT_ALIGNMENT_SHIFT) * 2654435761u;
#if SIZEOF_VOID_P == 4
	hash &= ~(LOCK_WORD_STATUS_MASK << (32 - LOCK_WORD_STATUS_BITS));
#endif

	id = get_thread_id ();

	/*
	 * while we are inside this function, the GC will keep this object pinned,
	 * since we are in the unmanaged stack. Thanks to this and to the hash
	 * function that depends only on the address, we can ignore the races if
	 * another thread computes the hash at the same time, because it'll end up
	 * with the same value.
	 */
	if (lw.lock_word == 0) {
		LockWord old_lw;
		lw.lock_word = hash;
		lw.lock_word = (lw.lock_word << LOCK_WORD_HASH_SHIFT) | LOCK_WORD_THIN_HASH;
		old_lw.sync = InterlockedCompareExchangePointer ((gpointer*)&obj->synchronisation, lw.sync, NULL);
		if (old_lw.sync == NULL) {
			return hash;
		}

		/* No need to inflate if another hash was placed in the lock word */
		if ((old_lw.lock_word & LOCK_WORD_STATUS_MASK) == LOCK_WORD_THIN_HASH) {
			return old_lw.lock_word >> LOCK_WORD_HASH_SHIFT;
		}

		mono_monitor_inflate (obj, INFINITE, FALSE, FALSE, id);
		lw.sync = obj->synchronisation;
	} else if ((lw.lock_word & LOCK_WORD_STATUS_MASK) == LOCK_WORD_FLAT) {
		if ((lw.lock_word >> LOCK_WORD_OWNER_SHIFT) == id)
			mono_monitor_inflate_owned (obj, FALSE);
		else
			mono_monitor_inflate (obj, INFINITE, FALSE, FALSE, id);
		lw.sync = obj->synchronisation;
	}

	/* At this point, the lock is inflated */

	get_inflated_lock (lw)->hash_code = hash;
	lw.lock_word |= LOCK_WORD_FAT_HASH;
	mono_memory_write_barrier ();
	obj->synchronisation = lw.sync;
	return hash;
#else
/*
 * Wang's address-based hash function:
 *   http://www.concentric.net/~Ttwang/tech/addrhash.htm
 */
	return (GPOINTER_TO_UINT (obj) >> MONO_OBJECT_ALIGNMENT_SHIFT) * 2654435761u;
#endif
}


static void
mono_monitor_ensure_synchronized (LockWord lw, guint32 id)
{
	if ((lw.lock_word & LOCK_WORD_STATUS_MASK) == LOCK_WORD_FLAT) {
		if ((lw.lock_word >> LOCK_WORD_OWNER_SHIFT) == id)
			return;
	} else if (lw.lock_word & LOCK_WORD_INFLATED) {
		if (get_inflated_lock (lw)->owner == id)
			return;
	}

	mono_raise_exception (mono_get_exception_synchronization_lock ("Object synchronization method was called from an unsynchronized block of code."));
}

/*
 * When this function is called it has already been established that the
 * current thread owns the monitor.
 */
static void
mono_monitor_exit_inflated (MonoObject *obj)
{
	LockWord lw;
	MonoThreadsSync *mon;

	lw.sync = obj->synchronisation;
	mon = get_inflated_lock (lw);
	if (G_LIKELY (mon->nest == 1)) {
		LOCK_DEBUG (g_message ("%s: (%d) Object %p is now unlocked; LW = %p", __func__, get_thread_id (), obj, obj->synchronisation));

		/* object is now unlocked, leave nest==1 so we don't
		 * need to set it when the lock is reacquired
		 */
		mon->owner = 0;

		/* Do the wakeup stuff. It's possible that the last
		 * blocking thread gave up waiting just before we
		 * release the semaphore resulting in a futile wakeup
		 * next time there's contention for this object, but
		 * it means we don't have to waste time locking the
		 * struct.
		 */
		if (mon->entry_count > 0) {
			ReleaseSemaphore (mon->entry_sem, 1, NULL);
		}
	} else {
		mon->nest -= 1;
		LOCK_DEBUG (g_message ("%s: (%d) Object %p is now locked %d times; LW = %p", __func__, get_thread_id (), obj, mon->nest, obj->synchronisation));
	}
}

/*
 * When this function is called it has already been established that the
 * current thread owns the monitor.
 */
static void
mono_monitor_exit_flat (MonoObject *obj, LockWord old_lw)
{
	LockWord new_lw;
	new_lw = old_lw;
	if (G_UNLIKELY (old_lw.lock_word & LOCK_WORD_NEST_MASK)) {
		new_lw.lock_word -= 1 << LOCK_WORD_NEST_SHIFT;
		obj->synchronisation = new_lw.sync;
		LOCK_DEBUG (g_message ("%s: (%d) Object %p is now locked %d times; LW = %p", __func__, get_thread_id (), obj, ((new_lw.lock_word & LOCK_WORD_NEST_MASK) >> LOCK_WORD_NEST_SHIFT) + 1, obj->synchronisation));
	} else {
		new_lw.lock_word = 0;
		obj->synchronisation = new_lw.sync;
		LOCK_DEBUG (g_message ("%s: (%d) Object %p is now unlocked; LW = %p", __func__, get_thread_id (), obj, obj->synchronisation));
	}
}

/* If allow_interruption==TRUE, the method will be interrumped if abort or suspend
 * is requested. In this case it returns -1.
 */ 
static gint32
mono_monitor_try_enter_inflated (MonoObject *obj, guint32 ms, gboolean allow_interruption, guint32 id)
{
	LockWord lw;
	MonoThreadsSync *mon;
	HANDLE sem;
	guint32 then = 0, now, delta;
	guint32 waitms;
	guint32 ret;
	MonoInternalThread *thread;

	lw.sync = obj->synchronisation;

	/* Lock is inflated */
	g_assert (lw.lock_word & LOCK_WORD_INFLATED);

	mon = get_inflated_lock (lw);

retry:
	/* If the object has previously been locked but isn't now... */
	if (G_LIKELY (mon->owner == 0)) {
		/* Try to install our ID in the owner field, nest
		 * should have been left at 1 by the previous unlock
		 * operation
		 */
		if (G_LIKELY (InterlockedCompareExchange ((gint32 *)&mon->owner, id, 0) == 0)) {
			/* Success */
			g_assert (mon->nest == 1);
			return 1;
		} else {
			/* Trumped again! */
			goto retry;
		}
	}

	/* If the object is currently locked by this thread... */
	if (mon->owner == id) {
		mon->nest++;
		return 1;
	}

	/* The object must be locked by someone else... */
#ifndef DISABLE_PERFCOUNTERS
	mono_perfcounters->thread_contentions++;
#endif

	/* If ms is 0 we don't block, but just fail straight away */
	if (ms == 0) {
		LOCK_DEBUG (g_message ("%s: (%d) timed out, returning FALSE", __func__, id));
		return 0;
	}

	mono_profiler_monitor_event (obj, MONO_PROFILER_MONITOR_CONTENTION);

	/* The slow path begins here. */
retry_contended:
	/* a small amount of duplicated code, but it allows us to insert the profiler
	 * callbacks without impacting the fast path: from here on we don't need to go back to the
	 * retry label, but to retry_contended. At this point mon is already installed in the object
	 * header.
	 */
	if (G_LIKELY (mon->owner == 0)) {
		/* Try to install our ID in the owner field, nest
		* should have been left at 1 by the previous unlock
		* operation
		*/
		if (G_LIKELY (InterlockedCompareExchange ((gint32 *)&mon->owner, id, 0) == 0)) {
			/* Success */
			g_assert (mon->nest == 1);
			mono_profiler_monitor_event (obj, MONO_PROFILER_MONITOR_DONE);
			return 1;
		}
	}

	/* If the object is currently locked by this thread... */
	if (mon->owner == id) {
		mon->nest++;
		mono_profiler_monitor_event (obj, MONO_PROFILER_MONITOR_DONE);
		return 1;
	}

	/* We need to make sure there's a semaphore handle (creating it if
	 * necessary), and block on it
	 */
	if (mon->entry_sem == NULL) {
		/* Create the semaphore */
		sem = CreateSemaphore (NULL, 0, 0x7fffffff, NULL);
		g_assert (sem != NULL);
		if (InterlockedCompareExchangePointer ((gpointer*)&mon->entry_sem, sem, NULL) != NULL) {
			/* Someone else just put a handle here */
			CloseHandle (sem);
		}
	}
	
	/* If we need to time out, record a timestamp and adjust ms,
	 * because WaitForSingleObject doesn't tell us how long it
	 * waited for.
	 *
	 * Don't block forever here, because theres a chance the owner
	 * thread released the lock while we were creating the
	 * semaphore: we would not get the wakeup.  Using the event
	 * handle technique from pulse/wait would involve locking the
	 * lock struct and therefore slowing down the fast path.
	 */
	if (ms != INFINITE) {
		then = mono_msec_ticks ();
		if (ms < 100) {
			waitms = ms;
		} else {
			waitms = 100;
		}
	} else {
		waitms = 100;
	}
	
	InterlockedIncrement (&mon->entry_count);

#ifndef DISABLE_PERFCOUNTERS
	mono_perfcounters->thread_queue_len++;
	mono_perfcounters->thread_queue_max++;
#endif
	thread = mono_thread_internal_current ();

	mono_thread_set_state (thread, ThreadState_WaitSleepJoin);

	/*
	 * We pass TRUE instead of allow_interruption since we have to check for the
	 * StopRequested case below.
	 */
	ret = WaitForSingleObjectEx (mon->entry_sem, waitms, TRUE);

	mono_thread_clr_state (thread, ThreadState_WaitSleepJoin);
	
	InterlockedDecrement (&mon->entry_count);
#ifndef DISABLE_PERFCOUNTERS
	mono_perfcounters->thread_queue_len--;
#endif

	if (ms != INFINITE) {
		now = mono_msec_ticks ();
		
		if (now < then) {
			/* The counter must have wrapped around */
			LOCK_DEBUG (g_message ("%s: wrapped around! now=0x%x then=0x%x", __func__, now, then));
			
			now += (0xffffffff - then);
			then = 0;

			LOCK_DEBUG (g_message ("%s: wrap rejig: now=0x%x then=0x%x delta=0x%x", __func__, now, then, now-then));
		}
		
		delta = now - then;
		if (delta >= ms) {
			ms = 0;
		} else {
			ms -= delta;
		}

		if ((ret == WAIT_TIMEOUT || (ret == WAIT_IO_COMPLETION && !allow_interruption)) && ms > 0) {
			/* More time left */
			goto retry_contended;
		}
	} else {
		if (ret == WAIT_TIMEOUT || (ret == WAIT_IO_COMPLETION && !allow_interruption)) {
			if (ret == WAIT_IO_COMPLETION && (mono_thread_test_state (thread, (ThreadState_StopRequested|ThreadState_SuspendRequested)))) {
				/* 
				 * We have to obey a stop/suspend request even if 
				 * allow_interruption is FALSE to avoid hangs at shutdown.
				 */
				mono_profiler_monitor_event (obj, MONO_PROFILER_MONITOR_FAIL);
				return -1;
			}
			/* Infinite wait, so just try again */
			goto retry_contended;
		}
	}
	
	if (ret == WAIT_OBJECT_0) {
		/* retry from the top */
		goto retry_contended;
	}

	/* We must have timed out */
	LOCK_DEBUG (g_message ("%s: (%d) timed out waiting, returning FALSE", __func__, id));

	mono_profiler_monitor_event (obj, MONO_PROFILER_MONITOR_FAIL);

	if (ret == WAIT_IO_COMPLETION)
		return -1;
	else 
		return 0;
}

static gint32
mono_monitor_inflate_owned (MonoObject *obj, gboolean acquire)
{
	MonoThreadsSync *mon;
	LockWord nlw, lw;
	guint32 nest;
	int id = get_thread_id ();

	lw.sync = obj->synchronisation;
	LOCK_DEBUG (g_message ("%s: (%d) Inflating owned lock object %p; LW = %p", __func__, id, obj, lw.sync));

	/* In word nest count starts from 0 */
	nest = ((lw.lock_word & LOCK_WORD_NEST_MASK) >> LOCK_WORD_NEST_SHIFT) + 1;

	/* Allocate the lock object */
	mono_monitor_allocator_lock ();
	mon = mon_new (id);

	if (acquire)
		mon->nest = nest + 1;
	else
		mon->nest = nest;

	nlw.sync = mon;
	g_assert ((nlw.lock_word & LOCK_WORD_STATUS_MASK) == 0);
	nlw.lock_word |= LOCK_WORD_INFLATED;

	mono_memory_write_barrier ();
	obj->synchronisation = nlw.sync;
	mono_gc_weak_link_add (&mon->data, obj, TRUE);
	mono_monitor_allocator_unlock ();

	return 1;
}

static void
discard_mon (MonoThreadsSync *mon)
{
	mono_monitor_allocator_lock ();
	mono_gc_weak_link_remove (&mon->data, TRUE);
	mon_finalize (mon);
	mono_monitor_allocator_unlock ();
}

static gint32
mono_monitor_inflate (MonoObject *obj, guint32 ms, gboolean allow_interruption, gboolean acquire, int id)
{
	MonoThreadsSync *mon;
	LockWord nlw;
	guint32 then;
	MonoInternalThread *thread = mono_thread_internal_current ();

	LOCK_DEBUG (g_message ("%s: (%d) Inflating lock object %p; LW = %p", __func__, id, obj, obj->synchronisation));

	then = ms != INFINITE ? mono_msec_ticks () : 0;

	/* Allocate the lock object */
	mono_monitor_allocator_lock ();
	mon = mon_new (id);
	mono_gc_weak_link_add (&mon->data, obj, TRUE);
	mono_monitor_allocator_unlock ();
	if (!acquire) {
		mon->owner = 0;
	}

	nlw.sync = mon;
	g_assert ((nlw.lock_word & LOCK_WORD_STATUS_MASK) == 0);
	nlw.lock_word |= LOCK_WORD_INFLATED;

	mono_memory_write_barrier ();
	for (;;) {
		LockWord lw;
		/* Try inserting the pointer */
		lw.sync = InterlockedCompareExchangePointer ((gpointer*)&obj->synchronisation, nlw.sync, NULL);
		if (lw.sync == NULL) {
			LOCK_DEBUG (g_message ("%s: (%d) Inflated lock object %p to mon %p (%d); LW = %p / prev = %p", __func__, get_thread_id (), obj, mon, mon->owner, obj->synchronisation, lw.sync));
			return 1;
		} else if (lw.lock_word & LOCK_WORD_INFLATED) {
			/* Another thread inflated the lock while we were waiting */
			discard_mon (mon);
			if (acquire)
				return mono_monitor_try_enter_inflated (obj, ms, allow_interruption, id);
			else
				return 1;
		}
#ifdef HAVE_MOVING_COLLECTOR
		 else if ((lw.lock_word & LOCK_WORD_STATUS_MASK) == LOCK_WORD_THIN_HASH) {
			nlw.lock_word |= LOCK_WORD_FAT_HASH;
			mon->hash_code = (lw.lock_word & (~LOCK_WORD_STATUS_MASK)) >> LOCK_WORD_HASH_SHIFT;
			mono_memory_write_barrier ();
			if (InterlockedCompareExchangePointer ((gpointer*)&obj->synchronisation, nlw.sync, lw.sync) == lw.sync) {
				LOCK_DEBUG (g_message ("%s: (%d) Inflated lock object %p to mon %p (%d); LW = %p / prev = %p", __func__, get_thread_id (), obj, mon, mon->owner, obj->synchronisation, lw.sync));
				return 1;
			}
		}
#endif

		mono_threads_core_yield ();

		if (ms != INFINITE) {
			int now = mono_msec_ticks ();
			int elapsed = now == then ? 1 : now - then;
			if (ms <= elapsed) {
				discard_mon (mon);
				LOCK_DEBUG (g_message ("%s: (%d) Inflation of lock object %p timed out", __func__, get_thread_id (), obj));
				return 0;
			} else {
				ms -= elapsed;
			}

			then = now;
		}

		if (allow_interruption && mono_thread_interruption_requested ()) {
			discard_mon (mon);
			return -1;
		} else if (!allow_interruption && mono_thread_test_state (thread, (ThreadState_StopRequested|ThreadState_SuspendRequested))) {
			/*
			 * We have to obey a stop/suspend request even if
			 * allow_interruption is FALSE to avoid hangs at shutdown.
			 */
			mono_profiler_monitor_event (obj, MONO_PROFILER_MONITOR_FAIL);
			discard_mon (mon);
			return -1;
		}

		lw.sync = obj->synchronisation;
	}
}


/* If allow_interruption==TRUE, the method will be interrumped if abort or suspend
 * is requested. In this case it returns -1.
 */
static gint32
mono_monitor_try_enter_internal (MonoObject *obj, guint32 ms, gboolean allow_interruption)
{
	LockWord lw;
	int id = get_thread_id ();

	LOCK_DEBUG (g_message("%s: (%d) Trying to lock object %p (%d ms)", __func__, id, obj, ms));

	if (G_UNLIKELY (!obj)) {
		mono_raise_exception (mono_get_exception_argument_null ("obj"));
		return FALSE;
	}

	lw.sync = obj->synchronisation;

	if (G_LIKELY (lw.lock_word == 0)) {
		LockWord nlw, prev;
		nlw.lock_word = id << LOCK_WORD_OWNER_SHIFT;
		prev.sync = InterlockedCompareExchangePointer ((gpointer*)&obj->synchronisation, nlw.sync, NULL);
		if (prev.sync == NULL) {
			LOCK_DEBUG (g_message("%s: (%d) Entered flat %p (%d ms); LW = %p / prev = %p", __func__, id, obj, ms, obj->synchronisation, prev.sync));
			return 1;
		} else {
			/* Somebody else acquired it in the meantime */
			return mono_monitor_inflate (obj, ms, allow_interruption, TRUE, id);
		}
	} else if ((lw.lock_word & LOCK_WORD_STATUS_MASK) == LOCK_WORD_FLAT) {
		if ((lw.lock_word >> LOCK_WORD_OWNER_SHIFT) == id) {
			if ((lw.lock_word & LOCK_WORD_NEST_MASK) == LOCK_WORD_NEST_MASK) {
				/* Inflate and acquire the lock because nest count overflows */
				return mono_monitor_inflate_owned (obj, TRUE);
			} else {
				lw.lock_word += 1 << LOCK_WORD_NEST_SHIFT;
				obj->synchronisation = lw.sync;
				return 1;
			}
		} else {
			/* Lock owned by somebody else */
			return mono_monitor_inflate (obj, ms, allow_interruption, TRUE, id);
		}
	} else if ((lw.lock_word & LOCK_WORD_STATUS_MASK) == LOCK_WORD_THIN_HASH) {
		return mono_monitor_inflate (obj, ms, allow_interruption, TRUE, id);
	} else if (lw.lock_word & LOCK_WORD_INFLATED){
		return mono_monitor_try_enter_inflated (obj, ms, allow_interruption, id);
	}

	g_assert_not_reached ();

	return -1;
}

gboolean
mono_monitor_enter (MonoObject *obj)
{
	return mono_monitor_try_enter_internal (obj, INFINITE, FALSE) == 1;
}

gboolean 
mono_monitor_try_enter (MonoObject *obj, guint32 ms)
{
	return mono_monitor_try_enter_internal (obj, ms, FALSE) == 1;
}

void
mono_monitor_exit (MonoObject *obj)
{
	LockWord lw;

	LOCK_DEBUG (g_message ("%s: (%d) Unlocking %p", __func__, get_thread_id (), obj));

	if (G_UNLIKELY (!obj)) {
		mono_raise_exception (mono_get_exception_argument_null ("obj"));
		return;
	}

	lw.sync = obj->synchronisation;

	mono_monitor_ensure_synchronized (lw, get_thread_id ());

	if (G_UNLIKELY (lw.lock_word & LOCK_WORD_INFLATED)) {
		mono_monitor_exit_inflated (obj);
	} else {
		mono_monitor_exit_flat (obj, lw);
	}
}

void**
mono_monitor_get_object_monitor_weak_link (MonoObject *object)
{
	LockWord lw;

	lw.sync = object->synchronisation;
	if (lw.lock_word & LOCK_WORD_INFLATED) {
		return &get_inflated_lock (lw)->data;
	} else {
		return NULL;
	}
}

/*
 * mono_monitor_threads_sync_member_offset:
 * @owner_offset: returns size and offset of the "owner" member
 * @nest_offset: returns size and offset of the "nest" member
 * @entry_count_offset: returns size and offset of the "entry_count" member
 *
 * Returns the offsets and sizes of three members of the
 * MonoThreadsSync struct.  The Monitor ASM fastpaths need this.
 */
void
mono_monitor_threads_sync_members_offset (int *owner_offset, int *nest_offset, int *entry_count_offset)
{
	MonoThreadsSync ts;

#define ENCODE_OFF_SIZE(o,s)	(((o) << 8) | ((s) & 0xff))

	*owner_offset = ENCODE_OFF_SIZE (MONO_STRUCT_OFFSET (MonoThreadsSync, owner), sizeof (ts.owner));
	*nest_offset = ENCODE_OFF_SIZE (MONO_STRUCT_OFFSET (MonoThreadsSync, nest), sizeof (ts.nest));
	*entry_count_offset = ENCODE_OFF_SIZE (MONO_STRUCT_OFFSET (MonoThreadsSync, entry_count), sizeof (ts.entry_count));
}

gboolean 
ves_icall_System_Threading_Monitor_Monitor_try_enter (MonoObject *obj, guint32 ms)
{
	gint32 res;

	do {
		res = mono_monitor_try_enter_internal (obj, ms, TRUE);
		if (res == -1)
			mono_thread_interruption_checkpoint ();
	} while (res == -1);
	
	return res == 1;
}

void
ves_icall_System_Threading_Monitor_Monitor_try_enter_with_atomic_var (MonoObject *obj, guint32 ms, char *lockTaken)
{
	gint32 res;
	do {
		res = mono_monitor_try_enter_internal (obj, ms, TRUE);
		/*This means we got interrupted during the wait and didn't got the monitor.*/
		if (res == -1)
			mono_thread_interruption_checkpoint ();
	} while (res == -1);
	/*It's safe to do it from here since interruption would happen only on the wrapper.*/
	*lockTaken = res == 1;
}

gboolean 
ves_icall_System_Threading_Monitor_Monitor_test_owner (MonoObject *obj)
{
	LockWord lw;

	LOCK_DEBUG (g_message ("%s: Testing if %p is owned by thread %d", __func__, obj, get_thread_id ()));

	lw.sync = obj->synchronisation;

	if ((lw.lock_word & LOCK_WORD_STATUS_MASK) == LOCK_WORD_FLAT) {
		return (lw.lock_word >> LOCK_WORD_OWNER_SHIFT) == get_thread_id ();
	} else if (lw.lock_word & LOCK_WORD_INFLATED) {
		return get_inflated_lock (lw)->owner == get_thread_id ();
	}
	
	return(FALSE);
}

gboolean 
ves_icall_System_Threading_Monitor_Monitor_test_synchronised (MonoObject *obj)
{
	LockWord lw;

	LOCK_DEBUG (g_message("%s: (%d) Testing if %p is owned by any thread", __func__, get_thread_id (), obj));

	lw.sync = obj->synchronisation;

	if ((lw.lock_word & LOCK_WORD_STATUS_MASK) == LOCK_WORD_FLAT) {
		return (lw.lock_word >> LOCK_WORD_OWNER_SHIFT) != 0;
	} else if (lw.lock_word & LOCK_WORD_INFLATED) {
		return get_inflated_lock (lw)->owner != 0;
	}

	return FALSE;
}

/* All wait list manipulation in the pulse, pulseall and wait
 * functions happens while the monitor lock is held, so we don't need
 * any extra struct locking
 */

void
ves_icall_System_Threading_Monitor_Monitor_pulse (MonoObject *obj)
{
	int id;
	LockWord lw;
	MonoThreadsSync *mon;

	LOCK_DEBUG (g_message ("%s: (%d) Pulsing %p", __func__, get_thread_id (), obj));

	id = get_thread_id ();
	lw.sync = obj->synchronisation;

	mono_monitor_ensure_synchronized (lw, id);

	if (!(lw.lock_word & LOCK_WORD_INFLATED)) {
		/* No threads waiting. A wait would have inflated the lock */
		return;
	}

	mon = get_inflated_lock (lw);

	LOCK_DEBUG (g_message ("%s: (%d) %d threads waiting", __func__, get_thread_id (), g_slist_length (mon->wait_list)));

	if (mon->wait_list != NULL) {
		LOCK_DEBUG (g_message ("%s: (%d) signalling and dequeuing handle %p", __func__, get_thread_id (), mon->wait_list->data));

		SetEvent (mon->wait_list->data);
		mon->wait_list = g_slist_remove (mon->wait_list, mon->wait_list->data);
	}
}

void
ves_icall_System_Threading_Monitor_Monitor_pulse_all (MonoObject *obj)
{
	int id;
	LockWord lw;
	MonoThreadsSync *mon;

	LOCK_DEBUG (g_message("%s: (%d) Pulsing all %p", __func__, get_thread_id (), obj));

	id = get_thread_id ();
	lw.sync = obj->synchronisation;

	mono_monitor_ensure_synchronized (lw, id);

	if (!(lw.lock_word & LOCK_WORD_INFLATED)) {
		/* No threads waiting. A wait would have inflated the lock */
		return;
	}

	mon = get_inflated_lock (lw);

	LOCK_DEBUG (g_message ("%s: (%d) %d threads waiting", __func__, get_thread_id (), g_slist_length (mon->wait_list)));

	while (mon->wait_list != NULL) {
		LOCK_DEBUG (g_message ("%s: (%d) signalling and dequeuing handle %p", __func__, get_thread_id (), mon->wait_list->data));

		SetEvent (mon->wait_list->data);
		mon->wait_list = g_slist_remove (mon->wait_list, mon->wait_list->data);
	}
}

gboolean
ves_icall_System_Threading_Monitor_Monitor_wait (MonoObject *obj, guint32 ms)
{
	LockWord lw;
	HANDLE event;
	guint32 nest;
	guint32 ret;
	gboolean success = FALSE;
	gint32 regain;
	MonoInternalThread *thread = mono_thread_internal_current ();
	MonoThreadsSync *mon;
	int id = get_thread_id ();

	LOCK_DEBUG (g_message ("%s: (%d) Trying to wait for %p with timeout %dms", __func__, get_thread_id (), obj, ms));

	lw.sync = obj->synchronisation;

	mono_monitor_ensure_synchronized (lw, id);

	if (!(lw.lock_word & LOCK_WORD_INFLATED)) {
		mono_monitor_inflate_owned (obj, FALSE);
		lw.sync = obj->synchronisation;
	}

	mon = get_inflated_lock (lw);

	/* Do this WaitSleepJoin check before creating the event handle */
	mono_thread_current_check_pending_interrupt ();
	
	event = CreateEvent (NULL, FALSE, FALSE, NULL);
	if (event == NULL) {
		mono_raise_exception (mono_get_exception_synchronization_lock ("Failed to set up wait event"));
		return FALSE;
	}
	
	LOCK_DEBUG (g_message ("%s: (%d) queuing handle %p", __func__, get_thread_id (), event));

	mono_thread_current_check_pending_interrupt ();
	
	mono_thread_set_state (thread, ThreadState_WaitSleepJoin);

	mon->wait_list = g_slist_append (mon->wait_list, event);
	
	/* Save the nest count, and release the lock */
	nest = mon->nest;
	mon->nest = 1;
	mono_memory_write_barrier ();
	mono_monitor_exit_inflated (obj);

	LOCK_DEBUG (g_message ("%s: (%d) Unlocked %p lock %p", __func__, get_thread_id (), obj, mon));

	/* There's no race between unlocking mon and waiting for the
	 * event, because auto reset events are sticky, and this event
	 * is private to this thread.  Therefore even if the event was
	 * signalled before we wait, we still succeed.
	 */
	ret = WaitForSingleObjectEx (event, ms, TRUE);

	/* Reset the thread state fairly early, so we don't have to worry
	 * about the monitor error checking
	 */
	mono_thread_clr_state (thread, ThreadState_WaitSleepJoin);
	
	if (mono_thread_interruption_requested ()) {
		/* 
		 * Can't remove the event from wait_list, since the monitor is not locked by
		 * us. So leave it there, mon_new () will delete it when the mon structure
		 * is placed on the free list.
		 * FIXME: The caller expects to hold the lock after the wait returns, but it
		 * doesn't happen in this case:
		 * http://connect.microsoft.com/VisualStudio/feedback/ViewFeedback.aspx?FeedbackID=97268
		 */
		return FALSE;
	}

	/* Regain the lock with the previous nest count */
	do {
		regain = mono_monitor_try_enter_inflated (obj, INFINITE, TRUE, id);
		if (regain == -1) 
			mono_thread_interruption_checkpoint ();
	} while (regain == -1);

	mon->nest = nest;

	LOCK_DEBUG (g_message ("%s: (%d) Regained %p lock %p", __func__, get_thread_id (), obj, mon));

	if (ret == WAIT_TIMEOUT) {
		/* Poll the event again, just in case it was signalled
		 * while we were trying to regain the monitor lock
		 */
		ret = WaitForSingleObjectEx (event, 0, FALSE);
	}

	/* Pulse will have popped our event from the queue if it signalled
	 * us, so we only do it here if the wait timed out.
	 *
	 * This avoids a race condition where the thread holding the
	 * lock can Pulse several times before the WaitForSingleObject
	 * returns.  If we popped the queue here then this event might
	 * be signalled more than once, thereby starving another
	 * thread.
	 */
	
	if (ret == WAIT_OBJECT_0) {
		LOCK_DEBUG (g_message ("%s: (%d) Success", __func__, get_thread_id ()));
		success = TRUE;
	} else {
		LOCK_DEBUG (g_message ("%s: (%d) Wait failed, dequeuing handle %p", __func__, get_thread_id (), event));
		/* No pulse, so we have to remove ourself from the wait queue.
		 */
		mon->wait_list = g_slist_remove (mon->wait_list, event);
	}
	CloseHandle (event);
	
	return success;
}

