#ifndef __spin_lock__
#define __spin_lock__
#include <pthread.h>


#define USE_PTHREAD_SPINLOCK
	//NOTIFY	Here is POSIX pthread_spin_lock compare myself create SpinLock
	//		I think you may use SpinLockGuard<pthread_spin_lock>	hhhhh
	//		Test Config 2C4T 3.0GHZ
	//	pthread_spin_lock 
	//		final=1545163896,final1=0,in 20s
	//		final=1556315601,final1=0,in 20s
	//		final=1554803980,final1=0,in 20s
	//		final=1343716523,final1=0,in 20s
	//	 SpinLockGuard<pthread_spin_lock>
	//		final=1025454696,final1=0,in 20s
	//		final=1032860206,final1=0,in 20s
	//	 SpinLock
	//		final=1056697712,final1=0,in 20s
	//		final=1034461262,final1=0,in 20s
	//		final=1034249387,final1=0,in 20s
	//	 SpinLockGuard<SpinLock>
	//		final=802417461,final1=0,in 20s
	//		final=798316525,final1=0,in 20s
	//	 SpinLockThreadOnce		//too slow
	//		final=0,final1=47258132,in 20s
	//		final=0,final1=46855234,in 20s
	//	 Mutex
	//		final=0,final1=572333205,in 20s
	//		final=0,final1=568371642,in 20s



#ifdef _SINGLE_THREAD_

class SpinLock
{
public:
	void Lock(){}
	void UnLock(){}
};

class Mutex
{
public:
	void Lock(){}
	void UnLock(){}
};

template<class T>
class LockGuard
{
public:
	LockGuard(T& lock){}
	~LockGuard(){}
};

class MutexCond
{
public:
	MutexCond() {}
	void Wait(Mutex& locker) {}
	void Signal(Mutex& locker) {}
	void Broadcast(Mutex& locker) {}
};

#else

#ifndef USE_PTHREAD_SPINLOCK
class SpinLock
{
	int _l;
public:
	SpinLock():_l(0){}
	void Lock();
	void UnLock();
};

#else
class SpinLock
{
	pthread_spinlock_t _l;
public:
	SpinLock()
	{
		pthread_spin_init(&_l, PTHREAD_PROCESS_PRIVATE);
	}
	void Lock();
	void TryLock(int time_out = 4);
	void UnLock();
	~SpinLock()
	{
		pthread_spin_destroy(&_l);
	}
};
#endif

class Mutex
{
	friend class MutexCond;
	pthread_mutex_t		_l;
public:
	Mutex();
	void Lock()
	{
		pthread_mutex_lock(&_l);
	}
	void TryLock();
	void UnLock()
	{
		pthread_mutex_unlock(&_l);
	}
	~Mutex();
};

class MutexCond
{
	pthread_cond_t		_cond;

public:
	MutexCond()
	{
		pthread_cond_init(&_cond, NULL);
	}
	~MutexCond()
	{
		pthread_cond_destroy(&_cond);
	}
	void Wait(Mutex& locker)
	{
		pthread_cond_wait(&_cond, &locker._l);
	}
	void Signal()
	{
		pthread_cond_signal(&_cond);
	}
	void Broadcast()
	{
		pthread_cond_broadcast(&_cond);
	}
};

template<class T>
class LockGuard
{
	T* 	_lock;
public:
	LockGuard(T& lock):_lock(&lock)
	{
		_lock->Lock();
	}
	~LockGuard()
	{
		_lock->UnLock();
	}
};
#endif

typedef LockGuard<SpinLock> SpinLockGuard;
typedef LockGuard<Mutex>   MutexGuard;
#endif
