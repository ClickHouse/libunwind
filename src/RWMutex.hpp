//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//
// Abstract interface to shared reader/writer log, hiding platform and
// configuration differences.
//
//===----------------------------------------------------------------------===//

#ifndef __RWMUTEX_HPP__
#define __RWMUTEX_HPP__

#if defined(_WIN32)
#include <windows.h>
#elif !defined(_LIBUNWIND_HAS_NO_THREADS)
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sched.h>
#if defined(__ELF__) && defined(_LIBUNWIND_LINK_PTHREAD_LIB)
#pragma comment(lib, "pthread")
#endif
#endif

namespace libunwind {

#if defined(_LIBUNWIND_HAS_NO_THREADS)

class _LIBUNWIND_HIDDEN RWMutex {
public:
  bool lock_shared() { return true; }
  bool unlock_shared() { return true; }
  bool lock() { return true; }
  bool unlock() { return true; }
};

class _LIBUNWIND_HIDDEN LockFreeRWMutex {
public:
  bool lock_shared() { return true; }
  bool unlock_shared() { return true; }
  bool lock() { return true; }
  bool unlock() { return true; }
};

#elif defined(_WIN32)

class _LIBUNWIND_HIDDEN RWMutex {
public:
  bool lock_shared() {
    AcquireSRWLockShared(&_lock);
    return true;
  }
  bool unlock_shared() {
    ReleaseSRWLockShared(&_lock);
    return true;
  }
  bool lock() {
    AcquireSRWLockExclusive(&_lock);
    return true;
  }
  bool unlock() {
    ReleaseSRWLockExclusive(&_lock);
    return true;
  }

private:
  SRWLOCK _lock = SRWLOCK_INIT;
};

#elif !defined(LIBUNWIND_USE_WEAK_PTHREAD)

class _LIBUNWIND_HIDDEN RWMutex {
public:
  bool lock_shared() { return pthread_rwlock_rdlock(&_lock) == 0;  }
  bool unlock_shared() { return pthread_rwlock_unlock(&_lock) == 0; }
  bool lock() { return pthread_rwlock_wrlock(&_lock) == 0; }
  bool unlock() { return pthread_rwlock_unlock(&_lock) == 0; }

private:
  pthread_rwlock_t _lock = PTHREAD_RWLOCK_INITIALIZER;
};

#else

extern "C" int __attribute__((weak))
pthread_create(pthread_t *thread, const pthread_attr_t *attr,
               void *(*start_routine)(void *), void *arg);
extern "C" int __attribute__((weak))
pthread_rwlock_rdlock(pthread_rwlock_t *lock);
extern "C" int __attribute__((weak))
pthread_rwlock_wrlock(pthread_rwlock_t *lock);
extern "C" int __attribute__((weak))
pthread_rwlock_unlock(pthread_rwlock_t *lock);

// Calls to the locking functions are gated on pthread_create, and not the
// functions themselves, because the data structure should only be locked if
// another thread has been created. This is what similar libraries do.

class _LIBUNWIND_HIDDEN RWMutex {
public:
  bool lock_shared() {
    return !pthread_create || (pthread_rwlock_rdlock(&_lock) == 0);
  }
  bool unlock_shared() {
    return !pthread_create || (pthread_rwlock_unlock(&_lock) == 0);
  }
  bool lock() {
    return !pthread_create || (pthread_rwlock_wrlock(&_lock) == 0);
  }
  bool unlock() {
    return !pthread_create || (pthread_rwlock_unlock(&_lock) == 0);
  }

private:
  pthread_rwlock_t _lock = PTHREAD_RWLOCK_INITIALIZER;
};

#endif

/// Simple lock-free reader-writer lock with writer preference.
/// It's supposed to be async-signal-safe.
class LockFreeRWMutex {
public:
  LockFreeRWMutex() {
    atomic_init(&state, 0);
    atomic_init(&waiting_writers, 0);
  }

  bool lock_shared() {
    while (true) {
      int current_state = atomic_load(&state);
      if (current_state < 0 || current_state >= INT_MAX ||
          atomic_load(&waiting_writers) > 0) {
        sched_yield();
        continue;
      }

      if (atomic_compare_exchange_weak(&state, &current_state,
                                       current_state + 1)) {
        return true;
      }
      sched_yield();
    }
  }

  bool unlock_shared() {
    int current_state = atomic_fetch_sub(&state, 1);
    if (current_state <= 0) {
      abort();
    }
    return true;
  }

  bool lock() {
    while (true) {
      int current_waiting_writers = atomic_load(&waiting_writers);
      if (current_waiting_writers == INT_MAX) {
        sched_yield();
        continue;
      }

      if (atomic_compare_exchange_weak(&waiting_writers,
                                       &current_waiting_writers,
                                       current_waiting_writers + 1)) {
        break;
      }
      sched_yield();
    }

    while (true) {
      int expected = 0;
      if (atomic_compare_exchange_weak(&state, &expected, -1)) {
        atomic_fetch_sub(&waiting_writers, 1);
        return true;
      }
      sched_yield();
    }
  }

  bool unlock() {
    int current_state = atomic_load(&state);
    if (current_state != -1) {
        abort();
    }
    atomic_store(&state, 0);
    return true;
  }

private:
  // Lock state:
  // - Positive: Number of active readers
  // - Zero: Unlocked
  // - Negative (-1): Writer holding the lock
  atomic_int state;

  // Number of writers waiting for the lock
  atomic_int waiting_writers;
};

} // namespace libunwind

#endif // __RWMUTEX_HPP__
