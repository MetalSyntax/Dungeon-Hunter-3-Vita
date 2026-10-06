/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022      GrapheneCt
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/pthr.h"

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <stdatomic.h>

#include "utils/utils.h"
#include "utils/logger.h"


#define BIONIC_PTHREAD_COND_INITIALIZER              0
#define BIONIC_PTHREAD_MUTEX_INITIALIZER             0
#define BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER   0x4000
#define BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER  0x8000

enum {
    BIONIC_PTHREAD_MUTEX_NORMAL = 0,
    BIONIC_PTHREAD_MUTEX_RECURSIVE = 1,
    BIONIC_PTHREAD_MUTEX_ERRORCHECK = 2,

    BIONIC_PTHREAD_MUTEX_ERRORCHECK_NP = BIONIC_PTHREAD_MUTEX_ERRORCHECK,
    BIONIC_PTHREAD_MUTEX_RECURSIVE_NP  = BIONIC_PTHREAD_MUTEX_RECURSIVE,

    BIONIC_PTHREAD_MUTEX_DEFAULT = BIONIC_PTHREAD_MUTEX_NORMAL
};

#define PTHR_INLINE static inline __attribute__((always_inline))

/*
 * Bionic mutex/cond objects are a single 4-byte word; we keep a pointer to
 * the real newlib (pthread-embedded) object IN that word. The word starts as
 * a bionic static initializer (0 / 0x4000 / 0x8000), so "is a real object"
 * is simply "looks like a heap pointer".
 *
 * This replaces a global 1024-slot registry (2026-10-04, log_016..019): once
 * the engine had >1024 live mutexes/conds, rememberObject() failed silently
 * and every lock() malloc'd a brand-new mutex over real_ptr, so no lock
 * excluded anything; lazy init was check-then-act (two threads could each
 * install their own mutex); and a new object at a recycled address was seen
 * as "initialized" with real_ptr == NULL. With glitch's loader threads that
 * meant races on refcounts/strings -> freed .bdae names, zero-filled skins,
 * constructGeometry crash. Lazy init is now a CAS on the word itself.
 */
#define PTHR_IS_OBJ(v) ((uintptr_t) (v) >= 0x10000u)

PTHR_INLINE int _bionic_mutex_kind(uintptr_t word, const pthread_mutexattr_t *attr) {
    int kind = PTHREAD_MUTEX_NORMAL;
    if (attr) {
        pthread_mutexattr_gettype((pthread_mutexattr_t *) attr, &kind);
    } else if (word == BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER) {
        kind = PTHREAD_MUTEX_RECURSIVE;
    } else if (word == BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER) {
        kind = PTHREAD_MUTEX_ERRORCHECK;
    }
    return kind;
}

static pthread_mutex_t *_new_mutex(int kind) {
    pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
    if (!m) return NULL;
    pthread_mutexattr_t mutattr;
    pthread_mutexattr_init(&mutattr);
    pthread_mutexattr_settype(&mutattr, kind);
    int ret = pthread_mutex_init(m, &mutattr);
    pthread_mutexattr_destroy(&mutattr);
    if (ret != 0) {
        l_error("pthread_mutex_init failed (%d)", ret);
        free(m);
        return NULL;
    }
    return m;
}

static pthread_cond_t *_new_cond(const pthread_condattr_t *attr) {
    pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
    if (!c) return NULL;
    if (pthread_cond_init(c, attr) != 0) {
        l_error("pthread_cond_init failed");
        free(c);
        return NULL;
    }
    return c;
}

