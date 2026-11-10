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
	for (const TickEntry& stEntry : m_vTicks)
	{
		if (stEntry.pOwner == pOwner)
		{
			return;
		}
	}
	m_vTicks.push_back(TickEntry{pOwner, fnUpdate});
}

void SingletonUpdates::AddSecond(void* pOwner, SecondFunction fnUpdate)
{
	if (pOwner == nullptr || fnUpdate == nullptr)
	{
		return;
	}
	::OI::Thread::MutexGuard stLock(m_stMutex);
	for (const SecondEntry& stEntry : m_vSeconds)
	{
		if (stEntry.pOwner == pOwner)
		{
			return;
		}
	}
	m_vSeconds.push_back(SecondEntry{pOwner, fnUpdate});
}

void SingletonUpdates::Remove(void* pOwner)
{
	::OI::Thread::MutexGuard stLock(m_stMutex);
	for (auto it = m_vTicks.begin(); it != m_vTicks.end();)
	{
		if (it->pOwner == pOwner)
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
		if (it->pOwner == pOwner)
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

void SingletonUpdates::DispatchTick(uint64_t iNowMilliseconds)
{
	if (!m_bStarted.load(std::memory_order_acquire))
	{
		return;
	}
	std::vector<TickEntry> vTicks;
	{
		::OI::Thread::MutexGuard stLock(m_stMutex);
		vTicks = m_vTicks;
	}
	for (const TickEntry& stEntry : vTicks)
	{
		try
		{
			stEntry.fnUpdate(stEntry.pOwner, iNowMilliseconds);
		}
		catch (...)
		{
			::OI::Thread::ReportTaskError(std::current_exception());
		}
	}
}

void SingletonUpdates::DispatchSecond(time_t iNow)
{
	if (!m_bStarted.load(std::memory_order_acquire))
	{
		return;
	}
	std::vector<SecondEntry> vSeconds;
	{
		::OI::Thread::MutexGuard stLock(m_stMutex);
		vSeconds = m_vSeconds;
	}
	for (const SecondEntry& stEntry : vSeconds)
	{
		try
		{
			stEntry.fnUpdate(stEntry.pOwner, iNow);
		}
		catch (...)
		{
			::OI::Thread::ReportTaskError(std::current_exception());
		}
	}
}
}
}
