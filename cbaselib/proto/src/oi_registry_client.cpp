#include "oi_registry_client.h"
#include "oi_proto_id.h"
#include "framework_protocol/oi_rpcdata_regregister.h"
#include "framework_protocol/oi_rpcdata_regheartbeat.h"
#include "framework_protocol/oi_rpcdata_reginstance.h"
#include "framework_protocol/oi_rpcdata_regpull.h"
#include "oi_rpc_coro.h"
#include "oi_rpc_session.h"
#include "oi_socket.h"
#include "oi_config.h"
#include "oi_log.h"
#include "oi_poll.h"
#include "oi_thread.h"
#include "oi_duration.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <utility>

namespace OI
{
namespace Base
{

namespace
{
const int kWaitMs = 1000;

void RejectPollCallback(const char* pOperation)
{
	if (::OI::Net::Poll::GetInstance().InPollCallback())
	{
		throw std::logic_error(std::string("RegistryClient::") + pOperation + " cannot run in a poll callback");
	}
}

std::vector<ServiceInstance> ToInstances(const std::vector<::OI::RpcData::RegInstance>& vInstances)
{
	std::vector<ServiceInstance> vList;
	vList.reserve(vInstances.size());
	for (const auto& stInstance : vInstances)
	{
		ServiceInstance stService;
		stService.sName = stInstance.sName;
		stService.iId = stInstance.iId;
		stService.sIp = stInstance.sIp;
		stService.sPort = stInstance.sPort;
		stService.sZone = stInstance.sZone;
		vList.push_back(std::move(stService));
	}
	return vList;
}

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

bool PublishAddress(const std::string& sSection, const std::string& sBindIp, const std::string& sBindPort, bool bLegacyName, std::string& sIp, std::string& sPort)
{
	sIp = sBindIp;
	sPort = sBindPort;
	TakeAdvertise("Service.advertise_ip", sIp);
	if (bLegacyName)
	{
		TakeAdvertise("Service.advertise_port", sPort);
	}
	TakeAdvertise(sSection + ".advertise_ip", sIp);
	TakeAdvertise(sSection + ".advertise_port", sPort);
	::OI::Net::NetAddr stAddr;
	return !sIp.empty() && !sPort.empty() && sIp != "*" && !(stAddr.Init(sIp, sPort) && stAddr.IsUnspecified());
}
}

RegistryClient::RegistryClient()
    : m_bConnected(false), m_bConnecting(false), m_bFailed(false), m_iSessionId(INVALID_SID), m_bAddressReady(false),
      m_bActive(false), m_bPullWanted(false),
      m_bRefresh(false), m_iHeartbeatMilliseconds(1000), m_iPullMilliseconds(1000), m_iNextHeartbeatMilliseconds(0),
      m_iNextPullMilliseconds(0), m_bReportKnown(false), m_bReport(false)
{
}

RegistryClient::~RegistryClient()
{
}

const std::string RegistryClient::GetIDString()
{
	return "RegistryClient";
}

int32_t RegistryClient::EnsureSource()
{
	if (m_bSourceReady.load(std::memory_order_acquire))
	{
		return m_iSourceStatus;
	}
	::OI::Thread::MutexGuard stLock(m_stPublishMutex);
	if (m_bSourceReady.load(std::memory_order_acquire))
	{
		return m_iSourceStatus;
	}
	std::string sIpLocal;
	if (::OI::Base::Config::GlobalConfig().GetAs("Registry.ip", sIpLocal) && !sIpLocal.empty())
	{
		m_eSource = Source::Registry;
		m_iSourceStatus = OI_OK;
	}
	else
	{
		m_iSourceStatus = m_stStatic.Load();
		if (m_iSourceStatus == OI_OK && !m_stStatic.Empty())
		{
			m_eSource = Source::Local;
		}
		else
		{
			m_eSource = Source::None;
		}
	}
	m_bSourceReady.store(true, std::memory_order_release);
	return m_iSourceStatus;
}

void RegistryClient::NoteReport(bool bReport)
{
	m_bReport.store(bReport, std::memory_order_relaxed);
	m_bReportKnown.store(true, std::memory_order_release);
}

int32_t RegistryClient::ProbeReport(const std::string& sName)
{
	struct State
	{
		std::atomic<bool> bDone{false};
		int32_t iCode = OI_ERR_FAIL;
		bool bReport = false;
	};
	auto pState = std::make_shared<State>();
	std::vector<std::string> vNames{sName};
	::OI::RpcData::RegPull stRequest(std::move(vNames));
	CallRegistry<::OI::RpcData::RegPull>(stRequest, kWaitMs, [this, pState](int32_t iCode, ::OI::RpcData::RegPullRes& stResponse)
	{
		if (iCode == OI_OK)
		{
			pState->bReport = !stResponse.bStatic;
		}
		pState->iCode = iCode;
		pState->bDone.store(true, std::memory_order_release);
		Notify();
	});
	const int32_t iWaitStatus = WaitDone(pState->bDone);
	if (iWaitStatus != OI_OK)
	{
		return iWaitStatus;
	}
	if (pState->iCode == OI_OK)
	{
		NoteReport(pState->bReport);
	}
	return pState->iCode;
}

bool RegistryClient::Configured()
{
	if (EnsureSource() != OI_OK)
	{
		return true;
	}
	return m_eSource != Source::None;
}

int32_t RegistryClient::ConfigStatus()
{
	return EnsureSource();
}

void RegistryClient::Notify()
{
	{
		::OI::Thread::MutexGuard stLock(m_stMutex);
	}
	m_stCondition.Broadcast();
}

int32_t RegistryClient::WaitFlag(const std::atomic<bool>& bFlag)
{
	const auto stDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
#ifdef _SINGLE_THREAD_
	while (!bFlag.load() && !m_bFailed.load() && std::chrono::steady_clock::now() < stDeadline)
	{
		::OI::Net::Poll::GetInstance().PollWait(1);
	}
#else
	::OI::Thread::MutexGuard stLock(m_stMutex);
	while (!bFlag.load() && !m_bFailed.load())
	{
		if (!m_stCondition.WaitUntil(m_stMutex, stDeadline))
		{
			break;
		}
	}
#endif
	if (bFlag.load())
	{
		return OI_OK;
	}
	if (m_bFailed.load())
	{
		return OI_ERR_CONNECT;
	}
	return OI_ERR_TIMEOUT;
}

int32_t RegistryClient::WaitDone(const std::atomic<bool>& bFlag)
{
	const auto stDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs + 500);
#ifdef _SINGLE_THREAD_
	while (!bFlag.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < stDeadline)
	{
		::OI::Net::Poll::GetInstance().PollWait(1);
	}
#else
	::OI::Thread::MutexGuard stLock(m_stMutex);
	while (!bFlag.load(std::memory_order_acquire))
	{
		if (!m_stCondition.WaitUntil(m_stMutex, stDeadline))
		{
			break;
		}
	}
#endif
	return bFlag.load(std::memory_order_acquire) ? OI_OK : OI_ERR_TIMEOUT;
}

int32_t RegistryClient::Connect()
{
	const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("RegistryClient::Connect");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}
	{
		::OI::Thread::MutexGuard stLock(m_stStateMutex);
		if (m_bConnected.load())
		{
			return OI_OK;
		}
		if (!m_bAddressReady)
		{
			std::string sIpLocal;
			std::string sPortLocal;
			std::string sType;
			if (!::OI::Base::Config::GlobalConfig().GetAs("Registry.ip", sIpLocal) ||
			    !::OI::Base::Config::GlobalConfig().GetAs("Registry.port", sPortLocal) ||
			    !::OI::Base::Config::GlobalConfig().GetAs("Registry.type", sType) ||
			    sIpLocal.empty() || sPortLocal.empty())
			{
				return OI_ERR_INVALID_ARGUMENT;
			}
			DefaultInit(sIpLocal, sPortLocal, GetSessionTypeByString(sType));
			m_bAddressReady = true;
		}
		bool bExpected = false;
		if (!m_bConnecting.compare_exchange_strong(bExpected, true))
		{
			return OI_OK;
		}
		m_bFailed = false;
		if (::OI::Thread::ThreadPool::GetInstance().StopRequested())
		{
			m_bConnecting = false;
			return OI_ERR_CONNECT;
		}
	}
	try
	{
		StartClient();
	}
	catch (...)
	{
		OnConnectFailed();
		throw;
	}
	return OI_OK;
}

