// Copyright 2026 the adbcbridge authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

/// The few threading primitives the prefetch pipeline (odbc_reader.c) and the
/// parallel ingest pool (odbc_bind.c) need, spelled the same way on every
/// platform: a mutex, a condition variable and a joinable thread.  POSIX gets
/// pthreads; Windows gets SRWLOCK, CONDITION_VARIABLE and _beginthreadex, which
/// is why those two features are no longer compiled out there.
///
/// Semantics are the pthread ones, with one deliberate difference: a mutex or
/// condition variable needs no destroy on Windows, so the destroy calls are
/// no-ops there and callers keep pairing them with init as they would for
/// pthreads.

#pragma once

#include <stdint.h>
#include <stdlib.h>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>

typedef SRWLOCK OdbcMutex;
typedef CONDITION_VARIABLE OdbcCond;
typedef HANDLE OdbcThread;

static inline int OdbcMutexInit(OdbcMutex* m) { InitializeSRWLock(m); return 0; }
static inline void OdbcMutexDestroy(OdbcMutex* m) { (void)m; }
static inline void OdbcMutexLock(OdbcMutex* m) { AcquireSRWLockExclusive(m); }
static inline void OdbcMutexUnlock(OdbcMutex* m) { ReleaseSRWLockExclusive(m); }

static inline int OdbcCondInit(OdbcCond* c) { InitializeConditionVariable(c); return 0; }
static inline void OdbcCondDestroy(OdbcCond* c) { (void)c; }
static inline void OdbcCondWait(OdbcCond* c, OdbcMutex* m) {
  SleepConditionVariableSRW(c, m, INFINITE, 0);
}
static inline void OdbcCondSignal(OdbcCond* c) { WakeConditionVariable(c); }
static inline void OdbcCondBroadcast(OdbcCond* c) { WakeAllConditionVariable(c); }

typedef void* (*OdbcThreadFn)(void*);

// _beginthreadex wants `unsigned __stdcall (*)(void*)`; carry the pthread-shaped
// entry point and its argument across in a heap cell the new thread frees.
struct OdbcThreadStart {
  OdbcThreadFn fn;
  void* arg;
};

static unsigned __stdcall OdbcThreadTrampoline(void* p) {
  struct OdbcThreadStart start = *(struct OdbcThreadStart*)p;
  free(p);
  start.fn(start.arg);
  return 0;
}

/// 0 on success, like pthread_create; the thread must be joined.
static inline int OdbcThreadCreate(OdbcThread* t, OdbcThreadFn fn, void* arg) {
  struct OdbcThreadStart* start = malloc(sizeof(*start));
  if (!start) return -1;
  start->fn = fn;
  start->arg = arg;
  uintptr_t h = _beginthreadex(NULL, 0, OdbcThreadTrampoline, start, 0, NULL);
  if (h == 0) {
    free(start);
    return -1;
  }
  *t = (HANDLE)h;
  return 0;
}

static inline void OdbcThreadJoin(OdbcThread t) {
  WaitForSingleObject(t, INFINITE);
  CloseHandle(t);
}

#else  // POSIX

#include <pthread.h>

typedef pthread_mutex_t OdbcMutex;
typedef pthread_cond_t OdbcCond;
typedef pthread_t OdbcThread;
typedef void* (*OdbcThreadFn)(void*);

static inline int OdbcMutexInit(OdbcMutex* m) { return pthread_mutex_init(m, NULL); }
static inline void OdbcMutexDestroy(OdbcMutex* m) { pthread_mutex_destroy(m); }
static inline void OdbcMutexLock(OdbcMutex* m) { pthread_mutex_lock(m); }
static inline void OdbcMutexUnlock(OdbcMutex* m) { pthread_mutex_unlock(m); }

static inline int OdbcCondInit(OdbcCond* c) { return pthread_cond_init(c, NULL); }
static inline void OdbcCondDestroy(OdbcCond* c) { pthread_cond_destroy(c); }
static inline void OdbcCondWait(OdbcCond* c, OdbcMutex* m) { pthread_cond_wait(c, m); }
static inline void OdbcCondSignal(OdbcCond* c) { pthread_cond_signal(c); }
static inline void OdbcCondBroadcast(OdbcCond* c) { pthread_cond_broadcast(c); }

static inline int OdbcThreadCreate(OdbcThread* t, OdbcThreadFn fn, void* arg) {
  return pthread_create(t, NULL, fn, arg);
}
static inline void OdbcThreadJoin(OdbcThread t) { pthread_join(t, NULL); }

#endif
