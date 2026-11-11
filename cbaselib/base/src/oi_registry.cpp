#include "oi_registry.h"
#include "oi_proto_id.h"
#include "oi_rpcdata_regregister.h"
#include "oi_rpcdata_regregisterres.h"
#include "oi_rpcdata_regheartbeat.h"
#include "oi_rpcdata_regheartbeatres.h"
#include "oi_rpcdata_reginstance.h"
#include "oi_rpcdata_regpull.h"
#include "oi_rpcdata_regpullres.h"
#include "oi_net_session.h"
#include "oi_socket.h"
#include "oi_config.h"
#include "oi_log.h"
#include "oi_poll.h"
#include "oi_thread.h"
#include "oi_timer_manager.h"
#include "oi_duration.h"
#include <limits>
#include <algorithm>
#include <stdexcept>
#include <chrono>
#include <cstdio>
#include <memory>
#include <utility>

namespace OI
{
namespace Base
{

struct RegPullState
{
	std::atomic<int32_t> iOnce{0};
	std::function<void(int32_t, const std::vector<ServiceInstance>&)> fnDone;
};

namespace
{
const int kWaitMs = 1000;

// 同步等待要靠 poll 收回包：多线程在 poll 线程上只会空等到超时，单线程会重入 uv_run。
void RejectPollCallback(const char* pOperation)
{
	if (::OI::Net::Poll::GetInstance().InPollCallback())
	{
		throw std::logic_error(std::string("RegistryClient::") + pOperation + " cannot run in a poll callback");
	}
}

class RegistryClientSession : public ::OI::Net::NetSession
{
  public:
	explicit RegistryClientSession(::OI::Net::SessionManager* pManager)
	    : ::OI::Net::NetSession(pManager)
	{
	}