int32_t RegistryClient::Ensure()
{
	if (m_bConnected.load())
	{
		return OI_OK;
	}
	const int32_t iStatus = Connect();
	if (iStatus != OI_OK)
	{
		return iStatus;
	}
	return WaitFlag(m_bConnected);
}

template <typename Req>
void RegistryClient::CallRegistry(const Req& stRequest, int64_t iTimeoutMilliseconds,
    std::function<void(int32_t, typename ::OI::Rpc::RpcProtoId<Req>::Response&)> fnDone)
{
	using Res = typename ::OI::Rpc::RpcProtoId<Req>::Response;
	static_assert(::OI::Rpc::RpcProtoId<Req>::iValue == ::OI::Rpc::RpcProtoId<Res>::iValue);
	iTimeoutMilliseconds = ::OI::Rpc::NormalizeRpcTimeoutMilliseconds(iTimeoutMilliseconds);
	struct State
	{
		int32_t iCode = OI_ERR_FAIL;
		std::atomic<uint64_t> iCallId{0};
		Res stRes;
		std::function<void(int32_t, Res&)> fnDone;
	};
	std::shared_ptr<State> pState;
	try
	{
		pState = std::make_shared<State>();
		pState->fnDone = std::move(fnDone);
	}
	catch (...)
	{
		::OI::Thread::ReportTaskError(std::current_exception());
		Res stResponse;
		try
		{
			if (fnDone)
			{
				fnDone(OI_ERR_FAIL, stResponse);
			}
		}
		catch (...)
		{
			::OI::Thread::ReportTaskError(std::current_exception());
		}
		return;
	}
	auto fnFinish = [pState]()
	{
		try
		{
			if (pState->fnDone)
			{
				pState->fnDone(pState->iCode, pState->stRes);
			}
		}
		catch (...)
		{
			::OI::Thread::ReportTaskError(std::current_exception());
		}
	};
	const sid_t iSessionIdLocal = m_iSessionId.load();
	if (iSessionIdLocal == INVALID_SID)
	{
		pState->iCode = OI_ERR_CONNECT;
		fnFinish();
		return;
	}
	uint64_t iCallId = 0;
	bool bBound = false;
	try
	{
		iCallId = ::OI::Rpc::RpcCallManager::GetInstance().Bind(this, iSessionIdLocal, ::OI::Rpc::RpcProtoId<Res>::iValue, {}, &pState->iCode, iTimeoutMilliseconds,
		    [pState](::OI::Base::BytesStream& stInput, sid_t iSourceSession)
		{
			try
			{
				stInput >> pState->iCode;
				if (pState->iCode == OI_OK)
				{
					Res stDecoded;
					stDecoded.deserialize(stInput);
					pState->stRes = std::move(stDecoded);
				}
			}
			catch (...)
			{
				pState->iCode = OI_ERR_FAIL;
				::OI::Rpc::RpcCallManager::ReportError(0, pState->iCallId.load(), iSourceSession, ::OI::Rpc::RpcProtoId<Res>::iValue, "decode_response", std::current_exception());
			}
			return true;
		}, fnFinish);
		bBound = true;
		pState->iCallId.store(iCallId);
		::OI::Base::BytesStream stBody;
		stBody << (iCallId | uint64_t(RPC_REQ_MASK));
		stRequest.serialize(stBody);
		auto stFrame = ::OI::Rpc::EncodePbFrame(::OI::Rpc::RpcProtoId<Req>::iValue, stBody);
		const int32_t iStatus = SendFrame(iSessionIdLocal, stFrame.GetMutableBuff());
		if (iStatus != OI_OK)
		{
			::OI::Rpc::RpcCallManager::GetInstance().Wake(iCallId, iStatus, nullptr);
		}
	}
	catch (...)
	{
		::OI::Thread::ReportTaskError(std::current_exception());
		if (bBound)
		{
			::OI::Rpc::RpcCallManager::GetInstance().Wake(iCallId, OI_ERR_FAIL, nullptr);
		}
		else
		{
			pState->iCode = OI_ERR_FAIL;
			fnFinish();
		}
	}
}

