#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <errno.h>

typedef struct pthread_mutex_t
{
	CRITICAL_SECTION cs;
	int ready;
} pthread_mutex_t;

typedef struct pthread_mutexattr_t
{
	int type;
} pthread_mutexattr_t;

typedef struct pthread_cond_t
{
	CONDITION_VARIABLE cv;
} pthread_cond_t;

typedef pthread_mutex_t pthread_spinlock_t;

typedef struct pthread_t
{
	HANDLE handle;
	DWORD id;
	bool operator==(const pthread_t& other) const { return id == other.id; }
	bool operator!=(const pthread_t& other) const { return id != other.id; }
} pthread_t;

typedef struct sigset_t
{
	int mask;
} sigset_t;

#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1
#ifndef SIG_BLOCK
#define SIG_BLOCK 0
#endif
#ifndef SIG_UNBLOCK
#define SIG_UNBLOCK 1
#endif

inline int sigemptyset(sigset_t* set) { if (set) set->mask = 0; return 0; }
inline int sigaddset(sigset_t* set, int sig) { if (set) set->mask |= sig; return 0; }
inline int pthread_sigmask(int, const sigset_t*, sigset_t*) { return 0; }
inline int sigprocmask(int how, const sigset_t* set, sigset_t* old) { return pthread_sigmask(how, set, old); }
inline int sigwait(const sigset_t*, int* sig) { if (sig) *sig = 0; Sleep(1000); return 0; }

inline int pthread_mutexattr_init(pthread_mutexattr_t* attr) { if (attr) attr->type = PTHREAD_MUTEX_NORMAL; return 0; }
inline int pthread_mutexattr_settype(pthread_mutexattr_t* attr, int type) { if (attr) attr->type = type; return 0; }
inline int pthread_mutexattr_destroy(pthread_mutexattr_t*) { return 0; }

inline int pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t*)
{
	InitializeCriticalSection(&mutex->cs);
	mutex->ready = 1;
	return 0;
}
inline int pthread_mutex_destroy(pthread_mutex_t* mutex)
{
	if (mutex && mutex->ready)
	{
		DeleteCriticalSection(&mutex->cs);
		mutex->ready = 0;
	}
	return 0;
}
inline int pthread_mutex_lock(pthread_mutex_t* mutex) { EnterCriticalSection(&mutex->cs); return 0; }
inline int pthread_mutex_unlock(pthread_mutex_t* mutex) { LeaveCriticalSection(&mutex->cs); return 0; }
inline int pthread_mutex_trylock(pthread_mutex_t* mutex)
{
	return TryEnterCriticalSection(&mutex->cs) ? 0 : EBUSY;
}

inline int pthread_cond_init(pthread_cond_t* cond, const void*)
{
	InitializeConditionVariable(&cond->cv);
	return 0;
}
inline int pthread_cond_destroy(pthread_cond_t*) { return 0; }
inline int pthread_cond_signal(pthread_cond_t* cond) { WakeConditionVariable(&cond->cv); return 0; }
inline int pthread_cond_broadcast(pthread_cond_t* cond) { WakeAllConditionVariable(&cond->cv); return 0; }
inline int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex)
{
	SleepConditionVariableCS(&cond->cv, &mutex->cs, INFINITE);
	return 0;
}

inline int pthread_spin_init(pthread_spinlock_t* lock, int) { return pthread_mutex_init(lock, 0); }
inline int pthread_spin_destroy(pthread_spinlock_t* lock) { return pthread_mutex_destroy(lock); }
inline int pthread_spin_lock(pthread_spinlock_t* lock) { return pthread_mutex_lock(lock); }
inline int pthread_spin_unlock(pthread_spinlock_t* lock) { return pthread_mutex_unlock(lock); }
inline int pthread_spin_trylock(pthread_spinlock_t* lock) { return pthread_mutex_trylock(lock); }

inline int pthread_yield(void) { SwitchToThread(); return 0; }

struct pthread_start_s
{
	void* (*fn)(void*);
	void* arg;
};

inline DWORD WINAPI pthread_start_thunk(LPVOID param)
{
	pthread_start_s* start = (pthread_start_s*)param;
	start->fn(start->arg);
	delete start;
	return 0;
}

inline int pthread_create(pthread_t* thread, const void*, void* (*fn)(void*), void* arg)
{
	pthread_start_s* start = new pthread_start_s();
	start->fn = fn;
	start->arg = arg;
	DWORD id = 0;
	HANDLE handle = CreateThread(NULL, 0, pthread_start_thunk, start, 0, &id);
	if (!handle)
	{
		delete start;
		return -1;
	}
	thread->handle = handle;
	thread->id = id;
	return 0;
}

inline int pthread_detach(pthread_t thread)
{
	if (thread.handle)
	{
		CloseHandle(thread.handle);
	}
	return 0;
}

inline pthread_t pthread_self(void)
{
	pthread_t self;
	self.handle = NULL;
	self.id = GetCurrentThreadId();
	return self;
}
#endif
