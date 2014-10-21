/*
 * monitor.h: Monitor locking functions
 *
 * Author:
 *	Dick Porter (dick@ximian.com)
 *
 * (C) 2003 Ximian, Inc
 */

#ifndef _MONO_METADATA_MONITOR_H_
#define _MONO_METADATA_MONITOR_H_

#include <glib.h>
#include <mono/metadata/object.h>
#include <mono/io-layer/io-layer.h>
#include "mono/utils/mono-compiler.h"

G_BEGIN_DECLS

struct _MonoThreadsSync
{
	volatile guint32 owner;	/* thread ID */
	guint32 nest;	/* the number of times the thread owns the lock */
#ifdef HAVE_MOVING_COLLECTOR
	volatile gint32 hash_code;
#endif
	volatile gint32 entry_count;
	HANDLE entry_sem;
	GSList *wait_list;
	void *data;
};

/*
 * Lock word format:
 *
 * The least significant bit stores whether a hash for the object is computed
 * and it is stored either in the lock word or in the MonoThreadsSync structure
 * that the lock word points to.
 *
 * The second bit stores whether the lock word is inflated, containing an
 * address to the MonoThreadsSync structure.
 *
 * If both bits are 0, either the lock word is free (entire lock word is 0)
 * or it is a thin lock.
 *
 * 32-bit
 *            LOCK_WORD_FLAT:    [owner:22 | nest:8 | status:2]
 *       LOCK_WORD_THIN_HASH:    [hash:30 | status:2]
 *        LOCK_WORD_INFLATED:    [sync:30 | status:2]
 *        LOCK_WORD_FAT_HASH:    [sync:30 | status:2]
 *
 * 64-bit
 *            LOCK_WORD_FLAT:    [unused:22 | owner:32 | nest:8 | status:2]
 *       LOCK_WORD_THIN_HASH:    [hash:62 | status:2]
 *        LOCK_WORD_INFLATED:    [sync:62 | status:2]
 *        LOCK_WORD_FAT_HASH:    [sync:62 | status:2]
 *
 * In order to save processing time and to have one additional value, the nest
 * count starts from 0 for the lock word (just valid thread ID in the lock word
 * means that the thread holds the lock once, although nest is 0).
 * FIXME Have the same convention on inflated locks
 */

typedef union {
#if SIZEOF_REGISTER == 8
	guint64 lock_word;
#elif SIZEOF_REGISTER == 4
	guint32 lock_word;
#endif
	MonoThreadsSync *sync;
} LockWord;


enum {
	LOCK_WORD_FLAT = 0,
	LOCK_WORD_THIN_HASH = 1,
	LOCK_WORD_INFLATED = 2,
	LOCK_WORD_FAT_HASH = 3,

	LOCK_WORD_STATUS_BITS = 2,
	LOCK_WORD_NEST_BITS = 8,

	LOCK_WORD_STATUS_MASK = (1 << LOCK_WORD_STATUS_BITS) - 1,
	LOCK_WORD_NEST_MASK = ((1 << LOCK_WORD_NEST_BITS) - 1) << LOCK_WORD_STATUS_BITS,

	LOCK_WORD_HASH_SHIFT = LOCK_WORD_STATUS_BITS,
	LOCK_WORD_NEST_SHIFT = LOCK_WORD_STATUS_BITS,
	LOCK_WORD_OWNER_SHIFT = LOCK_WORD_STATUS_BITS + LOCK_WORD_NEST_BITS
};

MONO_API void mono_locks_dump (gboolean include_untaken);

void mono_monitor_init (void) MONO_INTERNAL;
void mono_monitor_cleanup (void) MONO_INTERNAL;

void** mono_monitor_get_object_monitor_weak_link (MonoObject *object) MONO_INTERNAL;

void mono_monitor_init_tls (void) MONO_INTERNAL;

void mono_monitor_threads_sync_members_offset (int *owner_offset, int *nest_offset, int *entry_count_offset) MONO_INTERNAL;
#define MONO_THREADS_SYNC_MEMBER_OFFSET(o)	((o)>>8)
#define MONO_THREADS_SYNC_MEMBER_SIZE(o)	((o)&0xff)

extern gboolean ves_icall_System_Threading_Monitor_Monitor_try_enter(MonoObject *obj, guint32 ms) MONO_INTERNAL;
extern gboolean ves_icall_System_Threading_Monitor_Monitor_test_owner(MonoObject *obj) MONO_INTERNAL;
extern gboolean ves_icall_System_Threading_Monitor_Monitor_test_synchronised(MonoObject *obj) MONO_INTERNAL;
extern void ves_icall_System_Threading_Monitor_Monitor_pulse(MonoObject *obj) MONO_INTERNAL;
extern void ves_icall_System_Threading_Monitor_Monitor_pulse_all(MonoObject *obj) MONO_INTERNAL;
extern gboolean ves_icall_System_Threading_Monitor_Monitor_wait(MonoObject *obj, guint32 ms) MONO_INTERNAL;
extern void ves_icall_System_Threading_Monitor_Monitor_try_enter_with_atomic_var (MonoObject *obj, guint32 ms, char *lockTaken) MONO_INTERNAL;

G_END_DECLS

#endif /* _MONO_METADATA_MONITOR_H_ */