void RegistryClient::Activate()
{
	if (m_bActive.load(std::memory_order_acquire))
	{
		return;
	}
	::OI::Thread::MutexGuard stLock(m_stStateMutex);
	if (m_bActive.load(std::memory_order_acquire))
	{
		return;
	}
	int32_t iHeartbeatMilliseconds = 1000;
	int32_t iPullMilliseconds = 1000;
	TryGetConfig("Registry.heartbeat_ms", iHeartbeatMilliseconds);
	TryGetConfig("Registry.pull_ms", iPullMilliseconds);
	m_iHeartbeatMilliseconds = iHeartbeatMilliseconds > 0 ? iHeartbeatMilliseconds : 1000;
	m_iPullMilliseconds = iPullMilliseconds > 0 ? iPullMilliseconds : 1000;
	m_bActive.store(true, std::memory_order_release);
}

::OI::Coro::Task RegistryClient::OnSecondUpdate(time_t)
{
	if (EnsureSource() != OI_OK || m_eSource == Source::None || !m_bActive.load(std::memory_order_acquire))
	{
		co_return;
	}
	const int64_t iNowMilliseconds = ::OI::Timer::SteadyMilliseconds() + 500;
	if (m_eSource == Source::Local)
	{
		if (m_bPullWanted.exchange(false) || iNowMilliseconds >= m_iNextPullMilliseconds)
		{
			m_iNextPullMilliseconds = iNowMilliseconds + m_iPullMilliseconds;
			m_stStatic.ApplyWatched();
		}
		co_return;
	}
	if (!m_bConnected.load())
	{
		if (!m_bConnecting.load())
		{
			Connect();
		}
		co_return;
	}
	if (m_bRefresh.exchange(false))
	{
		m_iNextHeartbeatMilliseconds = 0;
		m_iNextPullMilliseconds = 0;
	}
	// 先问要不要上报。还没问到就先不上报。
	if (!m_bReportKnown.load(std::memory_order_acquire))
	{
		std::vector<std::string> vProbe;
		::OI::RpcData::RegPullRes stProbe;
		const int32_t iProbe = co_await Call(m_iSessionId.load(), ::OI::RpcData::RegPull(vProbe), stProbe, ::OI::Timer::Milliseconds(kWaitMs));
		if (iProbe == OI_OK)
		{
			NoteReport(!stProbe.bStatic);
		}
	}
	// 每秒才跑一次，提前半秒算到点，间隔不会被拖成两秒。
	if (m_bReportKnown.load(std::memory_order_acquire) && m_bReport.load() && iNowMilliseconds >= m_iNextHeartbeatMilliseconds)
	{
		m_iNextHeartbeatMilliseconds = iNowMilliseconds + m_iHeartbeatMilliseconds;
		co_await RenewServices(m_iSessionId.load());
	}
	if (m_bPullWanted.exchange(false) || iNowMilliseconds >= m_iNextPullMilliseconds)
	{
		m_iNextPullMilliseconds = iNowMilliseconds + m_iPullMilliseconds;
		const std::vector<std::string> vNames = ServiceTable::GetInstance().Names();
		if (!vNames.empty())
		{
			::OI::RpcData::RegPullRes stResponse;
			const int32_t iCode = co_await Call(m_iSessionId.load(), ::OI::RpcData::RegPull(vNames), stResponse, ::OI::Timer::Milliseconds(kWaitMs));
			// registry 暂时给不了名单，缓存保持原状，下一秒重拉。
			if (iCode == OI_OK)
			{
				NoteReport(!stResponse.bStatic);
				ServiceTable::GetInstance().Apply(vNames, ToInstances(stResponse.vList));
			}
			else
			{
				m_iNextPullMilliseconds = 0;
				LOG_TRACE("registry pull names=%zu code=%d", vNames.size(), iCode);
			}
		}
	}
}

