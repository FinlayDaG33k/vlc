/*****************************************************************************
 * threads.c: LibVLC generic thread support
 *****************************************************************************
 * Copyright (C) 2009-2016 Rémi Denis-Courmont
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>

#include <vlc_common.h>
#include "libvlc.h"

/* <stdatomic.h> types cannot be used in the C++ view of <vlc_threads.h> */
struct vlc_suuint { union { unsigned int value; }; };

static_assert (sizeof (atomic_uint) <= sizeof (struct vlc_suuint),
               "Size mismatch");
static_assert (alignof (atomic_uint) <= alignof (struct vlc_suuint),
               "Alignment mismatch");

/*** Global locks ***/

void vlc_global_mutex (unsigned n, bool acquire)
{
    static vlc_mutex_t locks[] = {
        VLC_STATIC_MUTEX,
        VLC_STATIC_MUTEX,
        VLC_STATIC_MUTEX,
        VLC_STATIC_MUTEX,
#ifdef _WIN32
        VLC_STATIC_MUTEX, // For MTA holder
#endif
    };
    static_assert (VLC_MAX_MUTEX == (sizeof (locks) / sizeof (locks[0])),
                   "Wrong number of global mutexes");
    assert (n < (sizeof (locks) / sizeof (locks[0])));

    vlc_mutex_t *lock = locks + n;
    if (acquire)
        vlc_mutex_lock (lock);
    else
        vlc_mutex_unlock (lock);
}

#ifndef NDEBUG
# ifdef HAVE_SEARCH_H
#  include <search.h>
# endif

struct vlc_lock_mark
{
    const void *object;
    uintptr_t refs;
};

static int vlc_lock_mark_cmp(const void *a, const void *b)
{
    const struct vlc_lock_mark *ma = a, *mb = b;

    if (ma->object == mb->object)
        return 0;

    return ((uintptr_t)(ma->object) > (uintptr_t)(mb->object)) ? +1 : -1;
}

static void vlc_lock_mark(const void *lock, void **rootp)
{
    struct vlc_lock_mark *mark = malloc(sizeof (*mark));
    if (unlikely(mark == NULL))
        abort();

    mark->object = lock;
    mark->refs = 0;

    void **entry = tsearch(mark, rootp, vlc_lock_mark_cmp);
    if (unlikely(entry == NULL))
        abort();

    if (unlikely(*entry != mark)) {
        /* Recursive locking: lock is already in the tree */
        free(mark);
        mark = *entry;
    }

    mark->refs++;
}

static void vlc_lock_unmark(const void *lock, void **rootp)
{
    struct vlc_lock_mark *mark = &(struct vlc_lock_mark){ lock, 0 };
    void **entry = tfind(mark, rootp, vlc_lock_mark_cmp);

    assert(entry != NULL);
    mark = *entry;
    assert(mark->refs > 0);

    if (likely(--mark->refs == 0)) {
        tdelete(mark, rootp, vlc_lock_mark_cmp);
        free(mark);
    }
}

static bool vlc_lock_marked(const void *lock, void **rootp)
{
    struct vlc_lock_mark *mark = &(struct vlc_lock_mark){ lock, 0 };

    return tfind(mark, rootp, vlc_lock_mark_cmp) != NULL;
}

static _Thread_local void *vlc_mutex_marks = NULL;

void vlc_mutex_mark(const vlc_mutex_t *mutex)
{
    vlc_lock_mark(mutex, &vlc_mutex_marks);
}

void vlc_mutex_unmark(const vlc_mutex_t *mutex)
{
    vlc_lock_unmark(mutex, &vlc_mutex_marks);
}

bool vlc_mutex_marked(const vlc_mutex_t *mutex)
{
    return vlc_lock_marked(mutex, &vlc_mutex_marks);
}
#else
bool vlc_mutex_marked(const vlc_mutex_t *mutex)
{
    return true;
}
#endif

#if defined (_WIN32) && (_WIN32_WINNT < _WIN32_WINNT_WIN8)
/* Cannot define OS version-dependent stuff in public headers */
# undef LIBVLC_NEED_SLEEP
#endif

#if defined(LIBVLC_NEED_SLEEP) || defined(LIBVLC_NEED_CONDVAR)
#include <stdatomic.h>

static void do_vlc_cancel_addr_clear(void *addr)
{
    vlc_cancel_addr_clear(addr);
}

static void vlc_cancel_addr_prepare(atomic_uint *addr)
{
    /* Let thread subsystem on address to broadcast for cancellation */
    vlc_cancel_addr_set(addr);
    vlc_cleanup_push(do_vlc_cancel_addr_clear, addr);
    /* Check if cancellation was pending before vlc_cancel_addr_set() */
    vlc_testcancel();
    vlc_cleanup_pop();
}

static void vlc_cancel_addr_finish(atomic_uint *addr)
{
    vlc_cancel_addr_clear(addr);
    /* Act on cancellation as potential wake-up source */
    vlc_testcancel();
}
#endif

