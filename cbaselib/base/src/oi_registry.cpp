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
#include "oi_config.h"
#include "oi_log.h"
#include "oi_poll.h"
#include "oi_thread.h"
#include "oi_timer_manager.h"
#include <chrono>
#include <cstdio>

namespace OI
{
namespace Base
{

namespace
{
const int kWaitMs = 1000;

class RegistryClientSession : public ::OI::Net::NetSession
{
  public:
	explicit RegistryClientSession(::OI::Net::SessionManager* pManagerParam)
	    : ::OI::Net::NetSession(pManagerParam)
	{
	}

	virtual void OnDataOut()
	{
	}
	virtual void OnDataIn()
	{
		ReadRegFrames(stInputBuffer, this, [iSessionIdLocal = GetSessionID()](uint32_t iIdParam, ::OI::Base::BytesStream& stBodyParam)
		{
			RegistryClient::GetInstance().OnFrame(iSessionIdLocal, iIdParam, stBodyParam);
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
}

::OI::Base::Bytes EncodeRegFrame(uint32_t iIdParam, const ::OI::Rpc::Serialize& stMessageParam)
{
	::OI::Base::BytesStream stBody;
	stMessageParam.serialize(stBody);
	::OI::Base::BytesStream stFrame;
	uint32_t iSize = (uint32_t)stBody.Size();
	stFrame.WritePbRaw(iIdParam, iSize ? stBody.Begin() : NULL, iSize);
	return stFrame;
}

bool ReadRegFrames(::OI::Base::Bytes& stInputParam, ::OI::Net::Session* pSessionParam, const std::function<void(uint32_t, ::OI::Base::BytesStream&)>& fnOnFrameParam)
{
	::OI::Base::BytesStream stOutputStream;
	stOutputStream.Swap(stInputParam);
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
			pSessionParam->PassiveClose(PARSER_ERR_OTHER);
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
			fnOnFrameParam(iIdLocal, stBody);
		}
		catch (::OI::Base::ParserException&)
		{
			pSessionParam->PassiveClose(PARSER_ERR_PROTOCOL_SIZE);
			bClosedLocal = true;
		}
	}
	if (!bClosedLocal)
	{
		stInputParam.Swap(stOutputStream);
		stInputParam.Erase(0, stOutputStream.GetCurPos());
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

bool ServiceTable::Pick(const std::vector<ServiceInstance>& vListParam, const std::string& sNameParam, int32_t iCurrentParam, ServiceInstance& stOutputParam)
{
	const ServiceInstance* pBest = NULL;
	for (size_t iIndexLocal = 0; iIndexLocal < vListParam.size(); ++iIndexLocal)
	{
		if (vListParam[iIndexLocal].sName != sNameParam)
		{
			continue;
		}
		if (iCurrentParam > 0 && vListParam[iIndexLocal].iId == iCurrentParam)
		{
			stOutputParam = vListParam[iIndexLocal];
			return true;
		}
		if (!pBest || vListParam[iIndexLocal].iId < pBest->iId)
		{
			pBest = &vListParam[iIndexLocal];
		}
	}
	if (!pBest)
	{
		return false;
	}
	stOutputParam = *pBest;
	return true;
}

std::vector<::OI::Net::SessionManager*> ServiceTable::Watchers(const std::string& sNameParam)
{
	std::vector<::OI::Net::SessionManager*> vWatchers;
	::OI::Thread::MutexGuard stLock(m_stMutex);
	auto stIterator = m_mServices.find(sNameParam);
	if (stIterator == m_mServices.end())
	{
		return vWatchers;
	}
	vWatchers = stIterator->second.vWatchers;
	return vWatchers;
}

void ServiceTable::Watch(::OI::Net::SessionManager* pManagerParam, const std::string& sNameParam)
{
	::OI::Thread::MutexGuard stLock(m_stMutex);
	State& stState = m_mServices[sNameParam];
	for (size_t iIndexLocal = 0; iIndexLocal < stState.vWatchers.size(); ++iIndexLocal)
	{
		if (stState.vWatchers[iIndexLocal] == pManagerParam)
		{
			return;
		}
	}
	stState.vWatchers.push_back(pManagerParam);
}

std::vector<std::string> ServiceTable::Names()
{
	std::vector<std::string> vNamesLocal;
	::OI::Thread::MutexGuard stLock(m_stMutex);
	for (const auto& [sName, stState] : m_mServices)
	{
		if (!stState.vWatchers.empty())
		{
			vNamesLocal.push_back(sName);
		}
	}
	return vNamesLocal;
}

void ServiceTable::Apply(const std::vector<std::string>& vNamesParam, const std::vector<ServiceInstance>& vListParam)
{
	struct Change
	{
		::OI::Net::SessionManager* pManager;
		std::string sIp;
		std::string sPort;
		bool bOnline;
	};
	std::vector<Change> vChanged;
	{
		::OI::Thread::MutexGuard stLock(m_stMutex);
		for (size_t iNumber = 0; iNumber < vNamesParam.size(); ++iNumber)
		{
			auto stIterator = m_mServices.find(vNamesParam[iNumber]);
			if (stIterator == m_mServices.end())
			{
				continue;
			}
			State& stState = stIterator->second;
			ServiceInstance stPick;
			bool bOnlineLocal = Pick(vListParam, vNamesParam[iNumber], stState.bOnline ? stState.iId : 0, stPick);
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
		std::lock_guard<std::mutex> stLock(stMutex);
	}
	stConditionVariable.notify_all();
}

bool RegistryClient::WaitFlag(const std::atomic<bool>& bFlagParam)
{
	std::chrono::steady_clock::time_point stDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
#ifdef _SINGLE_THREAD_
	while (!bFlagParam.load() && !bFailed.load() && std::chrono::steady_clock::now() < stDeadline)
	{
		::OI::Net::Poll::GetInstance().PollWait(1);
	}
	return bFlagParam.load();
#else
	std::unique_lock<std::mutex> stLock(stMutex);
	stConditionVariable.wait_until(stLock, stDeadline, [&]()
	{
		return bFlagParam.load() || bFailed.load();
	});
	return bFlagParam.load();
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

bool RegistryClient::Send(uint32_t iIdParam, const ::OI::Rpc::Serialize& stMessageParam)
{
	sid_t iSessionIdLocal = iSessionId.load();
	if (iSessionIdLocal == INVALID_SID)
	{
		return false;
	}
	return SendBytes(iSessionIdLocal, EncodeRegFrame(iIdParam, stMessageParam));
}

void RegistryClient::SendRegister()
{
	::OI::RpcData::RegRegister stRequestLocal(sServiceName, iServiceId, sServiceIp, sServicePort, sServiceZone);
	Send(PROTO_REG_REGISTER, stRequestLocal);
}

void RegistryClient::OnAddSession(::OI::Net::Session* pSessionParam)
{
	::OI::Thread::MutexGuard stPendingLock(stPendingMutex);
	vPendingPulls.clear();
	iSessionId = pSessionParam->GetSessionID();
	bFailed = false;
	bConnected = true;
	bConnecting = false;
	Notify();
	if (bPublished.load() && !bLost.load())
	{
		SendRegister();
	}
	LOG_INFO("RegistryClient::OnAddSession sid=%d", pSessionParam->GetSessionID());
}

void RegistryClient::OnDelSession(const ::OI::Net::Session* pSessionParam)
{
	::OI::Thread::MutexGuard stPendingLock(stPendingMutex);
	if (iSessionId.load() != pSessionParam->GetSessionID())
	{
		return;
	}
	iSessionId = INVALID_SID;
	bConnected = false;
	bRegistered = false;
	bFailed = true;
	{
		::OI::Thread::MutexGuard stLock(stPendingMutex);
		vPendingPulls.clear();
	}
	Notify();
	LOG_INFO("RegistryClient::OnDelSession sid=%d", pSessionParam->GetSessionID());
}

void RegistryClient::OnConnectFailed()
{
	bConnecting = false;
	bFailed = true;
	Notify();
	LOG_TRACE("RegistryClient::OnConnectFailed");
}

void RegistryClient::OnFrame(sid_t iSessionIdParam, uint32_t iIdParam, ::OI::Base::BytesStream& stBodyParam)
{
	// 回包消费、同步结果发布与超时撤销共用同一把锁。
	::OI::Thread::MutexGuard stPendingLock(stPendingMutex);
	if (iSessionIdParam != iSessionId.load())
	{
		return;
	}
	if (iIdParam == PROTO_REG_REGISTER_RES)
	{
		::OI::RpcData::RegRegisterRes stResponseLocal;
		stResponseLocal.iCode = OI_ERR_FAIL;
		stResponseLocal.deserialize(stBodyParam);
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
	if (iIdParam == PROTO_REG_HEARTBEAT_RES)
	{
		::OI::RpcData::RegHeartbeatRes stResponseLocal;
		stResponseLocal.iCode = OI_ERR_FAIL;
		stResponseLocal.deserialize(stBodyParam);
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
	if (iIdParam == PROTO_REG_PULL_RES)
	{
		::OI::RpcData::RegPullRes stResponseLocal;
		stResponseLocal.iCode = OI_ERR_FAIL;
		stResponseLocal.deserialize(stBodyParam);
		PendingPull stPending;
		stPending.bSynchronous = false;
		{
			::OI::Thread::MutexGuard stLock(stPendingMutex);
			if (vPendingPulls.empty())
			{
				return;
			}
			stPending = vPendingPulls.front();
			vPendingPulls.pop_front();
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

bool RegistryClient::PullSync(const std::string& sNameParam, int32_t& iCodeParam, std::vector<ServiceInstance>& vListParam)
{
	::OI::Thread::MutexGuard stSyncLock(stSyncMutex);
	vListParam.clear();
	iCodeParam = OI_ERR_FAIL;
	if (sNameParam.empty() || !Configured())
	{
		return false;
	}
	if (!Ensure())
	{
		LOG_TRACE("registry lookup %s connect failed", sNameParam.c_str());
		return false;
	}
	PendingPull stPending;
	stPending.bSynchronous = true;
	stPending.vNames.push_back(sNameParam);
	::OI::RpcData::RegPull stRequestLocal(stPending.vNames);
	bool bSent = false;
	{
		// 排队顺序必须等于线上的发送顺序，不能在两步之间释放锁。
		::OI::Thread::MutexGuard stLock(stPendingMutex);
		bGot = false;
		iCode = OI_ERR_FAIL;
		vPendingPulls.push_back(stPending);
		try
		{
			bSent = Send(PROTO_REG_PULL, stRequestLocal);
		}
		catch (...)
		{
			vPendingPulls.pop_back();
			throw;
		}
		if (!bSent)
		{
			vPendingPulls.pop_back();
		}
	}
	if (!bSent || !WaitFlag(bGot))
	{
		// 同步调用已串行化。发送失败已回滚；超时仅取消等待，保留配对占位。
		::OI::Thread::MutexGuard stLock(stPendingMutex);
		for (auto stIterator = vPendingPulls.begin(); stIterator != vPendingPulls.end(); ++stIterator)
		{
			if (stIterator->bSynchronous)
			{
				stIterator->bSynchronous = false;
				break;
			}
		}
		LOG_TRACE("registry lookup %s %s", sNameParam.c_str(), bSent ? "timeout" : "send failed");
		return false;
	}
	::OI::Thread::MutexGuard stResultLock(stPendingMutex);
	iCodeParam = iCode;
	vListParam = vResponseList;
	return true;
}

bool RegistryClient::Lookup(const std::string& sNameParam, std::string& sIpParam, std::string& sPortParam)
{
	int32_t iCodeLocal = OI_ERR_FAIL;
	std::vector<ServiceInstance> vList;
	if (!PullSync(sNameParam, iCodeLocal, vList))
	{
		return false;
	}
	ServiceInstance stPick;
	if (iCodeLocal != OI_OK || !ServiceTable::Pick(vList, sNameParam, 0, stPick))
	{
		LOG_TRACE("registry lookup %s miss", sNameParam.c_str());
		return false;
	}
	sIpParam = stPick.sIp;
	sPortParam = stPick.sPort;
	LOG_TRACE("registry lookup %s -> %d %s:%s", sNameParam.c_str(), stPick.iId, sIpParam.c_str(), sPortParam.c_str());
	return true;
}

void RegistryClient::Watch(::OI::Net::SessionManager* pManagerParam, const std::string& sNameParam)
{
	if (!pManagerParam || sNameParam.empty() || !Configured())
	{
		return;
	}
	ServiceTable::GetInstance().Watch(pManagerParam, sNameParam);
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
	::OI::RpcData::RegPull stRequestLocal(vNamesLocal);
	::OI::Thread::MutexGuard stLock(stPendingMutex);
	if (!bConnected.load())
	{
		return;
	}
	vPendingPulls.push_back(stPending);
	try
	{
		if (!Send(PROTO_REG_PULL, stRequestLocal))
		{
			vPendingPulls.pop_back();
		}
	}
	catch (...)
	{
		vPendingPulls.pop_back();
		throw;
	}
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
bool TakeAdvertise(const std::string& sKeyParam, std::string& sValueParam)
{
	std::string sConfigured;
	if (!::OI::Base::Config::GlobalConfig().GetAs(sKeyParam, sConfigured) || sConfigured.empty())
	{
		return false;
	}
	sValueParam = sConfigured;
	return true;
}

bool PublishAddress(const std::string& sSectionParam, const std::string& sBindIpParam, const std::string& sBindPortParam, std::string& sIpParam, std::string& sPortParam)
{
	sIpParam = sBindIpParam;
	sPortParam = sBindPortParam;
	TakeAdvertise("Service.advertise_ip", sIpParam);
	TakeAdvertise("Service.advertise_port", sPortParam);
	TakeAdvertise(sSectionParam + ".advertise_ip", sIpParam);
	TakeAdvertise(sSectionParam + ".advertise_port", sPortParam);
	if (sIpParam.empty() || sPortParam.empty() || sIpParam == "0.0.0.0" || sIpParam == "*")
	{
		return false;
	}
	return true;
}
}

bool RegistryClient::Publish(const std::string& sSectionParam, const std::string& sIpParam, const std::string& sPortParam)
{
	::OI::Thread::MutexGuard stSyncLock(stSyncMutex);
	if (sSectionParam == "Registry" || !Configured())
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
	auto fnToken = [](const std::string& sTextParam)
	{
		if (sTextParam.empty())
		{
			return false;
		}
		for (size_t iIndexLocal = 0; iIndexLocal < sTextParam.size(); ++iIndexLocal)
		{
			char iCharacter = sTextParam[iIndexLocal];
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
	if (!PublishAddress(sSectionParam, sIpParam, sPortParam, sAdvertiseIp, sAdvertisePort))
	{
		LOG_TRACE("registry publish %s skip bind %s:%s", sNameLocal.c_str(), sIpParam.c_str(), sPortParam.c_str());
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