::OI::Coro::Task RegistryClient::RenewServices(sid_t iSessionIdLocal)
{
	for (size_t iIndex = 0;; ++iIndex)
	{
		Publication stPublication;
		{
			::OI::Thread::MutexGuard stLock(m_stStateMutex);
			if (m_iSessionId.load() != iSessionIdLocal || iIndex >= m_vPublications.size())
			{
				co_return;
			}
			if (m_vPublications[iIndex].bLost)
			{
				continue;
			}
			stPublication = m_vPublications[iIndex];
		}
		const auto& stService = stPublication.stService;
		int32_t iCode;
		if (stPublication.iRegisteredSession == iSessionIdLocal)
		{
			::OI::RpcData::RegHeartbeatRes stResponse;
			iCode = co_await Call(iSessionIdLocal, ::OI::RpcData::RegHeartbeat(stService.sName, stService.iId), stResponse, ::OI::Timer::Milliseconds(kWaitMs));
		}
		else
		{
			::OI::RpcData::RegRegister stRequest(stService.sName, stService.iId, stService.sIp, stService.sPort, stService.sZone);
			::OI::RpcData::RegRegisterRes stResponse;
			iCode = co_await Call(iSessionIdLocal, stRequest, stResponse, ::OI::Timer::Milliseconds(kWaitMs));
		}
		{
			::OI::Thread::MutexGuard stLock(m_stStateMutex);
			if (m_iSessionId.load() != iSessionIdLocal)
			{
				co_return;
			}
			auto& stCurrent = m_vPublications[iIndex];
			stCurrent.iRegisteredSession = iCode == OI_OK ? iSessionIdLocal : INVALID_SID;
			stCurrent.bLost = iCode == OI_ERR_EXIST;
		}
		if (iCode == OI_ERR_EXIST)
		{
			fprintf(stderr, "registry %s id=%d 已被别的进程占用，停止登记\n", stService.sName.c_str(), stService.iId);
			LOG_ERROR("registry %s id=%d taken by another process, stop register", stService.sName.c_str(), stService.iId);
		}
	}
}