#ifdef LIBVLC_NEED_SLEEP
void (vlc_tick_wait)(vlc_tick_t deadline)
{
    atomic_uint value = ATOMIC_VAR_INIT(0);

    vlc_cancel_addr_prepare(&value);

    while (vlc_atomic_timedwait(&value, 0, deadline) == 0)
        vlc_testcancel();

    vlc_cancel_addr_finish(&value);
}

void (vlc_tick_sleep)(vlc_tick_t delay)
{
    vlc_tick_wait(vlc_tick_now() + delay);
}
#endif

#ifdef LIBVLC_NEED_CONDVAR
void vlc_cond_init(vlc_cond_t *cond)
{
    cond->head = NULL;
    vlc_mutex_init(&cond->lock);
}

void vlc_cond_init_daytime(vlc_cond_t *cond)
{
    vlc_cond_init(cond);
}

void vlc_cond_destroy(vlc_cond_t *cond)
{
    assert(cond->head == NULL);
    vlc_mutex_destroy(&cond->lock);
}

struct vlc_cond_waiter {
    struct vlc_cond_waiter **pprev, *next;
    atomic_uint value;
    vlc_cond_t *cond;
    vlc_mutex_t *mutex;
};

static void vlc_cond_signal_waiter(struct vlc_cond_waiter *waiter)
{
    waiter->pprev = &waiter->next;
    waiter->next = NULL;
    atomic_fetch_add_explicit(&waiter->value, 1, memory_order_relaxed);
    vlc_atomic_notify_one(&waiter->value);
}

void vlc_cond_signal(vlc_cond_t *cond)
{
    struct vlc_cond_waiter *waiter;

    /* Some call sites signal their condition variable without holding the
     * corresponding lock. Thus an extra lock is needed here to ensure the
     * consistency of the linked list and the lifetime of its elements.
     * If all call sites locked cleanly, the inner lock would be unnecessary.
     */
    vlc_mutex_lock(&cond->lock);
    waiter = cond->head;

    if (waiter != NULL) {
        struct vlc_cond_waiter *next = waiter->next;
        struct vlc_cond_waiter **pprev = waiter->pprev;

        *pprev = next;

        if (next != NULL)
            next->pprev = pprev;

        vlc_cond_signal_waiter(waiter);
    }

    vlc_mutex_unlock(&cond->lock);
}

void vlc_cond_broadcast(vlc_cond_t *cond)
{
    struct vlc_cond_waiter *waiter;

    vlc_mutex_lock(&cond->lock);
    waiter = cond->head;
    cond->head = NULL;

    /* Keep the lock here so that waiters don't go out of scope */
    while (waiter != NULL) {
        struct vlc_cond_waiter *next = waiter->next;

        vlc_cond_signal_waiter(waiter);
        waiter = next;
    }

    vlc_mutex_unlock(&cond->lock);
}

static void vlc_cond_wait_prepare(struct vlc_cond_waiter *waiter,
                                  vlc_cond_t *cond, vlc_mutex_t *mutex)
{
    struct vlc_cond_waiter *next;

    waiter->pprev = &cond->head;
    atomic_init(&waiter->value, 0);
    waiter->cond = cond;
    waiter->mutex = mutex;

    vlc_mutex_lock(&cond->lock);
    next = cond->head;
    cond->head = waiter;
    waiter->next = next;

    if (next != NULL)
        next->pprev = &waiter->next;

    vlc_mutex_unlock(&cond->lock);
    vlc_cancel_addr_prepare(&waiter->value);
    vlc_mutex_unlock(mutex);
}

static void vlc_cond_wait_finish(struct vlc_cond_waiter *waiter,
                                 vlc_cond_t *cond, vlc_mutex_t *mutex)
{
    struct vlc_cond_waiter *next;

    /* If this waiter is still on the linked list, remove it before it goes
     * out of scope. Otherwise, this is a no-op.
     */
    vlc_mutex_lock(&cond->lock);
    next = waiter->next;
    *(waiter->pprev) = next;

    if (next != NULL)
        next->pprev = waiter->pprev;

    vlc_mutex_unlock(&cond->lock);

    /* Lock the caller's mutex as required by condition variable semantics. */
    vlc_mutex_lock(mutex);
    vlc_cancel_addr_finish(&waiter->value);
}

static void vlc_cond_wait_cleanup(void *data)
{
    struct vlc_cond_waiter *waiter = data;

    vlc_cond_wait_finish(waiter, waiter->cond, waiter->mutex);
}

void vlc_cond_wait(vlc_cond_t *cond, vlc_mutex_t *mutex)
{
    struct vlc_cond_waiter waiter;

    vlc_cond_wait_prepare(&waiter, cond, mutex);
    vlc_cleanup_push(vlc_cond_wait_cleanup, &waiter);
    vlc_atomic_wait(&waiter.value, 0);
    vlc_cleanup_pop();
    vlc_cond_wait_cleanup(&waiter);
}

int vlc_cond_timedwait(vlc_cond_t *cond, vlc_mutex_t *mutex,
                       vlc_tick_t deadline)
{
    struct vlc_cond_waiter waiter;
    int ret;

    vlc_cond_wait_prepare(&waiter, cond, mutex);
    vlc_cleanup_push(vlc_cond_wait_cleanup, &waiter);
    ret = vlc_atomic_timedwait(&waiter.value, 0, deadline);
    vlc_cleanup_pop();
    vlc_cond_wait_cleanup(&waiter);

    return ret;
}

