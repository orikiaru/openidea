#ifndef __spin_lock__
#define __spin_lock__
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <ctime>
#include <assert.h>

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
	LockGuard(T&){}
	~LockGuard(){}
};

class MutexCond
{
public:
	MutexCond() {}
	void Wait(Mutex&) {}
	void Signal() {}
	void Broadcast() {}
	void Signal(Mutex&) {}
	void Broadcast(Mutex&) {}
};

#else

class SpinLock
{
	std::atomic<int> _l;
public:
	SpinLock() : _l(0) {}
	void Lock()
	{
		int spins = 0;
		std::time_t start = std::time(NULL);
		while (_l.exchange(1, std::memory_order_acquire) != 0)
		{
			++spins;
			if ((spins % 100000) == 0 && std::time(NULL) - start > 5)
			{
				assert(false && "spin_lock timeout");
			}
			std::this_thread::yield();
		}
	}
	void UnLock()
	{
		_l.store(0, std::memory_order_release);
	}
};

class Mutex
{
	friend class MutexCond;
	std::mutex _m;
public:
	void Lock() { _m.lock(); }
	void UnLock() { _m.unlock(); }
	void TryLock()
	{
		int spins = 0;
		std::time_t start = std::time(NULL);
		while (!_m.try_lock())
		{
			++spins;
			if ((spins % 100000) == 0 && std::time(NULL) - start > 10)
			{
				assert(false && "mutex timeout");
			}
			std::this_thread::yield();
		}
	}
};

class MutexCond
{
	std::condition_variable _cv;
public:
	void Wait(Mutex& locker)
	{
		std::unique_lock<std::mutex> hold(locker._m, std::adopt_lock);
		_cv.wait(hold);
		hold.release();
	}
	void Signal() { _cv.notify_one(); }
	void Broadcast() { _cv.notify_all(); }
};

template<class T>
class LockGuard
{
	T* _lock;
public:
	LockGuard(T& lock) : _lock(&lock) { _lock->Lock(); }
	~LockGuard() { _lock->UnLock(); }
};

#endif

typedef LockGuard<SpinLock> SpinLockGuard;
typedef LockGuard<Mutex> MutexGuard;
#endif