void RegistryClient::OnAddSession(::OI::Net::Session* pSession)
{
	{
		::OI::Thread::MutexGuard stLock(m_stStateMutex);
		m_iSessionId = pSession->GetSessionID();
		m_bFailed = false;
		m_bConnected = true;
		m_bConnecting = false;
	}
	Notify();
	// 重连后下一跳马上补登记、拉一次。
	m_bRefresh = true;
	LOG_INFO("RegistryClient::OnAddSession sid=%d", pSession->GetSessionID());
}

void RegistryClient::OnDelSession(const ::OI::Net::Session* pSession)
{
	{
		::OI::Thread::MutexGuard stLock(m_stStateMutex);
		if (m_iSessionId.load() != pSession->GetSessionID())
		{
			return;
		}
		m_iSessionId = INVALID_SID;
		m_bConnected = false;
		m_bReportKnown = false;
		m_bFailed = true;
	}
	Notify();
	LOG_INFO("RegistryClient::OnDelSession sid=%d", pSession->GetSessionID());
}

void RegistryClient::OnConnectFailed()
{
	{
		::OI::Thread::MutexGuard stLock(m_stStateMutex);
		m_bConnecting = false;
		m_bFailed = true;
	}
	Notify();
	LOG_TRACE("RegistryClient::OnConnectFailed");
}