int vlc_cond_timedwait_daytime(vlc_cond_t *cond, vlc_mutex_t *mutex,
                               time_t deadline)
{
    struct vlc_cond_waiter waiter;
    int ret;

    vlc_cond_wait_prepare(&waiter, cond, mutex);
    vlc_cleanup_push(vlc_cond_wait_cleanup, &waiter);
    ret = vlc_atomic_timedwait_daytime(&waiter.value, 0, deadline);
    vlc_cleanup_pop();
    vlc_cond_wait_cleanup(&waiter);

    return ret;
}
#endif

#ifdef LIBVLC_NEED_RWLOCK
/*** Generic read/write locks ***/
#include <stdlib.h>
#include <limits.h>
/* NOTE:
 * lock->state is a signed long integer:
 *  - The sign bit is set when the lock is held for writing.
 *  - The other bits code the number of times the lock is held for reading.
 * Consequently:
 *  - The value is negative if and only if the lock is held for writing.
 *  - The value is zero if and only if the lock is not held at all.
 */
#define READER_MASK LONG_MAX
#define WRITER_BIT  LONG_MIN

void vlc_rwlock_init (vlc_rwlock_t *lock)
{
    vlc_mutex_init (&lock->mutex);
    vlc_cond_init (&lock->wait);
    lock->state = 0;
}

void vlc_rwlock_destroy (vlc_rwlock_t *lock)
{
    vlc_cond_destroy (&lock->wait);
    vlc_mutex_destroy (&lock->mutex);
}

void vlc_rwlock_rdlock (vlc_rwlock_t *lock)
{
    vlc_mutex_lock (&lock->mutex);
    /* Recursive read-locking is allowed.
     * Ensure that there is no active writer. */
    while (lock->state < 0)
    {
        assert (lock->state == WRITER_BIT);
        mutex_cleanup_push (&lock->mutex);
        vlc_cond_wait (&lock->wait, &lock->mutex);
        vlc_cleanup_pop ();
    }
    if (unlikely(lock->state >= READER_MASK))
        abort (); /* An overflow is certainly a recursion bug. */
    lock->state++;
    vlc_mutex_unlock (&lock->mutex);
}

void vlc_rwlock_wrlock (vlc_rwlock_t *lock)
{
    vlc_mutex_lock (&lock->mutex);
    /* Wait until nobody owns the lock in any way. */
    while (lock->state != 0)
    {
        mutex_cleanup_push (&lock->mutex);
        vlc_cond_wait (&lock->wait, &lock->mutex);
        vlc_cleanup_pop ();
    }
    lock->state = WRITER_BIT;
    vlc_mutex_unlock (&lock->mutex);
}

void vlc_rwlock_unlock (vlc_rwlock_t *lock)
{
    vlc_mutex_lock (&lock->mutex);
    if (lock->state < 0)
    {   /* Write unlock */
        assert (lock->state == WRITER_BIT);
        /* Let reader and writer compete. OS scheduler decides who wins. */
        lock->state = 0;
        vlc_cond_broadcast (&lock->wait);
    }
    else
    {   /* Read unlock */
        assert (lock->state > 0);
        /* If there are no readers left, wake up one pending writer. */
        if (--lock->state == 0)
            vlc_cond_signal (&lock->wait);
    }
    vlc_mutex_unlock (&lock->mutex);
}
#endif /* LIBVLC_NEED_RWLOCK */

/*** Generic semaphores ***/

void vlc_sem_init (vlc_sem_t *sem, unsigned value)
{
    atomic_init(&sem->value, value);
}

int vlc_sem_post (vlc_sem_t *sem)
{
    unsigned exp = atomic_load_explicit(&sem->value, memory_order_relaxed);

    do
    {
        if (unlikely(exp == UINT_MAX))
           return EOVERFLOW;
    } while (!atomic_compare_exchange_weak_explicit(&sem->value, &exp, exp + 1,
                                                    memory_order_release,
                                                    memory_order_relaxed));

    vlc_atomic_notify_one(&sem->value);
    return 0;
}

void vlc_sem_wait (vlc_sem_t *sem)
{
    unsigned exp = 1;

    while (!atomic_compare_exchange_weak_explicit(&sem->value, &exp, exp - 1,
                                                  memory_order_acquire,
                                                  memory_order_relaxed))
    {
        if (likely(exp == 0))
        {
            vlc_atomic_wait(&sem->value, 0);
            exp = 1;
        }
    }
}

int vlc_sem_timedwait(vlc_sem_t *sem, vlc_tick_t deadline)
{
    unsigned exp = 1;

    while (!atomic_compare_exchange_weak_explicit(&sem->value, &exp, exp - 1,
                                                  memory_order_acquire,
                                                  memory_order_relaxed))
    {
        if (likely(exp == 0))
        {
            int ret = vlc_atomic_timedwait(&sem->value, 0, deadline);
            if (ret)
                return ret;

            exp = 1;
        }
    }

    return 0;
}
