#include "oi_singleton.h"
#include "oi_thread.h"

namespace OI
{
namespace Base
{
SingletonUpdates& SingletonUpdates::Instance()
{
	static SingletonUpdates stInst;
	return stInst;
}

void SingletonUpdates::AddTick(void* pOwner, TickFunction fnUpdate)
{
	if (pOwner == nullptr || fnUpdate == nullptr)
	{
		return;
	}
	::OI::Thread::MutexGuard stLock(m_stMutex);
	for (const auto& pEntry : m_vTicks)
	{
		if (pEntry->pOwner == pOwner)
		{
			return;
		}
	}
	auto pEntry = std::make_shared<Entry>();
	pEntry->pOwner = pOwner;
	pEntry->fnTick = fnUpdate;
	m_vTicks.push_back(std::move(pEntry));
}

void SingletonUpdates::AddSecond(void* pOwner, SecondFunction fnUpdate)
{
	if (pOwner == nullptr || fnUpdate == nullptr)
	{
		return;
	}
	::OI::Thread::MutexGuard stLock(m_stMutex);
	for (const auto& pEntry : m_vSeconds)
	{
		if (pEntry->pOwner == pOwner)
		{
			return;
		}
	}
	auto pEntry = std::make_shared<Entry>();
	pEntry->pOwner = pOwner;
	pEntry->fnSecond = fnUpdate;
	m_vSeconds.push_back(std::move(pEntry));
}

void SingletonUpdates::Remove(void* pOwner)
{
	::OI::Thread::MutexGuard stLock(m_stMutex);
	for (auto it = m_vTicks.begin(); it != m_vTicks.end();)
	{
		if ((*it)->pOwner == pOwner)
		{
			it = m_vTicks.erase(it);
		}
		else
		{
			++it;
		}
	}
	for (auto it = m_vSeconds.begin(); it != m_vSeconds.end();)
	{
		if ((*it)->pOwner == pOwner)
		{
			it = m_vSeconds.erase(it);
		}
		else
		{
			++it;
		}
	}
}

void SingletonUpdates::Start()
{
	m_bStarted.store(true, std::memory_order_release);
}

::OI::Coro::Task SingletonUpdates::RunTick(std::shared_ptr<Entry> pEntry, uint64_t iNowMilliseconds)
{
	try
	{
		co_await pEntry->fnTick(pEntry->pOwner, iNowMilliseconds);
	}
	catch (...)
	{
		pEntry->bRunning.store(false, std::memory_order_release);
		throw;
	}
	pEntry->bRunning.store(false, std::memory_order_release);
}

::OI::Coro::Task SingletonUpdates::RunSecond(std::shared_ptr<Entry> pEntry, time_t iNow)
{
	try
	{
		co_await pEntry->fnSecond(pEntry->pOwner, iNow);
	}
	catch (...)
	{
		pEntry->bRunning.store(false, std::memory_order_release);
		throw;
	}
	pEntry->bRunning.store(false, std::memory_order_release);
}

template <typename F>
void SingletonUpdates::Launch(const std::shared_ptr<Entry>& pEntry, F fnStart)
{
	bool bIdle = false;
	if (!pEntry->bRunning.compare_exchange_strong(bIdle, true, std::memory_order_acq_rel))
	{
		return;
	}
	try
	{
		(void)fnStart();
	}
	catch (...)
	{
		pEntry->bRunning.store(false, std::memory_order_release);
		::OI::Thread::ReportTaskError(std::current_exception());
	}
}

void SingletonUpdates::DispatchTick(uint64_t iNowMilliseconds)
{
	if (!m_bStarted.load(std::memory_order_acquire))
	{
		return;
	}
	std::vector<std::shared_ptr<Entry>> vTicks;
	{
		::OI::Thread::MutexGuard stLock(m_stMutex);
		vTicks = m_vTicks;
	}
	for (const auto& pEntry : vTicks)
	{
		Launch(pEntry, [pEntry, iNowMilliseconds]()
		{
			return ::OI::Coro::StartTask([pEntry, iNowMilliseconds](::OI::Coro::Token)
			{
				return RunTick(pEntry, iNowMilliseconds);
			});
		});
	}
}

void SingletonUpdates::DispatchSecond(time_t iNow)
{
	if (!m_bStarted.load(std::memory_order_acquire))
	{
		return;
	}
	std::vector<std::shared_ptr<Entry>> vSeconds;
	{
		::OI::Thread::MutexGuard stLock(m_stMutex);
		vSeconds = m_vSeconds;
	}
	for (const auto& pEntry : vSeconds)
	{
		Launch(pEntry, [pEntry, iNow]()
		{
			return ::OI::Coro::StartTask([pEntry, iNow](::OI::Coro::Token)
			{
				return RunSecond(pEntry, iNow);
			});
		});
	}
}
}
}
