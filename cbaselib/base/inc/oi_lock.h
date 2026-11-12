#pragma once
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <ctime>
#include <cstdint>
#include <assert.h>

namespace OI
{
namespace Thread
{
// 后台执行器与 poll 始终跨线程，不随业务单线程构建退化。
class NativeMutex
{
	friend class NativeMutexCond;
	std::mutex m_stMutex;

  public:
	void Lock()
	{
		m_stMutex.lock();
	}
	void UnLock()
	{
		m_stMutex.unlock();
	}
};
class NativeMutexGuard
{
	NativeMutex& m_stMutex;

  public:
	explicit NativeMutexGuard(NativeMutex& stMutex)
	    : m_stMutex(stMutex)
	{
		m_stMutex.Lock();
	}
	~NativeMutexGuard()
	{
		m_stMutex.UnLock();
	}
	NativeMutexGuard(const NativeMutexGuard&) = delete;
	NativeMutexGuard& operator=(const NativeMutexGuard&) = delete;
};
class NativeMutexCond
{
	std::condition_variable_any m_stCondition;

  public:
	void Wait(NativeMutex& stMutex)
	{
		m_stCondition.wait(stMutex.m_stMutex);
	}
	void Signal()
	{
		m_stCondition.notify_one();
	}
	void Broadcast()
	{
		m_stCondition.notify_all();
	}
};

// 自有代码的互斥、自旋和条件等待。单线程编译下全是空操作，不能靠它们阻塞；要等事件改走 poll。
#ifdef _SINGLE_THREAD_

// 短临界区自旋。单线程下加锁和解锁都是空的。
class SpinLock
{
  public:
	void Lock()
	{
	}
	void UnLock()
	{
	}
};

// 可重入互斥。单线程下加锁和解锁都是空的。
class Mutex
{
  public:
	void Lock()
	{
	}
	void UnLock()
	{
	}
};

// 构造时加锁，析构时解锁。单线程下两个动作都是空的。
template <class T>
class LockGuard
{
  public:
	LockGuard(T&)
	{
	}
	~LockGuard()
	{
	}
};

// 和 Mutex 配对的等待。单线程里 Wait 立即返回，WaitUntil 直接返回 false，不会阻塞。
class MutexCond
{
  public:
	MutexCond()
	{
	}
	// 单线程不等待。调用方不能靠返回这一刻条件已经成立。
	void Wait(Mutex&)
	{
	}
	// 单线程立即返回 false，表示没等到截止时间前的通知。
	bool WaitUntil(Mutex&, const std::chrono::steady_clock::time_point&)
	{
		return false;
	}
	// 单线程没有等待者。
	void Signal()
	{
	}
	void Broadcast()
	{
	}
	void Signal(Mutex&)
	{
	}
	void Broadcast(Mutex&)
	{
	}
};

#else

// 短临界区自旋。持有超过约 5 秒断言，避免死锁被当成正常等待。
class SpinLock
{
	// 0 空闲，1 已占用。
	std::atomic<int32_t> m_iLockState;

  public:
	SpinLock()
	    : m_iLockState(0)
	{
	}
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

// 可重入互斥。同一线程可以再次 Lock，必须配对 UnLock。
class Mutex
{
	friend class MutexCond;
	// 实际的可重入锁。
	std::recursive_mutex m_stMutex;

  public:
	void Lock()
	{
		m_stMutex.lock();
	}
	void UnLock()
	{
		m_stMutex.unlock();
	}
	// 一直试到拿到。超过约 10 秒仍拿不到就断言。
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

// 和 Mutex 配对的等待。Wait 可能虚假唤醒，调用方用 while 查条件。
class MutexCond
{
	// 实际的条件变量。等待时用调用方 Mutex 里的那把锁。
	std::condition_variable_any m_stConditionVariable;

  public:
	// 放开锁并等待通知，返回时重新持有锁。可能虚假唤醒。
	void Wait(Mutex& stLocker)
	{
		m_stConditionVariable.wait(stLocker.m_stMutex);
	}
	// 到截止时间返回 false。和 Wait 一样可能虚假唤醒，调用方自己循环查条件。
	bool WaitUntil(Mutex& stLocker, const std::chrono::steady_clock::time_point& stDeadline)
	{
		return m_stConditionVariable.wait_until(stLocker.m_stMutex, stDeadline) == std::cv_status::no_timeout;
	}
	// 唤醒一个等待者。
	void Signal()
	{
		m_stConditionVariable.notify_one();
	}
	// 唤醒全部等待者。
	void Broadcast()
	{
		m_stConditionVariable.notify_all();
	}
};

// 构造时 Lock，析构时 UnLock。
template <class T>
class LockGuard
{
	// 被守卫的锁。
	T* m_pLock;

  public:
	LockGuard(T& stLock)
	    : m_pLock(&stLock)
	{
		m_pLock->Lock();
	}
	~LockGuard()
	{
		m_pLock->UnLock();
	}
};

#endif

typedef LockGuard<SpinLock> SpinLockGuard;
typedef LockGuard<Mutex> MutexGuard;
}
}
