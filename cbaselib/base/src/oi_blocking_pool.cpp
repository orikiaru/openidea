#include "oi_blocking_pool.h"
#include "oi_lock.h"
#include "oi_errno.h"
#include "oi_thread.h"
#include <deque>
#include <map>
#include <memory>
#include <vector>
#include <limits>

namespace OI
{
namespace Thread
{
namespace
{
class BlockingPool
{
	NativeMutex m_stMutex;
	NativeMutexCond m_stReady;
	std::deque<std::function<void()>> m_vQueue;
	std::vector<std::thread> m_vThreads;
	size_t m_iQueueLimit;
	bool m_bStopped = false;
	void Run()
	{
		for (;;)
		{
			std::function<void()> fnWork;
			{
				NativeMutexGuard stGuard(m_stMutex);
				while (m_vQueue.empty() && !m_bStopped)
				{
					m_stReady.Wait(m_stMutex);
				}
				if (m_vQueue.empty())
				{
					return;
				}
				fnWork = std::move(m_vQueue.front());
				m_vQueue.pop_front();
			}
			try
			{
				fnWork();
			}
			catch (...)
			{
				ReportTaskError(std::current_exception());
			}
		}
	}

  public:
	BlockingPool(size_t iThreads, size_t iQueueLimit)
	    : m_iQueueLimit(iQueueLimit)
	{
		try
		{
			m_vThreads.reserve(iThreads);
			for (size_t iIndex = 0; iIndex < iThreads; ++iIndex)
			{
				m_vThreads.emplace_back([this]()
				{
					Run();
				});
			}
		}
		catch (...)
		{
			Stop();
			throw;
		}
	}
	~BlockingPool()
	{
		Stop();
	}
	int32_t Submit(std::function<void()> fnWork)
	{
		NativeMutexGuard stGuard(m_stMutex);
		if (m_bStopped)
		{
			return OI_ERR_CONNECT;
		}
		if (m_vQueue.size() >= m_iQueueLimit)
		{
			return OI_ERR_BUSY;
		}
		m_vQueue.push_back(std::move(fnWork));
		m_stReady.Signal();
		return OI_OK;
	}
	void Stop()
	{
		{
			NativeMutexGuard stGuard(m_stMutex);
			m_bStopped = true;
			m_stReady.Broadcast();
		}
		for (auto& stThread : m_vThreads)
		{
			if (stThread.joinable())
			{
				stThread.join();
			}
		}
	}
};
struct PoolEntry
{
	size_t iThreads = 4;
	size_t iQueueLimit = 1024;
	std::shared_ptr<BlockingPool> pPool;
};
class Pools
{
  public:
	NativeMutex stMutex;
	std::map<BlockingType, PoolEntry> mEntries{{BlockingType::Default, {}}};
	bool bStopped = false;
};
Pools& GetPools()
{
	static Pools stPools;
	return stPools;
}
}
void InitializeBlockingPools()
{
	(void)GetPools();
}
int32_t ConfigureBlockingPool(BlockingType iType, size_t iThreads, size_t iQueueLimit)
{
	if (iThreads == 0 || iThreads > 256 || iQueueLimit == 0 || iQueueLimit > std::numeric_limits<size_t>::max() - iThreads)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	auto& stPools = GetPools();
	NativeMutexGuard stGuard(stPools.stMutex);
	if (stPools.bStopped)
	{
		return OI_ERR_CONNECT;
	}
	auto& stEntry = stPools.mEntries[iType];
	if (stEntry.pPool)
	{
		return OI_ERR_BUSY;
	}
	stEntry.iThreads = iThreads;
	stEntry.iQueueLimit = iQueueLimit;
	return OI_OK;
}
size_t BlockingPoolCapacity(BlockingType iType)
{
	auto& stPools = GetPools();
	NativeMutexGuard stGuard(stPools.stMutex);
	const auto it = stPools.mEntries.find(iType);
	if (it == stPools.mEntries.end() || stPools.bStopped)
	{
		return 0;
	}
	const size_t iCapacity = it->second.iThreads + it->second.iQueueLimit;
	return iType == BlockingType::Default && iCapacity > 1024 ? 1024 : iCapacity;
}
int32_t SubmitBlockingWork(BlockingType iType, std::function<void()> fnWork)
{
	if (!fnWork)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	auto& stPools = GetPools();
	NativeMutexGuard stGuard(stPools.stMutex);
	if (stPools.bStopped)
	{
		return OI_ERR_CONNECT;
	}
	auto it = stPools.mEntries.find(iType);
	if (it == stPools.mEntries.end())
	{
		return OI_ERR_NOT_FOUND;
	}
	auto& stEntry = it->second;
	if (!stEntry.pPool)
	{
		stEntry.pPool = std::make_shared<BlockingPool>(stEntry.iThreads, stEntry.iQueueLimit);
	}
	return stEntry.pPool->Submit(std::move(fnWork));
}
void StopBlockingPools()
{
	auto& stPools = GetPools();
	std::map<BlockingType, PoolEntry> mEntries;
	{
		NativeMutexGuard stGuard(stPools.stMutex);
		stPools.bStopped = true;
		mEntries.swap(stPools.mEntries);
	}
	for (auto& [iType, stEntry] : mEntries)
	{
		(void)iType;
		if (stEntry.pPool)
		{
			stEntry.pPool->Stop();
		}
	}
}
}
}