int32_t RegistryClient::PullSync(const std::string& sNameLocal, std::vector<ServiceInstance>& vList)
{
	const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("RegistryClient::PullSync");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}
	vList.clear();
	const int32_t iSourceStatus = EnsureSource();
	if (iSourceStatus != OI_OK)
	{
		return iSourceStatus;
	}
	if (sNameLocal.empty() || m_eSource == Source::None)
	{
		return sNameLocal.empty() ? OI_ERR_INVALID_ARGUMENT : OI_ERR_NOT_FOUND;
	}
	if (m_eSource == Source::Local)
	{
		Activate();
		m_stStatic.Collect(sNameLocal, vList);
		ServiceTable::GetInstance().Store(sNameLocal, vList);
		return OI_OK;
	}
	RejectPollCallback("PullSync");
	Activate();
	const int32_t iConnectStatus = Ensure();
	if (iConnectStatus != OI_OK)
	{
		LOG_TRACE("registry pull service=%s stage=connect code=%d", sNameLocal.c_str(), iConnectStatus);
		return iConnectStatus;
	}
	struct State
	{
		std::atomic<bool> bDone{false};
		int32_t iCode = OI_ERR_FAIL;
		std::vector<ServiceInstance> vList;
	};
	auto pState = std::make_shared<State>();
	::OI::RpcData::RegPull stRequest(std::vector<std::string>{sNameLocal});
	CallRegistry<::OI::RpcData::RegPull>(stRequest, kWaitMs, [this, pState](int32_t iCode, ::OI::RpcData::RegPullRes& stResponse)
	{
		try
		{
			if (iCode == OI_OK)
			{
				NoteReport(!stResponse.bStatic);
				pState->vList = ToInstances(stResponse.vList);
			}
			pState->iCode = iCode;
		}
		catch (...)
		{
			pState->iCode = OI_ERR_FAIL;
			::OI::Thread::ReportTaskError(std::current_exception());
		}
		pState->bDone.store(true, std::memory_order_release);
		Notify();
	});
	const int32_t iWaitStatus = WaitDone(pState->bDone);
	if (iWaitStatus != OI_OK)
	{
		LOG_TRACE("registry pull service=%s stage=wait code=%d", sNameLocal.c_str(), iWaitStatus);
		return iWaitStatus;
	}
	if (pState->iCode != OI_OK)
	{
		LOG_TRACE("registry pull service=%s stage=reply code=%d", sNameLocal.c_str(), pState->iCode);
		return pState->iCode;
	}
	ServiceTable::GetInstance().Store(sNameLocal, pState->vList);
	vList = std::move(pState->vList);
	return OI_OK;
}

int32_t RegistryClient::Lookup(const std::string& sNameLocal, std::string& sIpLocal, std::string& sPortLocal)
{
	const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("RegistryClient::Lookup");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}
	std::vector<ServiceInstance> vList;
	const int32_t iPullStatus = PullSync(sNameLocal, vList);
	if (iPullStatus != OI_OK)
	{
		return iPullStatus;
	}
	ServiceInstance stPick;
	if (!ServiceTable::Pick(vList, sNameLocal, 0, stPick))
	{
		LOG_TRACE("registry lookup %s miss", sNameLocal.c_str());
		return OI_ERR_NOT_FOUND;
	}
	sIpLocal = stPick.sIp;
	sPortLocal = stPick.sPort;
	LOG_TRACE("registry lookup %s -> %d %s:%s", sNameLocal.c_str(), stPick.iId, sIpLocal.c_str(), sPortLocal.c_str());
	return OI_OK;
}

bool RegistryClient::Snapshot(const std::string& sNameLocal, std::vector<ServiceInstance>& vList)
{
	if (ServiceTable::GetInstance().Snapshot(sNameLocal, vList))
	{
		return true;
	}
	if (Configured())
	{
		m_bPullWanted = true;
		Activate();
	}
	return false;
}

int32_t RegistryClient::Watch(::OI::Net::SessionManager* pManager, const std::string& sNameLocal)
{
	const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("RegistryClient::Watch");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}
	if (!pManager || sNameLocal.empty())
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	const int32_t iSourceStatus = EnsureSource();
	if (iSourceStatus != OI_OK)
	{
		return iSourceStatus;
	}
	if (m_eSource == Source::None)
	{
		return OI_OK;
	}
	const int32_t iStatus = ServiceTable::GetInstance().Watch(pManager, sNameLocal);
	if (iStatus == OI_OK)
	{
		m_bPullWanted = true;
		Activate();
	}
	return iStatus;
}