// Lazy (static-initializer) path: install a real mutex exactly once.
PTHR_INLINE pthread_mutex_t *_mutex_get(pthread_mutex_t_bionic *mutex) {
    uintptr_t *w = (uintptr_t *) &mutex->real_ptr;
    uintptr_t cur = __atomic_load_n(w, __ATOMIC_ACQUIRE);
    if (PTHR_IS_OBJ(cur)) return (pthread_mutex_t *) cur;
    pthread_mutex_t *m = _new_mutex(_bionic_mutex_kind(cur, NULL));
    if (!m) return NULL;
    if (__atomic_compare_exchange_n(w, &cur, (uintptr_t) m, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return m;
    // Another thread won the race: use its mutex.
    pthread_mutex_destroy(m);
    free(m);
    return PTHR_IS_OBJ(cur) ? (pthread_mutex_t *) cur : NULL;
}

PTHR_INLINE pthread_cond_t *_cond_get(pthread_cond_t_bionic *cond) {
    uintptr_t *w = (uintptr_t *) &cond->real_ptr;
    uintptr_t cur = __atomic_load_n(w, __ATOMIC_ACQUIRE);
    if (PTHR_IS_OBJ(cur)) return (pthread_cond_t *) cur;
    pthread_cond_t *c = _new_cond(NULL);
    if (!c) return NULL;
    if (__atomic_compare_exchange_n(w, &cur, (uintptr_t) c, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return c;
    pthread_cond_destroy(c);
    free(c);
    return PTHR_IS_OBJ(cur) ? (pthread_cond_t *) cur : NULL;
}

// null check for `attr` must be performed before this
PTHR_INLINE int _attr_t_static_init(pthread_attr_t_bionic * attr) {
    if (attr->magic != 0x42424242) {
        attr->magic = 0x42424242;
        attr->real_ptr = malloc(sizeof(pthread_attr_t));
        return pthread_attr_init(attr->real_ptr);
    }
    return 0;
}

int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr, void *(*start)(void *), void *param) {
    int ret;

    if (!attr) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 512 * 1024);
        ret = pthread_create(thread, &a, start, param);
        pthread_attr_destroy(&a);
    } else{
        _attr_t_static_init((pthread_attr_t_bionic *) attr);
        pthread_attr_setstacksize(attr->real_ptr, 512 * 1024);
        ret = pthread_create(thread, attr->real_ptr, start, param);
    }

    return ret;
}

int pthread_mutexattr_init_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_init(attr);
}

int pthread_mutexattr_settype_soloader(pthread_mutexattr_t *attr, int type)
{
    return pthread_mutexattr_settype(attr, type);
}

int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_destroy(attr);
}

int pthread_kill_soloader(pthread_t thread, int sig)
{
    return pthread_kill(thread, sig);
}

int pthread_mutex_init_soloader(pthread_mutex_t_bionic *uid, const pthread_mutexattr_t *attr)
{
    if (!uid) return EINVAL;
    // Explicit init runs on fresh (possibly recycled, garbage-filled) memory:
    // always install a new object, never trust what the word holds.
    pthread_mutex_t *m = _new_mutex(_bionic_mutex_kind(0, attr));
    if (!m) return ENOMEM;
    __atomic_store_n((uintptr_t *) &uid->real_ptr, (uintptr_t) m, __ATOMIC_RELEASE);
    return 0;
}

int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return 0;
    uintptr_t v = __atomic_exchange_n((uintptr_t *) &mutex->real_ptr, 0, __ATOMIC_ACQ_REL);
    if (!PTHR_IS_OBJ(v)) return 0;
    int ret = pthread_mutex_destroy((pthread_mutex_t *) v);
    free((void *) v);
    return ret;
}

int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    pthread_mutex_t *m = _mutex_get(mutex);
    return m ? pthread_mutex_lock(m) : EINVAL;
}

int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    pthread_mutex_t *m = _mutex_get(mutex);
    return m ? pthread_mutex_trylock(m) : EINVAL;
}

int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    uintptr_t v = __atomic_load_n((uintptr_t *) &mutex->real_ptr, __ATOMIC_ACQUIRE);
    if (!PTHR_IS_OBJ(v)) return EINVAL;
    return pthread_mutex_unlock((pthread_mutex_t *) v);
}

int pthread_join_soloader(pthread_t thread, void **value_ptr)
{
    return pthread_join(thread, value_ptr);
}

int pthread_condattr_init_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_init(attr);
}

int pthread_condattr_destroy_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_destroy(attr);
}

int pthread_cond_init_soloader(pthread_cond_t_bionic *cond,
                               const pthread_condattr_t *attr)
{
    if (!cond) return EINVAL;
    pthread_cond_t *c = _new_cond(attr);
    if (!c) return ENOMEM;
    __atomic_store_n((uintptr_t *) &cond->real_ptr, (uintptr_t) c, __ATOMIC_RELEASE);
    return 0;
}

int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return 0;
    uintptr_t v = __atomic_exchange_n((uintptr_t *) &cond->real_ptr, 0, __ATOMIC_ACQ_REL);
    if (!PTHR_IS_OBJ(v)) return 0;
    int ret = pthread_cond_destroy((pthread_cond_t *) v);
    free((void *) v);
    return ret;
}

int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;
    pthread_cond_t *c = _cond_get(cond);
    return c ? pthread_cond_signal(c) : EINVAL;
}

int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex, struct timespec *abstime)
{
    if (!cond || !mutex) return EINVAL;

    pthread_cond_t *c = _cond_get(cond);
    pthread_mutex_t *m = _mutex_get(mutex);
    return (c && m) ? pthread_cond_timedwait(c, m, abstime) : EINVAL;
}


int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex)
{
    if (!cond || !mutex) return EINVAL;

    pthread_cond_t *c = _cond_get(cond);
    pthread_mutex_t *m = _mutex_get(mutex);
    return (c && m) ? pthread_cond_wait(c, m) : EINVAL;
}

