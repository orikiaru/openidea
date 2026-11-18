#include "oi_rpc_session_manager.h"
#include "oi_thread.h"
#include "oi_log.h"
#include "oi_registry_client.h"
#include "oi_timer_manager.h"
#include "oi_duration.h"
#include "oi_config.h"
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
namespace OI
{
namespace Rpc
{
namespace
{
// 缓存还没拉到时多久再看一次。
const int kRecheckMilliseconds = 10;
// 路由连接连不上之后隔多久再连。
const int kRetryMilliseconds = 2000;
}

std::vector<::OI::Base::ServiceInstance> RouteManager::Named(const std::string& sName, const std::vector<::OI::Base::ServiceInstance>& vList)
{
	std::vector<::OI::Base::ServiceInstance> vNamed;
	for (const auto& stInstance : vList)
	{
		if (stInstance.sName == sName)
		{
			vNamed.push_back(stInstance);
		}
	}
	return vNamed;
}

RouteManager::RouteManager()
{
}

const std::string RouteManager::GetIDString()
{
	return "RpcRoute";
}

void RouteManager::Stop()
{
	std::vector<ServiceGroup*> vGroups;
	{
		::OI::Thread::MutexGuard stLock(m_stGroupLocker);
		m_bStopped = true;
		for (const auto& [sName, pGroup] : m_mGroups)
		{
			vGroups.push_back(pGroup.get());
		}
	}
	// 不在表锁内结束等待：恢复的协程可能马上再次选路。
	for (auto* pGroup : vGroups)
	{
		pGroup->RetireAll();
	}
}

ServiceGroup* RouteManager::Group(const std::string& sName, const std::vector<::OI::Base::ServiceInstance>& vList)
{
	std::call_once(m_stLimitOnce, [this]()
	{
		// 非法配置已记录原因，路由沿用默认限额。
		LoadSessionLimits();
	});
	std::call_once(m_stStopOnce, []()
	{
		::OI::Thread::ThreadPool::GetInstance().AddStopHandler([]()
		{
			GetInstance().Stop();
		});
	});
	ServiceGroup* pGroup = nullptr;
	{
		::OI::Thread::MutexGuard stLock(m_stGroupLocker);
		if (m_bStopped || ::OI::Thread::ThreadPool::GetInstance().StopRequested())
		{
			return nullptr;
		}
		auto& pSlot = m_mGroups[sName];
		if (!pSlot)
		{
			pSlot = std::make_unique<ServiceGroup>(this, true);
		}
		pGroup = pSlot.get();
	}
	pGroup->Apply(vList, kRetryMilliseconds);
	return pGroup;
}

::OI::Coro::Task RouteManager::OnSecondUpdate(time_t)
{
	std::vector<std::pair<std::string, ServiceGroup*>> vGroups;
	{
		::OI::Thread::MutexGuard stLock(m_stGroupLocker);
		if (m_bStopped)
		{
			co_return;
		}
		for (const auto& [sName, pGroup] : m_mGroups)
		{
			vGroups.emplace_back(sName, pGroup.get());
		}
	}
	auto& stClient = ::OI::Base::RegistryClient::GetInstance();
	std::vector<::OI::Base::ServiceInstance> vList;
	for (const auto& [sName, pGroup] : vGroups)
	{
		if (stClient.Snapshot(sName, vList))
		{
			pGroup->Apply(Named(sName, vList), kRetryMilliseconds);
		}
	}
	co_return;
}

RouteConnectionInfo RouteManager::ConnectionInfo(const RpcSender& stSender)
{
	if (stSender.pManager != this)
	{
		return {};
	}
	auto* pConnection = static_cast<PeerConnection*>(stSender.pContext);
	::OI::Thread::MutexGuard stLock(pConnection->stMutex);
	const auto& stPolicy = pConnection->stPolicy;
	return {pConnection->sIp, pConnection->sPort, pConnection->iSessionId, stPolicy.iSendBytes,
	    stPolicy.iReceiveBytes, stPolicy.iFrameBytes, stPolicy.iIncompleteMilliseconds,
	    {stPolicy.iHeldCount, stPolicy.iHeldBytes}};
}

std::shared_ptr<void> RouteManager::ConnectionOwner(const RpcSender& stSender)
{
	if (stSender.pManager != this)
	{
		return {};
	}
	return static_cast<PeerConnection*>(stSender.pContext)->pSelf.lock();
}

RoutePoolStats RouteManager::Stats()
{
	std::vector<ServiceGroup*> vGroups;
	{
		::OI::Thread::MutexGuard stLock(m_stGroupLocker);
		for (const auto& [sName, pGroup] : m_mGroups)
		{
			vGroups.push_back(pGroup.get());
		}
	}
	RoutePoolStats stStats;
	stStats.iGroups = vGroups.size();
	for (auto* pGroup : vGroups)
	{
		stStats.iClients += pGroup->Size();
	}
	return stStats;
}

ServiceRoute RouteManager::Resolve(const std::string& sName, ServicePick iPick, const std::string& sZone, int32_t iCode, const std::vector<::OI::Base::ServiceInstance>& vList)
{
	ServiceRoute stRoute;
	if (iCode != OI_OK)
	{
		stRoute.iStatus = ServiceRouteStatus::Offline;
		return stRoute;
	}
	const std::vector<::OI::Base::ServiceInstance> vNamed = Named(sName, vList);
	bool bGlobal = false;
	bool bZoned = false;
	bool bMatched = false;
	for (const auto& stInstance : vNamed)
	{
		if (stInstance.sZone.empty())
		{
			bGlobal = true;
		}
		else
		{
			bZoned = true;
		}
		bMatched = bMatched || iPick != ServicePick::Zone || stInstance.sZone == sZone;
	}
	if ((bGlobal && bZoned) || (iPick == ServicePick::Zone && bGlobal))
	{
		stRoute.iStatus = ServiceRouteStatus::WrongKind;
		return stRoute;
	}
	if (!bMatched)
	{
		stRoute.iStatus = ServiceRouteStatus::Offline;
		return stRoute;
	}
	ServiceGroup* pGroup = Group(sName, vNamed);
	if (!pGroup)
	{
		stRoute.iStatus = ServiceRouteStatus::Busy;
		return stRoute;
	}
	if (iPick == ServicePick::All)
	{
		std::vector<int32_t> vIds;
		pGroup->All(vIds, stRoute);
		return stRoute;
	}
	auto pSender = iPick == ServicePick::Zone ? pGroup->Zone(sZone) : pGroup->Any();
	if (!pSender)
	{
		stRoute.iStatus = ServiceRouteStatus::Offline;
		return stRoute;
	}
	stRoute.vClients.push_back(pSender.get());
	stRoute.vOwners.push_back(std::move(pSender));
	stRoute.iStatus = ServiceRouteStatus::Ok;
	return stRoute;
}

ServiceRoute RouteManager::ResolveZones(const std::string& sName, const std::vector<std::string>& vZones, int32_t iCode, const std::vector<::OI::Base::ServiceInstance>& vList)
{
	ServiceRoute stRoute;
	if (iCode != OI_OK)
	{
		stRoute.iStatus = ServiceRouteStatus::Offline;
		return stRoute;
	}
	const std::vector<::OI::Base::ServiceInstance> vNamed = Named(sName, vList);
	for (const auto& stInstance : vNamed)
	{
		if (stInstance.sZone.empty())
		{
			stRoute.iStatus = ServiceRouteStatus::WrongKind;
			return stRoute;
		}
	}
	ServiceGroup* pGroup = Group(sName, vNamed);
	if (!pGroup)
	{
		stRoute.iStatus = ServiceRouteStatus::Busy;
		return stRoute;
	}
	for (const auto& sZone : vZones)
	{
		auto pSender = pGroup->Zone(sZone);
		stRoute.vClients.push_back(pSender.get());
		stRoute.vOwners.push_back(std::move(pSender));
	}
	stRoute.iStatus = ServiceRouteStatus::Ok;
	return stRoute;
}

bool RouteAwaiter::ValidToken(const std::string& sText)
{
	if (sText.empty())
	{
		return false;
	}
	for (size_t iIndexLocal = 0; iIndexLocal < sText.size(); ++iIndexLocal)
	{
		char iCharacter = sText[iIndexLocal];
		bool bOk = (iCharacter >= 'a' && iCharacter <= 'z') || (iCharacter >= 'A' && iCharacter <= 'Z') || (iCharacter >= '0' && iCharacter <= '9') || iCharacter == '_' || iCharacter == '-';
		if (!bOk)
		{
			return false;
		}
	}
	return true;
}

bool RouteAwaiter::Prepare()
{
	vZoneNames.clear();
	bool bValid = ValidToken(sName);
	if (iKind == Kind::Zones)
	{
		bValid = bValid && !vZones.empty();
		for (int32_t iZoneId : vZones)
		{
			bValid = bValid && iZoneId >= 0;
		}
		if (bValid)
		{
			vZoneNames.reserve(vZones.size());
			for (int32_t iZoneId : vZones)
			{
				vZoneNames.push_back(std::to_string(iZoneId));
			}
		}
	}
	else if (iPick == ServicePick::Zone)
	{
		bValid = bValid && ValidToken(sZone);
	}
	if (!bValid)
	{
		vZoneNames.clear();
		stRoute.iStatus = ServiceRouteStatus::BadName;
	}
	return !bValid;
}

RpcDeadline::RpcDeadline(::OI::Timer::MS stTimeout)
{
	if (stTimeout.iMilliseconds > 0)
	{
		const int64_t iNow = ::OI::Timer::SteadyMilliseconds();
		iDeadlineMilliseconds = stTimeout.iMilliseconds > INT64_MAX - iNow ? INT64_MAX : iNow + stTimeout.iMilliseconds;
	}
}

bool RpcDeadline::Expired() const
{
	return iDeadlineMilliseconds > 0 && ::OI::Timer::SteadyMilliseconds() >= iDeadlineMilliseconds;
}

::OI::Timer::MS RpcDeadline::Remaining() const
{
	if (iDeadlineMilliseconds == 0)
	{
		return ::OI::Timer::MS(0);
	}
	return ::OI::Timer::MS((std::max)(int64_t(1), iDeadlineMilliseconds - ::OI::Timer::SteadyMilliseconds()));
}

int32_t RouteCall::Error(ServiceRouteStatus iStatus)
{
	switch (iStatus)
	{
	case ServiceRouteStatus::Timeout:
		return OI_ERR_TIMEOUT;
	case ServiceRouteStatus::Busy:
		return OI_ERR_BUSY;
	case ServiceRouteStatus::BadName:
	case ServiceRouteStatus::WrongKind:
		return OI_ERR_FAIL;
	default:
		return OI_ERR_CONNECT;
	}
}

OI::Coro::TaskResult<int32_t> RouteCall::Fail(int32_t iCode)
{
	co_return iCode;
}

bool RouteAwaiter::await_suspend(std::coroutine_handle<> stHandleLocal)
{
	stHandle = stHandleLocal;
	if (Prepare())
	{
		bResolved = true;
		return false;
	}
	if (stDeadline.Expired())
	{
		stRoute.iStatus = ServiceRouteStatus::Timeout;
		bResolved = true;
		return false;
	}
	const int64_t iRemaining = stDeadline.Remaining().iMilliseconds;
	// 注册表每秒拉一次，没拉到过的名字最多等两轮。
	if (ReadCache(iRemaining > 0 ? (std::min)(iRemaining, int64_t(3000)) : 3000))
	{
		return false;
	}
	// 看见 Ready 表示名单已经写完，不挂起。
	return iGate.exchange(OI::Coro::Gate::Waiting, std::memory_order_acq_rel) != OI::Coro::Gate::Ready;
}

ServiceRoute RouteAwaiter::await_resume()
{
	if (stDeadline.Expired() || iCode == OI_ERR_TIMEOUT)
	{
		stRoute.iStatus = ServiceRouteStatus::Timeout;
		return std::move(stRoute);
	}
	if (bResolved)
	{
		return std::move(stRoute);
	}
	if (iKind == Kind::Zones)
	{
		return RouteManager::GetInstance().ResolveZones(sName, vZoneNames, iCode, vList);
	}
	return RouteManager::GetInstance().Resolve(sName, iPick, sZone, iCode, vList);
}

bool RouteAwaiter::ReadCache(int64_t iTimeoutMilliseconds)
{
	auto& stClient = ::OI::Base::RegistryClient::GetInstance();
	if (!stClient.Configured())
	{
		iCode = OI_ERR_NOT_FOUND;
		return true;
	}
	if (stClient.Snapshot(sName, vList))
	{
		iCode = OI_OK;
		return true;
	}
	Recheck(::OI::Timer::SteadyMilliseconds() + iTimeoutMilliseconds);
	return false;
}

void RouteAwaiter::Recheck(int64_t iDeadlineMilliseconds)
{
	const int32_t iStatus = ::OI::Timer::BaseTimerManager::GetInstance().AddTimer([this, iDeadlineMilliseconds]()
	{
		try
		{
			if (::OI::Base::ServiceTable::GetInstance().Snapshot(sName, vList))
			{
				iCode = OI_OK;
			}
			else if (::OI::Timer::SteadyMilliseconds() < iDeadlineMilliseconds)
			{
				Recheck(iDeadlineMilliseconds);
				return;
			}
			else
			{
				iCode = OI_ERR_TIMEOUT;
			}
		}
		catch (...)
		{
			iCode = OI_ERR_FAIL;
		}
		Finish();
	}, kRecheckMilliseconds);
	if (iStatus != OI_OK)
	{
		iCode = OI_ERR_TIMEOUT;
		Finish();
	}
}

void RouteAwaiter::Finish()
{
	// Waiting→Ready 才调度。先到时门已是 Ready，await_suspend 的 exchange(Waiting) 会选择不挂起。
	if (iGate.exchange(OI::Coro::Gate::Ready, std::memory_order_acq_rel) == OI::Coro::Gate::Waiting)
	{
		OI::Coro::Schedule(stHandle);
	}
}
}
}