int32_t RegistryClient::WatchList(::OI::Net::SessionManager* pManager, const std::string& sNameLocal, const std::vector<ServiceInstance>& vInitial)
{
	const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("RegistryClient::WatchList");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}
	if (!pManager || sNameLocal.empty())
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	const int32_t iSourceStatus = EnsureSource();
	if (iSourceStatus != OI_OK)
	{
		return iSourceStatus;
	}
	if (m_eSource == Source::None)
	{
		return OI_OK;
	}
	const int32_t iStatus = ServiceTable::GetInstance().WatchList(pManager, sNameLocal, vInitial);
	if (iStatus == OI_OK)
	{
		m_bPullWanted = true;
		Activate();
	}
	return iStatus;
}

int32_t RegistryClient::Publish(const std::string& sSection, const std::string& sIpLocal, const std::string& sPortLocal)
{
	const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("RegistryClient::Publish");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}
	if (sSection == "Registry")
	{
		return OI_OK;
	}
	const int32_t iSourceStatus = EnsureSource();
	if (iSourceStatus != OI_OK)
	{
		return iSourceStatus;
	}
	if (m_eSource == Source::None)
	{
		return OI_OK;
	}
	auto& stConfig = ::OI::Base::Config::GlobalConfig();
	const std::string sManagerKey = sSection + ".service_name";
	const bool bLegacyName = !stConfig.Contains(sManagerKey) && stConfig.Contains("Service.name");
	std::string sNameLocal = sSection;
	if (stConfig.Contains(sManagerKey) || bLegacyName)
	{
		if (!stConfig.GetAs(bLegacyName ? "Service.name" : sManagerKey, sNameLocal))
		{
			return OI_ERR_INVALID_ARGUMENT;
		}
		if (sNameLocal.empty())
		{
			return OI_OK;
		}
	}
	else if (!stConfig.Contains("Service.id"))
	{
		return OI_OK;
	}
	std::string sZoneLocal;
	::OI::Base::Config::GlobalConfig().GetAs("Service.zone", sZoneLocal);
	auto fnToken = [](const std::string& sText)
	{
		if (sText.empty())
		{
			return false;
		}
		for (const auto& iCharacter : sText)
		{
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
		return OI_ERR_INVALID_ARGUMENT;
	}
	int32_t iIdLocal = 0;
	if (!::OI::Base::Config::GlobalConfig().GetAs("Service.id", iIdLocal) || iIdLocal <= 0)
	{
		fprintf(stderr, "registry publish %s: [Service] id 必须是正整数\n", sNameLocal.c_str());
		fflush(stderr);
		LOG_ERROR("registry publish %s invalid id", sNameLocal.c_str());
		return OI_ERR_INVALID_ARGUMENT;
	}
	std::string sAdvertiseIp;
	std::string sAdvertisePort;
	const bool bPublish = PublishAddress(sSection, sIpLocal, sPortLocal, bLegacyName, sAdvertiseIp, sAdvertisePort);
	if (!::OI::Net::NetAddr::ValidServicePort(sAdvertisePort))
	{
		fprintf(stderr, "registry publish invalid advertise_port for %s\n", sSection.c_str());
		return OI_ERR_INVALID_ARGUMENT;
	}
	std::string sExplicitIp;
	bool bExplicitIp = ::OI::Base::Config::GlobalConfig().GetAs(sSection + ".advertise_ip", sExplicitIp) && !sExplicitIp.empty();
	if (!bExplicitIp)
	{
		bExplicitIp = ::OI::Base::Config::GlobalConfig().GetAs("Service.advertise_ip", sExplicitIp) && !sExplicitIp.empty();
	}
	if (bExplicitIp && (sAdvertiseIp == "*" || ::OI::Net::NetAddr::IsUnspecifiedLiteral(sAdvertiseIp, sAdvertisePort)))
	{
		fprintf(stderr, "registry publish unreachable advertise_ip for %s\n", sSection.c_str());
		return OI_ERR_INVALID_ARGUMENT;
	}
	if (!bPublish)
	{
		LOG_TRACE("registry publish %s skip bind %s:%s", sNameLocal.c_str(), sIpLocal.c_str(), sPortLocal.c_str());
		return OI_OK;
	}
	if (m_eSource == Source::Registry)
	{
		RejectPollCallback("Publish");
	}
	::OI::Thread::MutexGuard stPublishLock(m_stPublishMutex);
	{
		::OI::Thread::MutexGuard stLock(m_stStateMutex);
		for (const auto& stPrevious : m_vPublications)
		{
			if (stPrevious.stService.sName != sNameLocal)
			{
				continue;
			}
			if ((stPrevious.bLegacyName && bLegacyName) ||
			    (stPrevious.sManager == sSection && stPrevious.stService.sIp == sAdvertiseIp && stPrevious.stService.sPort == sAdvertisePort))
			{
				return OI_OK;
			}
			fprintf(stderr, "registry duplicate service %s: managers %s and %s\n", sNameLocal.c_str(), stPrevious.sManager.c_str(), sSection.c_str());
			return OI_ERR_EXIST;
		}
	}
	Publication stPublication;
	stPublication.sManager = sSection;
	stPublication.bLegacyName = bLegacyName;
	auto& stService = stPublication.stService;
	stService.sName = sNameLocal;
	stService.iId = iIdLocal;
	stService.sIp = sAdvertiseIp;
	stService.sPort = sAdvertisePort;
	stService.sZone = sZoneLocal;
	if (m_eSource == Source::Local)
	{
		m_stStatic.CheckSelf(sNameLocal, iIdLocal, sAdvertiseIp, sAdvertisePort);
	}
	else
	{
		const int32_t iStatus = RegisterService(stPublication);
		if (iStatus != OI_OK)
		{
			return iStatus;
		}
	}
	{
		::OI::Thread::MutexGuard stLock(m_stStateMutex);
		m_vPublications.push_back(std::move(stPublication));
	}
	Activate();
	return OI_OK;
}