int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;
    pthread_cond_t *c = _cond_get(cond);
    return c ? pthread_cond_broadcast(c) : EINVAL;
}

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return EINVAL;

    return _attr_t_static_init(attr);
}

int pthread_attr_destroy_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return 0;
    if (attr->magic != 0x42424242) return 0;

    int ret = pthread_attr_destroy(attr->real_ptr);
    free(attr->real_ptr);
    attr->magic = 0x0;

    return ret;
}

int pthread_attr_setdetachstate_soloader(pthread_attr_t_bionic *attr, int state)
{
    if (!attr) return -1;
    _attr_t_static_init(attr);
    return pthread_attr_setdetachstate(attr->real_ptr, state);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize) {
    if (!attr) return -1;
    _attr_t_static_init(attr);
    return pthread_attr_setstacksize(attr->real_ptr, stacksize);
}

int pthread_setschedparam_soloader(pthread_t thread, int policy,
                                   const struct sched_param *param)
{
   return pthread_setschedparam(thread, policy, param);
}

int pthread_getschedparam_soloader(pthread_t thread, int *policy,
                                   struct sched_param *param)
{
    return pthread_getschedparam(thread, policy, param);
}

int pthread_detach_soloader(pthread_t thread)
{
    return pthread_detach(thread);
}

int pthread_equal_soloader(const pthread_t t1, const pthread_t t2)
{
    if (t1 == t2)
        return 1;
    if (!t1 || !t2)
        return 0;
    return pthread_equal(t1, t2);
}

pthread_t pthread_self_soloader()
{
    return pthread_self();
}

int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine)
        return -1;
    // 0 = not run, 1 = running, 2 = done. Late callers must wait for the
    // routine to finish, not just for it to start (bionic semantics).
    int expected = 0;
    if (__atomic_compare_exchange_n(once_control, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        (*init_routine)();
        __atomic_store_n(once_control, 2, __ATOMIC_RELEASE);
        return 0;
    }
    while (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) != 2)
        sceKernelDelayThread(100);
    return 0;
}

#ifndef MAX_TASK_COMM_LEN
#define MAX_TASK_COMM_LEN 16
#endif

int pthread_setname_np_soloader(pthread_t thread, const char* thread_name) {
    if (thread == 0 || thread_name == NULL) {
        return EINVAL;
    }
    size_t thread_name_len = strlen(thread_name);
    if (thread_name_len >= MAX_TASK_COMM_LEN) {
        return ERANGE;
    }

    sceClibPrintf("PTHREAD: pthread_setname_np with name %s for thread:0x%x\n", thread_name, pthread_self());

    return 0;
}

int sem_destroy_soloader(int * uid) {
    if (sceKernelDeleteSema(*uid) < 0)
        return -1;
    return 0;
}

int sem_getvalue_soloader (int * uid, int * sval) {
    SceKernelSemaInfo info;
    info.size = sizeof(SceKernelSemaInfo);

    if (sceKernelGetSemaInfo(*uid, &info) < 0) return -1;
    if (sval) *sval = info.currentCount;
    return 0;
}

int sem_init_soloader (int * uid, int pshared, unsigned int value) {
    *uid = sceKernelCreateSema("sema", 0, (int) value, 0x7fffffff, NULL);
    if (*uid < 0)
        return -1;
    return 0;
}

int sem_post_soloader (int * uid) {
    if (sceKernelSignalSema(*uid, 1) < 0)
        return -1;
    return 0;
}

int sem_timedwait_soloader (int * uid, const struct timespec * abstime) {
    // abstime is CLOCK_REALTIME (gettimeofday-based like current_timestamp_ms).
    // The old version returned -1 whenever time was still LEFT (inverted test).
    long long remaining = 0;
    if (abstime) {
        long long now = (long long) current_timestamp_ms() * 1000; // us
        long long deadline = (long long) abstime->tv_sec * 1000000LL + abstime->tv_nsec / 1000;
        remaining = deadline - now;
    }
    SceUInt timeout = remaining > 0 ? (remaining > 0x7FFFFFFF ? 0x7FFFFFFF : (SceUInt) remaining) : 0;
    if (sceKernelWaitSema(*uid, 1, &timeout) >= 0)
        return 0;
    errno = ETIMEDOUT;
    return -1;
}

int sem_trywait_soloader (int * uid) {
    // Must not block (the old version waited 1 ms).
    if (sceKernelPollSema(*uid, 1) < 0) {
        errno = EAGAIN;
        return -1;
    }
    return 0;
}

int sem_wait_soloader (int * uid) {
    if (sceKernelWaitSema(*uid, 1, NULL) < 0)
        return -1;
    return 0;
}
