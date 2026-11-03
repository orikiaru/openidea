#pragma once
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <ctime>
#include <cstdint>
#include <assert.h>

namespace OI { namespace Thread {
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
	std::atomic<int32_t> m_iLockState;
public:
	SpinLock() : m_iLockState(0) {}
	void Lock()
	{
		int iSpins = 0;
		std::time_t iStart = std::time(NULL);
		while (m_iLockState.exchange(1, std::memory_order_acquire) != 0)
		{
			++iSpins;
			if ((iSpins % 100000) == 0 && std::time(NULL) - iStart > 5)
			{
				assert(false && "spin_lock timeout");
			}
			std::this_thread::yield();
		}
	}
	void UnLock()
	{
		m_iLockState.store(0, std::memory_order_release);
	}
};

class Mutex
{
	friend class MutexCond;
	std::recursive_mutex m_stMutex;
public:
	void Lock() { m_stMutex.lock(); }
	void UnLock() { m_stMutex.unlock(); }
	void TryLock()
	{
		int iSpins = 0;
		std::time_t iStart = std::time(NULL);
		while (!m_stMutex.try_lock())
		{
			++iSpins;
			if ((iSpins % 100000) == 0 && std::time(NULL) - iStart > 10)
			{
				assert(false && "mutex timeout");
			}
			std::this_thread::yield();
		}
	}
};

class MutexCond
{
	std::condition_variable_any m_stConditionVariable;
public:
	void Wait(Mutex& stLockerParam)
	{
		m_stConditionVariable.wait(stLockerParam.m_stMutex);
	}
	void Signal() { m_stConditionVariable.notify_one(); }
	void Broadcast() { m_stConditionVariable.notify_all(); }
};

template<class T>
class LockGuard
{
	T* m_pLock;
public:
	LockGuard(T& stLockParam) : m_pLock(&stLockParam) { m_pLock->Lock(); }
	~LockGuard() { m_pLock->UnLock(); }
};

#endif

typedef LockGuard<SpinLock> SpinLockGuard;
typedef LockGuard<Mutex> MutexGuard;
}}
