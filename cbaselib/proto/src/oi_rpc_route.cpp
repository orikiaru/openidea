#include "oi_rpc_coro.h"
#include "oi_config.h"
#include "oi_thread.h"
#include "oi_log.h"
#include "oi_tcp_channel.h"
#include <tuple>
#include <mutex>
#include <cstdlib>
#include <uv.h>
namespace OI
{
namespace Rpc
{
RouteManager::RouteManager()
{
	int64_t iLimit = static_cast<int64_t>(m_iMaxClients);
	TryGetConfig("RpcRoute.max_clients", iLimit);
	if (iLimit <= 0 || static_cast<uint64_t>(iLimit) > SIZE_MAX)
	{
		throw std::invalid_argument("RpcRoute.max_clients");
	}
	m_iMaxClients = static_cast<size_t>(iLimit);
	TryGetConfig("RpcRoute.idle_ms", m_iIdleMilliseconds);
	if (m_iIdleMilliseconds <= 0)
	{
		throw std::invalid_argument("RpcRoute.idle_ms");
	}
}

const std::string RouteManager::GetIDString()
{
	return "RpcRoute";
}

void RouteManager::Sweep(bool bStop)
{
	assert(::OI::Net::Poll::GetInstance().InLoopThread());
	std::vector<std::shared_ptr<PeerConnection>> vRetired;
	const int64_t iNow = ::OI::Timer::SteadyMilliseconds();
	{
		::OI::Thread::MutexGuard stLock(m_stPoolLocker);
		vRetired.reserve(m_mClients.size());
		if (bStop)
		{
			m_bStopped = true;
		}
		for (auto it = m_mClients.begin(); it != m_mClients.end();)
		{
			auto& stEntry = it->second;
			if (!bStop && stEntry.pClient->HasLease())
			{
				stEntry.iIdleSinceMilliseconds = 0;
				++it;
				continue;
			}
			if (stEntry.iIdleSinceMilliseconds == 0)
			{
				stEntry.iIdleSinceMilliseconds = iNow;
			}
			if (!bStop && iNow - stEntry.iIdleSinceMilliseconds < m_iIdleMilliseconds)
			{
				++it;
				continue;
			}
			vRetired.push_back(stEntry.pClient);
			it = m_mClients.erase(it);
			++m_iEvicted;
		}
	}
	// 不在池锁内结束等待：恢复的协程可能马上再次选路。
	for (const auto& pClient : vRetired)
	{
		pClient->Retire();
	}
}

void RouteManager::Stop()
{
	if (m_pTimer)
	{
		uv_timer_stop(m_pTimer);
	}
	Sweep(true);
}

void RouteManager::OnArm(void*)
{
	GetInstance().Arm();
}

void RouteManager::OnSweep(uv_timer_s*)
{
	try
	{
		GetInstance().Sweep(false);
	}
	catch (...)
	{
		::OI::Thread::ReportTaskError(std::current_exception());
	}
}

void RouteManager::Arm()
{
	if (::OI::Thread::ThreadPool::GetInstance().StopRequested())
	{
		Stop();
		return;
	}
	auto* pTimer = new uv_timer_t();
	if (uv_timer_init(::OI::Net::Poll::GetInstance().Loop(), pTimer) != 0)
	{
		delete pTimer;
		Stop();
		return;
	}
	m_pTimer = pTimer;
	const uint64_t iIntervalMilliseconds = static_cast<uint64_t>((std::min)(m_iIdleMilliseconds, int64_t{1000}));
	if (uv_timer_start(m_pTimer, &RouteManager::OnSweep, iIntervalMilliseconds, iIntervalMilliseconds) != 0)
	{
		Stop();
	}
}

std::shared_ptr<RpcSender> RouteManager::Lease(const std::string& sIp, const std::string& sPort, ::OI::Net::SessionManager* pManager)
{
	std::call_once(m_stArmOnce, []()
	{
		if (::OI::Thread::ThreadPool::GetInstance().AddStopHandler([]()
		{
			GetInstance().Stop();
		}) == OI_OK)
		{
			::OI::Net::Poll::GetInstance().CallOnLoop(&RouteManager::OnArm, nullptr);
		}
	});
	const auto stKey = std::make_tuple(sIp, sPort, PeerPolicy(pManager));
	std::shared_ptr<RpcSender> pLease;
	{
		::OI::Thread::MutexGuard stLock(m_stPoolLocker);
		if (m_bStopped || ::OI::Thread::ThreadPool::GetInstance().StopRequested())
		{
			return {};
		}
		auto it = m_mClients.find(stKey);
		if (it == m_mClients.end())
		{
			if (m_mClients.size() >= m_iMaxClients)
			{
				return {};
			}
			auto pClient = PeerConnection::Create(this, 0, sIp, sPort, std::get<2>(stKey), false);
			it = m_mClients.emplace(stKey, Entry{std::move(pClient), 0}).first;
		}
		auto& stEntry = it->second;
		// 入站处理和出站路由共用业务租约，网络保活不计入空闲判断。
		pLease = stEntry.pClient->AcquireLease();
		stEntry.iIdleSinceMilliseconds = 0;
	}
	// 连接自行合并建连请求；管理器始终是进程常驻单例。
	static_cast<PeerConnection*>(pLease->pContext)->Ensure();
	return pLease;
}

bool RouteManager::AddClient(ServiceRoute& stRoute, const std::string& sIp, const std::string& sPort, ::OI::Net::SessionManager* pManager)
{
	auto pOwner = Lease(sIp, sPort, pManager);
	if (!pOwner)
	{
		stRoute.vClients.clear();
		stRoute.vOwners.clear();
		stRoute.iStatus = ServiceRouteStatus::Busy;
		return false;
	}
	stRoute.vClients.push_back(pOwner.get());
	stRoute.vOwners.push_back(std::move(pOwner));
	return true;
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
	::OI::Thread::MutexGuard stLock(m_stPoolLocker);
	RoutePoolStats stStats;
	stStats.iClients = m_mClients.size();
	stStats.iMaxClients = m_iMaxClients;
	stStats.iIdleMilliseconds = m_iIdleMilliseconds;
	stStats.iEvicted = m_iEvicted;
	for (const auto& [stKey, stEntry] : m_mClients)
	{
		if (stEntry.pClient->HasLease())
		{
			++stStats.iLeasedClients;
		}
	}
	return stStats;
}

ServiceRoute RouteManager::Resolve(const std::string& sName, ServicePick iPick, const std::string& sZone, int32_t iCode, const std::vector<::OI::Base::ServiceInstance>& vList, ::OI::Net::SessionManager* pManager)
{
	ServiceRoute stRoute;
	if (iCode != OI_OK)
	{
		stRoute.iStatus = ServiceRouteStatus::Offline;
		return stRoute;
	}
	bool bGlobal = false;
	bool bZoned = false;
	std::vector<::OI::Base::ServiceInstance> vMatched;
	for (const auto& stInst : vList)
	{
		if (stInst.sName != sName)
		{
			continue;
		}
		if (stInst.sZone.empty())
		{
			bGlobal = true;
		}
		else
		{
			bZoned = true;
		}
		if (iPick == ServicePick::Zone)
		{
			if (stInst.sZone == sZone)
			{
				vMatched.push_back(stInst);
			}
		}
		else
		{
			vMatched.push_back(stInst);
		}
	}
	if (bGlobal && bZoned)
	{
		stRoute.iStatus = ServiceRouteStatus::WrongKind;
		return stRoute;
	}
	if (iPick == ServicePick::Zone && bGlobal)
	{
		stRoute.iStatus = ServiceRouteStatus::WrongKind;
		return stRoute;
	}
	if (vMatched.empty())
	{
		stRoute.iStatus = ServiceRouteStatus::Offline;
		return stRoute;
	}
	std::sort(vMatched.begin(), vMatched.end(), [](const ::OI::Base::ServiceInstance& stLeft, const ::OI::Base::ServiceInstance& stRight)
	{
		return stLeft.iId < stRight.iId;
	});
	if (iPick == ServicePick::All)
	{
		for (const auto& stInst : vMatched)
		{
			if (!AddClient(stRoute, stInst.sIp, stInst.sPort, pManager))
			{
				return stRoute;
			}
		}
	}
	else
	{
		size_t iChosen = 0;
		if (iPick == ServicePick::Any && vMatched.size() > 1)
		{
			iChosen = m_iCursor.fetch_add(1, std::memory_order_relaxed) % vMatched.size();
		}
		const ::OI::Base::ServiceInstance& stInst = vMatched[iChosen];
		// 名单已选定地址，使用固定地址的连接；订阅管理器可能在提交前改连其他分区。
		if (!AddClient(stRoute, stInst.sIp, stInst.sPort, pManager))
		{
			return stRoute;
		}
	}
	stRoute.iStatus = ServiceRouteStatus::Ok;
	return stRoute;
}

ServiceRoute RouteManager::ResolveZones(const std::string& sName, const std::vector<std::string>& vZones, int32_t iCode, const std::vector<::OI::Base::ServiceInstance>& vList, ::OI::Net::SessionManager* pManager)
{
	ServiceRoute stRoute;
	if (iCode != OI_OK)
	{
		stRoute.iStatus = ServiceRouteStatus::Offline;
		return stRoute;
	}
	bool bGlobal = false;
	for (const auto& stInst : vList)
	{
		if (stInst.sName != sName)
		{
			continue;
		}
		if (stInst.sZone.empty())
		{
			bGlobal = true;
		}
	}
	if (bGlobal)
	{
		stRoute.iStatus = ServiceRouteStatus::WrongKind;
		return stRoute;
	}
	for (const auto& sZone : vZones)
	{
		const ::OI::Base::ServiceInstance* pBest = nullptr;
		for (const auto& stInst : vList)
		{
			if (stInst.sName != sName || stInst.sZone != sZone)
			{
				continue;
			}
			if (!pBest || stInst.iId < pBest->iId)
			{
				pBest = &stInst;
			}
		}
		if (!pBest)
		{
			stRoute.vClients.push_back(nullptr);
			stRoute.vOwners.push_back(nullptr);
			continue;
		}
		// 每个槽位固定到本次名单中的地址，不能随调用方管理器的服务切换而漂移。
		if (!AddClient(stRoute, pBest->sIp, pBest->sPort, pManager))
		{
			return stRoute;
		}
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

bool RouteAwaiter::SplitTarget(const std::string& sTargetLocal, bool bBatchLocal, std::string& sNameLocal, ServicePick& iPickLocal, std::string& sZoneLocal)
{
	if (sTargetLocal.size() >= 3 && sTargetLocal.compare(sTargetLocal.size() - 2, 2, ".*") == 0)
	{
		if (!bBatchLocal)
		{
			return false;
		}
		sNameLocal = sTargetLocal.substr(0, sTargetLocal.size() - 2);
		sZoneLocal.clear();
		iPickLocal = ServicePick::All;
		return ValidToken(sNameLocal);
	}
	if (bBatchLocal)
	{
		return false;
	}
	size_t iDot = sTargetLocal.find('.');
	if (iDot == std::string::npos)
	{
		sNameLocal = sTargetLocal;
		sZoneLocal.clear();
		iPickLocal = ServicePick::Any;
		return ValidToken(sNameLocal);
	}
	if (sTargetLocal.find('.', iDot + 1) != std::string::npos)
	{
		return false;
	}
	sNameLocal = sTargetLocal.substr(0, iDot);
	sZoneLocal = sTargetLocal.substr(iDot + 1);
	iPickLocal = ServicePick::Zone;
	return ValidToken(sNameLocal) && ValidToken(sZoneLocal);
}

bool RouteAwaiter::Prepare()
{
	if (iKind == Kind::Target && !SplitTarget(sTarget, bBatch, sName, iPick, sZone))
	{
		stRoute.iStatus = ServiceRouteStatus::BadName;
		return true;
	}
	return iKind == Kind::Zones ? PrepareZones() : PrepareNamed();
}

bool RouteAwaiter::PrepareNamed()
{
	if (!ValidToken(sName) || (iPick == ServicePick::Zone && !ValidToken(sZone)))
	{
		stRoute.iStatus = ServiceRouteStatus::BadName;
		return true;
	}
	if (iPick == ServicePick::Any && pManager)
	{
		std::vector<::OI::Net::SessionManager*> vWatchers = ::OI::Base::ServiceTable::GetInstance().Watchers(sName);
		for (auto* pWatcher : vWatchers)
		{
			if (pWatcher != pManager)
			{
				continue;
			}
			ProtoClientManager* pClient = dynamic_cast<ProtoClientManager*>(pManager);
			if (pClient && pClient->IsConnect())
			{
				stRoute.iStatus = ServiceRouteStatus::Ok;
				stRoute.vClients.push_back(&pClient->Sender());
				stRoute.vOwners.push_back({});
				return true;
			}
		}
	}
	return false;
}

bool RouteAwaiter::PrepareZones()
{
	vZoneNames.clear();
	if (!ValidToken(sName) || vZones.empty())
	{
		stRoute.iStatus = ServiceRouteStatus::BadName;
		return true;
	}
	vZoneNames.reserve(vZones.size());
	for (int32_t iZoneId : vZones)
	{
		if (iZoneId < 0)
		{
			stRoute.iStatus = ServiceRouteStatus::BadName;
			vZoneNames.clear();
			return true;
		}
		vZoneNames.push_back(std::to_string(iZoneId));
	}
	return false;
}

void RouteAwaiter::Pull(int64_t iTimeoutMilliseconds)
{
	::OI::Base::RegistryClient::GetInstance().PullAsync(sName, [this](int32_t iCodeLocal, const std::vector<::OI::Base::ServiceInstance>& vListLocal)
	{
		iCode = iCodeLocal;
		try
		{
			vList = vListLocal;
		}
		catch (...)
		{
			iCode = OI_ERR_FAIL;
		}
		// Waiting→Ready 才调度。回包先到时门已是 Ready，await_suspend 的 exchange(Waiting) 会选择不挂起。
		if (iGate.exchange(OI::Coro::Gate::Ready, std::memory_order_acq_rel) == OI::Coro::Gate::Waiting)
		{
			OI::Coro::Schedule(stHandle);
		}
	}, iTimeoutMilliseconds);
}
}
}