int32_t RegistryClient::RegisterService(Publication& stPublication)
{
	const auto& stService = stPublication.stService;
	int32_t iCodeLocal = Ensure();
	if (iCodeLocal == OI_OK && !m_bReportKnown.load(std::memory_order_acquire))
	{
		iCodeLocal = ProbeReport(stService.sName);
	}
	if (m_bReportKnown.load(std::memory_order_acquire) && !m_bReport.load())
	{
		LOG_TRACE("registry publish %s id=%d static, skip report", stService.sName.c_str(), stService.iId);
		return OI_OK;
	}
	struct State
	{
		std::atomic<bool> bDone{false};
		int32_t iCode = OI_ERR_FAIL;
	};
	auto pState = std::make_shared<State>();
	sid_t iPublishSessionId = INVALID_SID;
	if (iCodeLocal == OI_OK)
	{
		iPublishSessionId = m_iSessionId.load();
		::OI::RpcData::RegRegister stRequest(stService.sName, stService.iId, stService.sIp, stService.sPort, stService.sZone);
		CallRegistry<::OI::RpcData::RegRegister>(stRequest, kWaitMs, [this, pState](int32_t iCode, ::OI::RpcData::RegRegisterRes&)
		{
			pState->iCode = iCode;
			pState->bDone.store(true, std::memory_order_release);
			Notify();
		});
		iCodeLocal = WaitDone(pState->bDone);
		if (iCodeLocal == OI_OK)
		{
			iCodeLocal = pState->iCode;
		}
	}
	if (iCodeLocal == OI_ERR_EXIST)
	{
		fprintf(stderr, "registry publish %s id=%d 已经在线，拒绝启动\n", stService.sName.c_str(), stService.iId);
		fflush(stderr);
		LOG_ERROR("registry publish %s id=%d already online", stService.sName.c_str(), stService.iId);
		return OI_ERR_EXIST;
	}
	if (iCodeLocal == OI_OK)
	{
		stPublication.iRegisteredSession = iPublishSessionId;
		LOG_TRACE("registry publish %s id=%d %s:%s", stService.sName.c_str(), stService.iId, stService.sIp.c_str(), stService.sPort.c_str());
	}
	else
	{
		LOG_ERROR("registry publish %s id=%d not confirmed code=%d, retry on update", stService.sName.c_str(), stService.iId, iCodeLocal);
	}
	return OI_OK;
}

}
}