	virtual void OnDataOut()
	{
	}
	virtual void OnDataIn()
	{
		ReadRegFrames(stInputBuffer, this, [iSessionIdLocal = GetSessionID()](uint32_t iIdLocal, ::OI::Base::BytesStream& stBody)
		{
			RegistryClient::GetInstance().OnFrame(iSessionIdLocal, iIdLocal, stBody);
		});
	}
};

class HeartbeatTimer : public ::OI::Timer::BaseTimer
{
  public:
	bool OnTimer()
	{
		RegistryClient::GetInstance().Touch();
		return true;
	}
};

class PullTimer : public ::OI::Timer::BaseTimer
{
  public:
	bool OnTimer()
	{
		RegistryClient::GetInstance().Pull();
		return true;
	}
};

void InvokePullCallback(std::function<void(int32_t, const std::vector<ServiceInstance>&)> fnDoneLocal, int32_t iCodeLocal, const std::vector<ServiceInstance>& vList)
{
	if (fnDoneLocal)
	{
		try
		{
			fnDoneLocal(iCodeLocal, vList);
		}
		catch (...)
		{
			::OI::Thread::ReportTaskError(std::current_exception());
		}
	}
}

void FinishPull(const std::shared_ptr<RegPullState>& pState, int32_t iCodeLocal, const std::vector<ServiceInstance>& vList)
{
	if (!pState || pState->iOnce.exchange(1, std::memory_order_acq_rel) != 0)
	{
		return;
	}
	InvokePullCallback(std::move(pState->fnDone), iCodeLocal, vList);
}

class PullTimeoutTimer : public ::OI::Timer::BaseTimer
{
  public:
	bool OnTimer()
	{
		try
		{
			RegistryClient::GetInstance().ExpirePulls();
		}
		catch (...)
		{
			::OI::Thread::ReportTaskError(std::current_exception());
		}
		return true;
	}
};

}

::OI::Base::Bytes EncodeRegFrame(uint32_t iIdLocal, const ::OI::Rpc::Serialize& stMessage)
{
	::OI::Base::BytesStream stBody;
	stMessage.serialize(stBody);
	::OI::Base::BytesStream stFrame;
	uint32_t iSize = (uint32_t)stBody.Size();
	stFrame.WritePbRaw(iIdLocal, iSize ? stBody.Begin() : NULL, iSize);
	return stFrame;
}

bool ReadRegFrames(::OI::Base::Bytes& stInput, ::OI::Net::Session* pSession, const std::function<void(uint32_t, ::OI::Base::BytesStream&)>& fnOnFrame)
{
	::OI::Base::BytesStream stOutputStream;
	stOutputStream.Swap(stInput);
	bool bClosedLocal = false;
	while (stOutputStream.RemainSize() && !bClosedLocal)
	{
		stOutputStream.StartTransaction();
		uint32_t iIdLocal = 0;
		uint32_t iSize = 0;
		::OI::Base::PbHeaderState iSt = stOutputStream.PreparePbHeader(iIdLocal, iSize);
		if (iSt == ::OI::Base::PB_HEADER_NEED_MORE)
		{
			stOutputStream.RollBack();
			break;
		}
		if (iSt != ::OI::Base::PB_HEADER_OK)
		{
			pSession->PassiveClose(PARSER_ERR_OTHER);
			bClosedLocal = true;
			break;
		}
		::OI::Base::BytesStream stBody;
		if (iSize > 0)
		{
			stBody.Append(stOutputStream.GetCurPtr(), iSize);
		}
		stOutputStream.AddCurPos(iSize);
		try
		{
			fnOnFrame(iIdLocal, stBody);
		}
		catch (::OI::Base::ParserException&)
		{
			pSession->PassiveClose(PARSER_ERR_PROTOCOL_SIZE);
			bClosedLocal = true;
		}
	}
	if (!bClosedLocal)
	{
		stInput.Swap(stOutputStream);
		stInput.Erase(0, stOutputStream.GetCurPos());
	}
	return !bClosedLocal;
}

// ---------------------------------------------------------------- ServiceTable

ServiceTable::ServiceTable()
{
}
ServiceTable::~ServiceTable()
{
}

bool ServiceTable::Pick(const std::vector<ServiceInstance>& vList, const std::string& sNameLocal, int32_t iCurrent, ServiceInstance& stOutput)
{
	const ServiceInstance* pBest = NULL;
	for (size_t iIndexLocal = 0; iIndexLocal < vList.size(); ++iIndexLocal)
	{
		if (vList[iIndexLocal].sName != sNameLocal)
		{
			continue;
		}
		if (iCurrent > 0 && vList[iIndexLocal].iId == iCurrent)
		{
			stOutput = vList[iIndexLocal];
			return true;
		}
		if (!pBest || vList[iIndexLocal].iId < pBest->iId)
		{
			pBest = &vList[iIndexLocal];
		}
	}
	if (!pBest)
	{
		return false;
	}
	stOutput = *pBest;
	return true;
}

std::vector<::OI::Net::SessionManager*> ServiceTable::Watchers(const std::string& sNameLocal)
{
	std::vector<::OI::Net::SessionManager*> vWatchers;
	::OI::Thread::MutexGuard stLock(m_stMutex);
	auto stIterator = m_mServices.find(sNameLocal);
	if (stIterator == m_mServices.end())
	{
		return vWatchers;
	}
	vWatchers = stIterator->second.vWatchers;
	return vWatchers;
}

void ServiceTable::Watch(::OI::Net::SessionManager* pManager, const std::string& sNameLocal)
{
	::OI::Thread::MutexGuard stLock(m_stMutex);
	State& stState = m_mServices[sNameLocal];
	for (size_t iIndexLocal = 0; iIndexLocal < stState.vWatchers.size(); ++iIndexLocal)
	{
		if (stState.vWatchers[iIndexLocal] == pManager)
		{
			return;
		}
	}
	stState.vWatchers.push_back(pManager);
}

void ServiceTable::WatchList(::OI::Net::SessionManager* pManager, const std::string& sNameLocal, const std::vector<ServiceInstance>& vInitial)
{
	::OI::Thread::MutexGuard stLock(m_stMutex);
	State& stState = m_mServices[sNameLocal];
	for (const auto* pWatcher : stState.vListWatchers)
	{
		if (pWatcher == pManager)
		{
			return;
		}
	}
	// 已有别的管理器在收时保留上次下发的名单，免得它漏掉这次之后的变化。
	if (stState.vListWatchers.empty())
	{
		stState.vList = vInitial;
		std::sort(stState.vList.begin(), stState.vList.end(), [](const ServiceInstance& stLeft, const ServiceInstance& stRight)
		{
			return stLeft.iId < stRight.iId;
		});
	}
	stState.vListWatchers.push_back(pManager);
	stState.vPendingListWatchers.push_back(pManager);
}

std::vector<std::string> ServiceTable::Names()
{
	std::vector<std::string> vNamesLocal;
	::OI::Thread::MutexGuard stLock(m_stMutex);
	for (const auto& [sName, stState] : m_mServices)
	{
		if (!stState.vWatchers.empty() || !stState.vListWatchers.empty())
		{
			vNamesLocal.push_back(sName);
		}
	}
	return vNamesLocal;
}

void ServiceTable::Apply(const std::vector<std::string>& vNames, const std::vector<ServiceInstance>& vList)
{
	struct Change
	{
		::OI::Net::SessionManager* pManager;
		std::string sIp;
		std::string sPort;
		bool bOnline;
	};
	struct ListChange
	{
		::OI::Net::SessionManager* pManager;
		std::vector<ServiceInstance> vList;
	};
	std::vector<Change> vChanged;
	std::vector<ListChange> vListChanged;
	{
		::OI::Thread::MutexGuard stLock(m_stMutex);
		for (size_t iNumber = 0; iNumber < vNames.size(); ++iNumber)
		{
			auto stIterator = m_mServices.find(vNames[iNumber]);
			if (stIterator == m_mServices.end())
			{
				continue;
			}
			State& stState = stIterator->second;
			if (!stState.vListWatchers.empty())
			{
				std::vector<ServiceInstance> vMatched;
				for (const auto& stInst : vList)
				{
					if (stInst.sName == vNames[iNumber])
					{
						vMatched.push_back(stInst);
					}
				}
				std::sort(vMatched.begin(), vMatched.end(), [](const ServiceInstance& stLeft, const ServiceInstance& stRight)
				{
					return stLeft.iId < stRight.iId;
				});
				const bool bSame = vMatched.size() == stState.vList.size() &&
				                   std::equal(vMatched.begin(), vMatched.end(), stState.vList.begin(), [](const ServiceInstance& stLeft, const ServiceInstance& stRight)
				{
					return stLeft.iId == stRight.iId && stLeft.sIp == stRight.sIp && stLeft.sPort == stRight.sPort && stLeft.sZone == stRight.sZone;
				});
				if (!bSame)
				{
					stState.vList = vMatched;
					for (auto* pWatcher : stState.vListWatchers)
					{
						vListChanged.push_back(ListChange{pWatcher, vMatched});
					}
				}
				else
				{
					for (auto* pWatcher : stState.vPendingListWatchers)
					{
						vListChanged.push_back(ListChange{pWatcher, vMatched});
					}
				}
				stState.vPendingListWatchers.clear();
			}
			ServiceInstance stPick;
			bool bOnlineLocal = Pick(vList, vNames[iNumber], stState.bOnline ? stState.iId : 0, stPick);
			if (bOnlineLocal == stState.bOnline && (!bOnlineLocal || (stPick.iId == stState.iId && stPick.sIp == stState.sIp && stPick.sPort == stState.sPort)))
			{
				continue;
			}
			stState.bOnline = bOnlineLocal;
			stState.iId = bOnlineLocal ? stPick.iId : 0;
			stState.sIp = bOnlineLocal ? stPick.sIp : "";
			stState.sPort = bOnlineLocal ? stPick.sPort : "";
			for (size_t iIndexLocal = 0; iIndexLocal < stState.vWatchers.size(); ++iIndexLocal)
			{
				Change stCharacter;
				stCharacter.pManager = stState.vWatchers[iIndexLocal];
				stCharacter.sIp = stState.sIp;
				stCharacter.sPort = stState.sPort;
				stCharacter.bOnline = bOnlineLocal;
				vChanged.push_back(stCharacter);
			}
		}
	}
	for (size_t iIndexLocal = 0; iIndexLocal < vChanged.size(); ++iIndexLocal)
	{
		vChanged[iIndexLocal].pManager->ApplyService(vChanged[iIndexLocal].sIp, vChanged[iIndexLocal].sPort, vChanged[iIndexLocal].bOnline);
	}
	for (const auto& stChange : vListChanged)
	{
		stChange.pManager->ApplyServiceList(stChange.vList);
	}
}

// ---------------------------------------------------------------- RegistryClient

RegistryClient::RegistryClient()
    : bConnected(false), bConnecting(false), bFailed(false), iSessionId(INVALID_SID), bAddressReady(false),
      iServiceId(0), bPublished(false), bRegistered(false), bLost(false),
      iHeartbeatMilliseconds(1000), bHeartbeatArmed(false), iPullMilliseconds(1000), bPullArmed(false),
      bRegistrationWaiting(false), bGot(false), iCode(OI_ERR_FAIL)
{
}

RegistryClient::~RegistryClient()
{
}

const std::string RegistryClient::GetIDString()
{
	return "RegistryClient";
}

::OI::Net::Session* RegistryClient::CloneSession()
{
	return new RegistryClientSession(this);
}

bool RegistryClient::Configured()
{
	std::string sIpLocal;
	return ::OI::Base::Config::GlobalConfig().GetAs("Registry.ip", sIpLocal) && !sIpLocal.empty();
}

void RegistryClient::Notify()
{
	// 等待方在 _mu 里查完标志才睡下。不拿这把锁就通知，可能落在查完和睡下之间，等待方要白等到超时。
	{
		::OI::Thread::MutexGuard stLock(stMutex);
	}
	stConditionVariable.Broadcast();
}

bool RegistryClient::WaitFlag(const std::atomic<bool>& bFlag)
{
	std::chrono::steady_clock::time_point stDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
#ifdef _SINGLE_THREAD_
	while (!bFlag.load() && !bFailed.load() && std::chrono::steady_clock::now() < stDeadline)
	{
		::OI::Net::Poll::GetInstance().PollWait(1);
	}
	return bFlag.load();
#else
	::OI::Thread::MutexGuard stLock(stMutex);
	while (!bFlag.load() && !bFailed.load())
	{
		if (!stConditionVariable.WaitUntil(stMutex, stDeadline))
		{
			break;
		}
	}
	return bFlag.load();
#endif
}

bool RegistryClient::Connect()
{
	::OI::Thread::MutexGuard stPendingLock(stPendingMutex);
	if (bConnected.load())
	{
		return true;
	}
	if (!bAddressReady)
	{
		std::string sIpLocal;
		std::string sPortLocal;
		std::string sType;
		if (!::OI::Base::Config::GlobalConfig().GetAs("Registry.ip", sIpLocal))
		{
			return false;
		}
		if (!::OI::Base::Config::GlobalConfig().GetAs("Registry.port", sPortLocal))
		{
			return false;
		}
		if (!::OI::Base::Config::GlobalConfig().GetAs("Registry.type", sType))
		{
			return false;
		}
		if (sIpLocal.empty() || sPortLocal.empty())
		{
			return false;
		}
		DefaultInit(sIpLocal, sPortLocal, GetSessionTypeByString(sType));
		bAddressReady = true;
	}
	bool bExpected = false;
	if (!bConnecting.compare_exchange_strong(bExpected, true))
	{
		return true;
	}
	bFailed = false;
	if (OI::Thread::ThreadPool::GetInstance().StopRequested())
	{
		bConnecting = false;
		return false;
	}
	StartClient();
	return true;
}

bool RegistryClient::Ensure()
{
	if (bConnected.load())
	{
		return true;
	}
	if (!Connect())
	{
		return false;
	}
	return bConnected.load() || WaitFlag(bConnected);
}

bool RegistryClient::Send(uint32_t iIdLocal, const ::OI::Rpc::Serialize& stMessage)
{
	sid_t iSessionIdLocal = iSessionId.load();
	if (iSessionIdLocal == INVALID_SID)
	{
		return false;
	}
	return SendBytes(iSessionIdLocal, EncodeRegFrame(iIdLocal, stMessage));
}

bool RegistryClient::EnqueueSentPull(const PendingPull& stPending)
{
	ArmPullTimeout();
	vPendingPulls.push_back(stPending);
	if (vPendingPulls.back().iDeadlineMilliseconds == 0)
	{
		vPendingPulls.back().iDeadlineMilliseconds = ::OI::Timer::SteadyMilliseconds() + kWaitMs;
	}
	try
	{
		::OI::RpcData::RegPull stRequestLocal(stPending.vNames);
		if (!Send(PROTO_REG_PULL, stRequestLocal))
		{
			vPendingPulls.pop_back();
			return false;
		}
	}
	catch (...)
	{
		vPendingPulls.pop_back();
		throw;
	}
	return true;
}

size_t RegistryClient::GetPendingPullCount()
{
	::OI::Thread::MutexGuard stLock(stPendingMutex);
	return vPendingPulls.size() + vDeferredPulls.size();
}

namespace
{
int64_t ReadMaxPendingPulls()
{
	int64_t iMaximumPendingPulls = 4096;
	if (!TryGetConfig("Registry.max_pending_pulls", iMaximumPendingPulls))
	{
		std::string sInvalid;
		double dInvalid = 0;
		bool bInvalid = false;
		if (TryGetConfig("Registry.max_pending_pulls", sInvalid) || TryGetConfig("Registry.max_pending_pulls", dInvalid) || TryGetConfig("Registry.max_pending_pulls", bInvalid))
		{
			throw std::invalid_argument("Registry.max_pending_pulls must be a positive integer");
		}
	}
	if (iMaximumPendingPulls <= 0)
	{
		throw std::invalid_argument("Registry.max_pending_pulls must be a positive integer");
	}
	return iMaximumPendingPulls;
}
}

RegistryPullStats RegistryClient::GetPullStats()
{
	::OI::Thread::MutexGuard stLock(stPendingMutex);
	return {vPendingPulls.size(), vDeferredPulls.size(), vPendingPulls.size() + vDeferredPulls.size(), ReadMaxPendingPulls(), iPullResetCount};
}

bool RegistryClient::CanEnqueuePull()
{
	return vPendingPulls.size() + vDeferredPulls.size() < static_cast<uint64_t>(ReadMaxPendingPulls());
}

void RegistryClient::ArmPullTimeout()
{
	// 调用方持有 stPendingMutex；时间轮的 AddTimer 自带轮锁。
	if (!bPullTimeoutArmed)
	{
		auto pTimer = std::make_unique<PullTimeoutTimer>();
		// 实际到期受 Timer.offset_ms 向上对齐及调度延迟影响。
		::OI::Timer::BaseTimerManager::GetInstance().AddTimer(pTimer.get(), 10);
		pTimer.release();
		bPullTimeoutArmed = true;
	}
}

void RegistryClient::ExpirePulls()
{
	const int64_t iNowMilliseconds = ::OI::Timer::SteadyMilliseconds();
	std::deque<PendingPull> vExpired;
	std::deque<PendingPull> vInvalidated;
	sid_t iExpiredSessionId = INVALID_SID;
	{
		::OI::Thread::MutexGuard stLock(stPendingMutex);
		for (auto it = vDeferredPulls.begin(); it != vDeferredPulls.end();)
		{
			if (it->iDeadlineMilliseconds <= iNowMilliseconds)
			{
				vExpired.push_back(std::move(*it));
				it = vDeferredPulls.erase(it);
			}
			else
			{
				++it;
			}
		}
		for (const auto& stPending : vPendingPulls)
		{
			if (stPending.iDeadlineMilliseconds <= iNowMilliseconds)
			{
				// 协议没有调用号：不能仅删除中间占位。先失效 SID，再释放全部占位。
				iExpiredSessionId = iSessionId.exchange(INVALID_SID);
				++iPullResetCount;
				// 与 Connect 共用锁；先摘掉旧 endpoint，不能让新连接误认旧会话已连接。
				if (iExpiredSessionId != INVALID_SID)
				{
					try
					{
						CloseSession(iExpiredSessionId);
					}
					catch (...)
					{
						::OI::Thread::ReportTaskError(std::current_exception());
					}
				}
				bConnected = false;
				bRegistered = false;
				bFailed = true;
				vInvalidated.swap(vPendingPulls);
				for (const auto& stInvalidated : vInvalidated)
				{
					if (stInvalidated.bSynchronous)
					{
						iCode = stInvalidated.iDeadlineMilliseconds <= iNowMilliseconds ? OI_ERR_TIMEOUT : OI_ERR_CONNECT;
						bGot = true;
					}
				}
				break;
			}
		}
	}
	if (iExpiredSessionId != INVALID_SID)
	{
		Notify();
	}
	const std::vector<ServiceInstance> vEmpty;
	for (const auto& stPending : vExpired)
	{
		FinishPull(stPending.pState, OI_ERR_TIMEOUT, vEmpty);
	}
	for (const auto& stPending : vInvalidated)
	{
		FinishPull(stPending.pState, stPending.iDeadlineMilliseconds <= iNowMilliseconds ? OI_ERR_TIMEOUT : OI_ERR_CONNECT, vEmpty);
	}
}

void RegistryClient::PullAsync(const std::string& sNameLocal, std::function<void(int32_t, const std::vector<ServiceInstance>&)> fnDoneLocal, int64_t iTimeoutMilliseconds)
{
	const int64_t iStartedMilliseconds = ::OI::Timer::SteadyMilliseconds();
	std::shared_ptr<RegPullState> pState;
	const std::vector<ServiceInstance> vEmpty;
	try
	{
		pState = std::make_shared<RegPullState>();
		pState->fnDone = std::move(fnDoneLocal);
	}
	catch (...)
	{
		::OI::Thread::ReportTaskError(std::current_exception());
		InvokePullCallback(std::move(fnDoneLocal), OI_ERR_FAIL, vEmpty);
		return;
	}
	if (iTimeoutMilliseconds <= 0)
	{
		FinishPull(pState, OI_ERR_TIMEOUT, vEmpty);
		return;
	}
	int32_t iResult = OI_ERR_CONNECT;
	try
	{
		if (sNameLocal.empty() || !Configured())
		{
			FinishPull(pState, OI_ERR_FAIL, vEmpty);
			return;
		}
		PendingPull stPending;
		stPending.bSynchronous = false;
		stPending.vNames.push_back(sNameLocal);
		stPending.pState = pState;
		stPending.iDeadlineMilliseconds = iStartedMilliseconds + (std::min)(iTimeoutMilliseconds, (std::numeric_limits<int64_t>::max)() - iStartedMilliseconds);
		::OI::Thread::MutexGuard stLock(stPendingMutex);
		if (!CanEnqueuePull())
		{
			iResult = OI_ERR_BUSY;
		}
		else if (Connect())
		{
			ArmPullTimeout();
			if (stPending.iDeadlineMilliseconds <= ::OI::Timer::SteadyMilliseconds())
			{
				iResult = OI_ERR_TIMEOUT;
			}
			else if (bConnected.load())
			{
				if (EnqueueSentPull(stPending))
				{
					return;
				}
			}
			else if (bConnecting.load())
			{
				vDeferredPulls.push_back(std::move(stPending));
				return;
			}
		}
	}
	catch (...)
	{
		::OI::Thread::ReportTaskError(std::current_exception());
		iResult = OI_ERR_FAIL;
	}
	FinishPull(pState, iResult, vEmpty);
}

void RegistryClient::SendRegister()
{
	::OI::RpcData::RegRegister stRequestLocal(sServiceName, iServiceId, sServiceIp, sServicePort, sServiceZone);
	Send(PROTO_REG_REGISTER, stRequestLocal);
}

void RegistryClient::OnAddSession(::OI::Net::Session* pSession)
{
	std::deque<PendingPull> vFailed;
	std::deque<PendingPull> vSend;
	{
		::OI::Thread::MutexGuard stPendingLock(stPendingMutex);
		vFailed.swap(vPendingPulls);
		vSend.swap(vDeferredPulls);
		iSessionId = pSession->GetSessionID();
		bFailed = false;
		bConnected = true;
		bConnecting = false;
		Notify();
		if (bPublished.load() && !bLost.load())
		{
			try
			{
				SendRegister();
			}
			catch (...)
			{
				::OI::Thread::ReportTaskError(std::current_exception());
			}
		}
		for (auto& stPending : vSend)
		{
			if (stPending.iDeadlineMilliseconds <= ::OI::Timer::SteadyMilliseconds())
			{
				continue;
			}
			try
			{
				if (EnqueueSentPull(stPending))
				{
					stPending.pState.reset();
				}
			}
			catch (...)
			{
				::OI::Thread::ReportTaskError(std::current_exception());
			}
		}
	}
	const std::vector<ServiceInstance> vEmpty;
	for (const auto& stPending : vFailed)
	{
		FinishPull(stPending.pState, OI_ERR_CONNECT, vEmpty);
	}
	for (const auto& stPending : vSend)
	{
		FinishPull(stPending.pState, stPending.iDeadlineMilliseconds <= ::OI::Timer::SteadyMilliseconds() ? OI_ERR_TIMEOUT : OI_ERR_CONNECT, vEmpty);
	}
	LOG_INFO("RegistryClient::OnAddSession sid=%d", pSession->GetSessionID());
}

void RegistryClient::OnDelSession(const ::OI::Net::Session* pSession)
{
	std::deque<PendingPull> vFailed;
	std::deque<PendingPull> vDeferred;
	{
		::OI::Thread::MutexGuard stPendingLock(stPendingMutex);
		if (iSessionId.load() != pSession->GetSessionID())
		{
			return;
		}
		iSessionId = INVALID_SID;
		bConnected = false;
		bRegistered = false;
		bFailed = true;
		vFailed.swap(vPendingPulls);
		vDeferred.swap(vDeferredPulls);
	}
	const std::vector<ServiceInstance> vEmpty;
	for (const auto& stPending : vFailed)
	{
		FinishPull(stPending.pState, stPending.iDeadlineMilliseconds <= ::OI::Timer::SteadyMilliseconds() ? OI_ERR_TIMEOUT : OI_ERR_CONNECT, vEmpty);
	}
	for (const auto& stPending : vDeferred)
	{
		FinishPull(stPending.pState, stPending.iDeadlineMilliseconds <= ::OI::Timer::SteadyMilliseconds() ? OI_ERR_TIMEOUT : OI_ERR_CONNECT, vEmpty);
	}
	Notify();
	LOG_INFO("RegistryClient::OnDelSession sid=%d", pSession->GetSessionID());
}

void RegistryClient::OnConnectFailed()
{
	std::deque<PendingPull> vFailed;
	{
		::OI::Thread::MutexGuard stPendingLock(stPendingMutex);
		bConnecting = false;
		bFailed = true;
		vFailed.swap(vDeferredPulls);
	}
	const std::vector<ServiceInstance> vEmpty;
	for (const auto& stPending : vFailed)
	{
		FinishPull(stPending.pState, stPending.iDeadlineMilliseconds <= ::OI::Timer::SteadyMilliseconds() ? OI_ERR_TIMEOUT : OI_ERR_CONNECT, vEmpty);
	}
	Notify();
	LOG_TRACE("RegistryClient::OnConnectFailed");
}

void RegistryClient::OnFrame(sid_t iSessionIdLocal, uint32_t iIdLocal, ::OI::Base::BytesStream& stBody)
{
	// 回包消费、同步结果发布与超时撤销共用同一把锁。
	::OI::Thread::MutexGuard stPendingLock(stPendingMutex);
	if (iSessionIdLocal != iSessionId.load())
	{
		return;
	}
	if (iIdLocal == PROTO_REG_REGISTER_RES)
	{
		::OI::RpcData::RegRegisterRes stResponseLocal;
		stResponseLocal.iCode = OI_ERR_FAIL;
		stResponseLocal.deserialize(stBody);
		if (stResponseLocal.iCode == OI_OK)
		{
			bRegistered = true;
		}
		else if (stResponseLocal.iCode == OI_ERR_EXIST && !bRegistrationWaiting.load())
		{
			bLost = true;
			fprintf(stderr, "registry %s id=%d 已被别的进程占用，停止登记\n", sServiceName.c_str(), iServiceId);
			fflush(stderr);
			LOG_ERROR("registry %s id=%d taken by another process, stop register", sServiceName.c_str(), iServiceId);
		}
		if (bRegistrationWaiting.load())
		{
			iCode = stResponseLocal.iCode;
			bGot = true;
			Notify();
		}
		return;
	}
	if (iIdLocal == PROTO_REG_HEARTBEAT_RES)
	{
		::OI::RpcData::RegHeartbeatRes stResponseLocal;
		stResponseLocal.iCode = OI_ERR_FAIL;
		stResponseLocal.deserialize(stBody);
		if (stResponseLocal.iCode == OI_ERR_EXIST)
		{
			bLost = true;
			bRegistered = false;
			fprintf(stderr, "registry %s id=%d 已被别的进程占用，停止登记\n", sServiceName.c_str(), iServiceId);
			fflush(stderr);
			LOG_ERROR("registry %s id=%d taken by another process, stop heartbeat", sServiceName.c_str(), iServiceId);
		}
		else if (stResponseLocal.iCode != OI_OK)
		{
			// registry 重启过，或 Redis 暂时不可用。下一次心跳重新登记。
			bRegistered = false;
		}
		return;
	}
	if (iIdLocal == PROTO_REG_PULL_RES)
	{
		::OI::RpcData::RegPullRes stResponseLocal;
		stResponseLocal.iCode = OI_ERR_FAIL;
		stResponseLocal.deserialize(stBody);
		PendingPull stPending;
		stPending.bSynchronous = false;
		{
			::OI::Thread::MutexGuard stLock(stPendingMutex);
			if (vPendingPulls.empty())
			{
				return;
			}
			stPending = vPendingPulls.front();
		}
		std::vector<ServiceInstance> vList;
		for (size_t iIndexLocal = 0; iIndexLocal < stResponseLocal.vList.size(); ++iIndexLocal)
		{
			ServiceInstance stInst;
			stInst.sName = stResponseLocal.vList[iIndexLocal].sName;
			stInst.iId = stResponseLocal.vList[iIndexLocal].iId;
			stInst.sIp = stResponseLocal.vList[iIndexLocal].sIp;
			stInst.sPort = stResponseLocal.vList[iIndexLocal].sPort;
			stInst.sZone = stResponseLocal.vList[iIndexLocal].sZone;
			vList.push_back(stInst);
		}
		vPendingPulls.pop_front();
		// 已消费该 FIFO 回包，可以安全释放这一项；过期结果不能再发布成功。
		if (stPending.iDeadlineMilliseconds <= ::OI::Timer::SteadyMilliseconds())
		{
			stResponseLocal.iCode = OI_ERR_TIMEOUT;
			vList.clear();
		}
		if (stPending.pState)
		{
			FinishPull(stPending.pState, stResponseLocal.iCode, vList);
			return;
		}
		if (stPending.bSynchronous)
		{
			vResponseList.swap(vList);
			iCode = stResponseLocal.iCode;
			bGot = true;
			Notify();
			return;
		}
		// registry 自己连不上 Redis 时不知道真实情况，保持原状。
		if (stResponseLocal.iCode == OI_OK)
		{
			ServiceTable::GetInstance().Apply(stPending.vNames, vList);
		}
		return;
	}
}

bool RegistryClient::PullSync(const std::string& sNameLocal, int32_t& iCodeLocal, std::vector<ServiceInstance>& vList)
{
	::OI::Thread::MutexGuard stSyncLock(stSyncMutex);
	vList.clear();
	iCodeLocal = OI_ERR_FAIL;
	if (sNameLocal.empty() || !Configured())
	{
		return false;
	}
	RejectPollCallback("PullSync");
	if (!Ensure())
	{
		LOG_TRACE("registry lookup %s connect failed", sNameLocal.c_str());
		return false;
	}
	PendingPull stPending;
	stPending.bSynchronous = true;
	stPending.vNames.push_back(sNameLocal);
	bool bSent = false;
	{
		::OI::Thread::MutexGuard stLock(stPendingMutex);
		if (!CanEnqueuePull())
		{
			iCodeLocal = OI_ERR_BUSY;
			return false;
		}
		bGot = false;
		iCode = OI_ERR_FAIL;
		bSent = EnqueueSentPull(stPending);
	}
	if (!bSent || !WaitFlag(bGot))
	{
		{
			::OI::Thread::MutexGuard stLock(stPendingMutex);
			// 连接失败/停止也会提前结束等待，先取消本次全局同步结果的写入资格。
			for (auto& stPendingLocal : vPendingPulls)
			{
				if (stPendingLocal.bSynchronous)
				{
					stPendingLocal.bSynchronous = false;
					break;
				}
			}
		}
		// 未到期的占位只用于 FIFO 配对；统一扫描到期后失效整代并回收。
		ExpirePulls();
		iCodeLocal = bSent ? OI_ERR_TIMEOUT : OI_ERR_CONNECT;
		LOG_TRACE("registry lookup %s %s", sNameLocal.c_str(), bSent ? "timeout" : "send failed");
		return false;
	}
	::OI::Thread::MutexGuard stResultLock(stPendingMutex);
	iCodeLocal = iCode;
	if (iCodeLocal == OI_ERR_TIMEOUT || iCodeLocal == OI_ERR_CONNECT)
	{
		return false;
	}
	vList = vResponseList;
	return true;
}

bool RegistryClient::Lookup(const std::string& sNameLocal, std::string& sIpLocal, std::string& sPortLocal)
{
	int32_t iCodeLocal = OI_ERR_FAIL;
	std::vector<ServiceInstance> vList;
	if (!PullSync(sNameLocal, iCodeLocal, vList))
	{
		return false;
	}
	ServiceInstance stPick;
	if (iCodeLocal != OI_OK || !ServiceTable::Pick(vList, sNameLocal, 0, stPick))
	{
		LOG_TRACE("registry lookup %s miss", sNameLocal.c_str());
		return false;
	}
	sIpLocal = stPick.sIp;
	sPortLocal = stPick.sPort;
	LOG_TRACE("registry lookup %s -> %d %s:%s", sNameLocal.c_str(), stPick.iId, sIpLocal.c_str(), sPortLocal.c_str());
	return true;
}

void RegistryClient::Watch(::OI::Net::SessionManager* pManager, const std::string& sNameLocal)
{
	if (!pManager || sNameLocal.empty() || !Configured())
	{
		return;
	}
	ServiceTable::GetInstance().Watch(pManager, sNameLocal);
	ArmPull();
}

void RegistryClient::WatchList(::OI::Net::SessionManager* pManager, const std::string& sNameLocal, const std::vector<ServiceInstance>& vInitial)
{
	if (!pManager || sNameLocal.empty() || !Configured())
	{
		return;
	}
	ServiceTable::GetInstance().WatchList(pManager, sNameLocal, vInitial);
	ArmPull();
}

void RegistryClient::ArmPull()
{
	if (bPullArmed)
	{
		return;
	}
	TryGetConfig("Registry.pull_ms", iPullMilliseconds);
	if (iPullMilliseconds <= 0)
	{
		iPullMilliseconds = 1000;
	}
	::OI::Timer::BaseTimerManager::GetInstance().AddTimer(new PullTimer(), iPullMilliseconds);
	bPullArmed = true;
}

void RegistryClient::ArmHeartbeat()
{
	if (bHeartbeatArmed)
	{
		return;
	}
	TryGetConfig("Registry.heartbeat_ms", iHeartbeatMilliseconds);
	if (iHeartbeatMilliseconds <= 0)
	{
		iHeartbeatMilliseconds = 1000;
	}
	::OI::Timer::BaseTimerManager::GetInstance().AddTimer(new HeartbeatTimer(), iHeartbeatMilliseconds);
	bHeartbeatArmed = true;
}

void RegistryClient::Pull()
{
	std::vector<std::string> vNamesLocal = ServiceTable::GetInstance().Names();
	if (vNamesLocal.empty())
	{
		return;
	}
	if (!bConnected.load())
	{
		Connect();
		return;
	}
	PendingPull stPending;
	stPending.bSynchronous = false;
	stPending.vNames = vNamesLocal;
	::OI::Thread::MutexGuard stLock(stPendingMutex);
	if (!bConnected.load() || !CanEnqueuePull())
	{
		return;
	}
	EnqueueSentPull(stPending);
}

void RegistryClient::Touch()
{
	if (!bPublished.load() || bLost.load())
	{
		return;
	}
	if (!bConnected.load())
	{
		Connect();
		return;
	}
	if (!bRegistered.load())
	{
		SendRegister();
		return;
	}
	::OI::RpcData::RegHeartbeat stRequestLocal(sServiceName, iServiceId);
	Send(PROTO_REG_HEARTBEAT, stRequestLocal);
}

namespace
{
bool TakeAdvertise(const std::string& sKey, std::string& sValue)
{
	std::string sConfigured;
	if (!::OI::Base::Config::GlobalConfig().GetAs(sKey, sConfigured) || sConfigured.empty())
	{
		return false;
	}
	sValue = sConfigured;
	return true;
}

bool PublishAddress(const std::string& sSection, const std::string& sBindIp, const std::string& sBindPort, std::string& sIpLocal, std::string& sPortLocal)
{
	sIpLocal = sBindIp;
	sPortLocal = sBindPort;
	TakeAdvertise("Service.advertise_ip", sIpLocal);
	TakeAdvertise("Service.advertise_port", sPortLocal);
	TakeAdvertise(sSection + ".advertise_ip", sIpLocal);
	TakeAdvertise(sSection + ".advertise_port", sPortLocal);
	::OI::Net::NetAddr stAddr;
	if (sIpLocal.empty() || sPortLocal.empty() || sIpLocal == "*" || (stAddr.Init(sIpLocal, sPortLocal) && stAddr.IsUnspecified()))
	{
		return false;
	}
	return true;
}
}

bool RegistryClient::Publish(const std::string& sSection, const std::string& sIpLocal, const std::string& sPortLocal)
{
	::OI::Thread::MutexGuard stSyncLock(stSyncMutex);
	if (sSection == "Registry" || !Configured())
	{
		return true;
	}
	std::string sNameLocal;
	if (!::OI::Base::Config::GlobalConfig().GetAs("Service.name", sNameLocal) || sNameLocal.empty())
	{
		return true;
	}
	std::string sZoneLocal;
	::OI::Base::Config::GlobalConfig().GetAs("Service.zone", sZoneLocal);
	auto fnToken = [](const std::string& sText)
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
	};
	if (!fnToken(sNameLocal) || (!sZoneLocal.empty() && !fnToken(sZoneLocal)))
	{
		fprintf(stderr, "registry publish name=\"%s\" zone=\"%s\" 只能用字母、数字、_、-，名字不能为空，不能带 . 或 *\n", sNameLocal.c_str(), sZoneLocal.c_str());
		fflush(stderr);
		LOG_ERROR("registry publish invalid name=%s zone=%s", sNameLocal.c_str(), sZoneLocal.c_str());
		return false;
	}
	int32_t iIdLocal = 0;
	if (!::OI::Base::Config::GlobalConfig().GetAs("Service.id", iIdLocal) || iIdLocal <= 0)
	{
		fprintf(stderr, "registry publish %s: [Service] id 必须是正整数\n", sNameLocal.c_str());
		fflush(stderr);
		LOG_ERROR("registry publish %s invalid id", sNameLocal.c_str());
		return false;
	}
	std::string sAdvertiseIp;
	std::string sAdvertisePort;
	if (!PublishAddress(sSection, sIpLocal, sPortLocal, sAdvertiseIp, sAdvertisePort))
	{
		LOG_TRACE("registry publish %s skip bind %s:%s", sNameLocal.c_str(), sIpLocal.c_str(), sPortLocal.c_str());
		return true;
	}
	if (bPublished.load())
	{
		if (sServiceIp != sAdvertiseIp || sServicePort != sAdvertisePort)
		{
			LOG_TRACE("registry publish %s keep %s:%s, skip %s:%s", sNameLocal.c_str(),
			    sServiceIp.c_str(), sServicePort.c_str(), sAdvertiseIp.c_str(), sAdvertisePort.c_str());
		}
		return true;
	}
	RejectPollCallback("Publish");
	sServiceName = sNameLocal;
	iServiceId = iIdLocal;
	sServiceIp = sAdvertiseIp;
	sServicePort = sAdvertisePort;
	sServiceZone = sZoneLocal;
	bool bGotLocal = false;
	int32_t iCodeLocal = OI_ERR_FAIL;
	if (Ensure())
	{
		{
			::OI::Thread::MutexGuard stPendingLock(stPendingMutex);
			bGot = false;
			iCode = OI_ERR_FAIL;
			bRegistrationWaiting = true;
			SendRegister();
		}
		bGotLocal = WaitFlag(bGot);
		{
			::OI::Thread::MutexGuard stPendingLock(stPendingMutex);
			iCodeLocal = iCode;
			bRegistrationWaiting = false;
		}
	}
	if (bGotLocal && iCodeLocal == OI_ERR_EXIST)
	{
		fprintf(stderr, "registry publish %s id=%d 已经在线，拒绝启动\n", sNameLocal.c_str(), iIdLocal);
		fflush(stderr);
		LOG_ERROR("registry publish %s id=%d already online", sNameLocal.c_str(), iIdLocal);
		return false;
	}
	bPublished = true;
	if (bGotLocal && iCodeLocal == OI_OK)
	{
		LOG_TRACE("registry publish %s id=%d %s:%s", sNameLocal.c_str(), iIdLocal, sAdvertiseIp.c_str(), sAdvertisePort.c_str());
	}
	else
	{
		LOG_ERROR("registry publish %s id=%d not confirmed, retry on heartbeat", sNameLocal.c_str(), iIdLocal);
	}
	ArmHeartbeat();
	return true;
}

}
}
