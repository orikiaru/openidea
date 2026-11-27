#include "test_rpc_service.h"
#include "hold_test.h"
#include "oi_rpc_session_manager.h"
#include "rpc_probe.h"
#include "oi_http.h"
#include "oi_log_client.h"
#include "oi_log.h"
#include "oi_registry.h"
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include "oi_rpcdata_jsonall.h"
#include <limits>
#include "oi_config.h"
#include "oi_rpcdata_allechoreq.h"
#include "oi_rpcdata_allechores.h"
#include "oi_rpcdata_holdreq.h"
#include "oi_rpcdata_holdres.h"
#include "oi_rpcdata_peernotereq.h"
#include "oi_rpcdata_peerprobereq.h"
#include "oi_rpcdata_peerproberes.h"
#include "oi_thread.h"
#include "ipcclient.h"
#include "rpc_ids.h"
#include "oi_redis.h"
#include "oi_poll.h"
#include "oi_net_session.h"
#include "oi_timer_manager.h"
#include <uv.h>
#include "test_board.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

// 仅本回归持有的堆资源，不改变 Hold 的线上字段或协议号。
struct HoldRequestResource
{
	std::vector<char> vPayload = std::vector<char>(1024 * 1024, 'R');
	std::shared_ptr<std::atomic<int>> pDestroyed;
	explicit HoldRequestResource(std::shared_ptr<std::atomic<int>> pDestroyed)
	    : pDestroyed(std::move(pDestroyed))
	{
	}
	~HoldRequestResource()
	{
		pDestroyed->fetch_add(1, std::memory_order_relaxed);
	}
};

struct ResourceHoldRequest : OI::RpcData::HoldReq
{
	std::shared_ptr<HoldRequestResource> pResource;
	bool bThrow = false;
	OI::Base::BytesStream& serialize(OI::Base::BytesStream& stOutput) const override
	{
		if (!pResource || pResource->vPayload.size() != 1024 * 1024)
		{
			throw std::logic_error("request resource released before serialization");
		}
		if (bThrow)
		{
			throw std::runtime_error("request resource serialization failure");
		}
		return HoldReq::serialize(stOutput);
	}
};

namespace OI
{
namespace Rpc
{
// 复用生成的 Hold 配对，仅让测试附带一个不参与序列化的资源探针。
template <>
struct RpcProtoId<ResourceHoldRequest> : RpcProtoId<OI::RpcData::HoldReq>
{
};
}
}

template <typename T, typename Res>
OI::Coro::TaskResult<TestRpcService::NamedOutcome> TestRpcService::AwaitNamedHold(T stCall, const Res& stOutput)
{
	try
	{
		const int32_t iCode = co_await stCall;
		if (iCode == 0 && stOutput.iMark == 77)
		{
			co_return NamedOutcome::Success;
		}
		co_return NamedOutcome::Failed;
	}
	catch (const std::logic_error&)
	{
	}
	co_return NamedOutcome::Rejected;
}

OI::Coro::TaskResult<int> TestRpcService::CheckCompletedRpcRequests(sid_t iSessionId)
{
	int iBad = 0;
	auto& stRoute = OI::Rpc::RouteManager::GetInstance();
	auto& stManager = IpcClient::GetInstance();
	auto pDestroyed = std::make_shared<std::atomic<int>>(0);
	std::vector<OI::RpcData::HoldRes> vOutputs(20);
	std::vector<std::weak_ptr<HoldRequestResource>> vResources;
	std::vector<OI::Coro::TaskResult<int32_t>> vTasks;
	// 本连接、指定会话、客户端 Call、服务名、分区 1001、分区 1002，各测成功、业务失败、序列化异常，再测超时和选路失败。
	for (int iIndex = 0; iIndex < 20; ++iIndex)
	{
		ResourceHoldRequest stRequest;
		stRequest.pResource = std::make_shared<HoldRequestResource>(pDestroyed);
		vResources.emplace_back(stRequest.pResource);
		stRequest.iMark = 900 + iIndex;
		stRequest.iDelayMilliseconds = iIndex < 18 && iIndex % 3 == 1 ? -1 : 50;
		stRequest.bThrow = iIndex < 18 && iIndex % 3 == 2;
		vOutputs[iIndex].iMark = -900;
		switch (iIndex / 3)
		{
		case 0:
			vTasks.push_back(OI::Rpc::RpcCall(stManager, std::move(stRequest), vOutputs[iIndex], OI::Timer::Seconds(3)));
			break;
		case 1:
			vTasks.push_back(OI::Rpc::RpcCall(stManager, iSessionId, std::move(stRequest), vOutputs[iIndex], OI::Timer::Seconds(3)));
			break;
		case 2:
			vTasks.push_back(stManager.Call(std::move(stRequest), vOutputs[iIndex], OI::Timer::Seconds(3)));
			break;
		case 3:
			vTasks.push_back(stRoute.Call("gameserver", std::move(stRequest), vOutputs[iIndex], OI::Timer::Seconds(3)));
			break;
		case 4:
			vTasks.push_back(stRoute.Call("gameserver", 1001, std::move(stRequest), vOutputs[iIndex], OI::Timer::Seconds(3)));
			break;
		case 5:
			vTasks.push_back(stRoute.Call("gameserver", 1002, std::move(stRequest), vOutputs[iIndex], OI::Timer::Seconds(3)));
			break;
		default:
			if (iIndex == 18)
			{
				stRequest.iDelayMilliseconds = 700;
				vTasks.push_back(stManager.Call(std::move(stRequest), vOutputs[iIndex], OI::Timer::Milliseconds(100)));
			}
			else
			{
				vTasks.push_back(stRoute.Call("gameserver", 9999, std::move(stRequest), vOutputs[iIndex], OI::Timer::Seconds(3)));
			}
			break;
		}
	}
	// 保留全部 Task，重复读取保存的结果/异常；不能靠清空任务容器释放请求。
	for (int iPass = 0; iPass < 2; ++iPass)
	{
		for (int iIndex = 0; iIndex < 20; ++iIndex)
		{
			const bool bExpectedException = iIndex < 18 && iIndex % 3 == 2;
			int32_t iExpected = OI_OK;
			if (iIndex == 18)
			{
				iExpected = OI_ERR_TIMEOUT;
			}
			else if (iIndex == 19)
			{
				iExpected = OI_ERR_CONNECT;
			}
			else if (iIndex % 3 == 1)
			{
				iExpected = 1001;
			}
			bool bCaught = false;
			try
			{
				if (co_await vTasks[iIndex] != iExpected)
				{
					++iBad;
				}
			}
			catch (const std::runtime_error& stError)
			{
				bCaught = std::string(stError.what()) == "request resource serialization failure";
				if (!bCaught)
				{
					++iBad;
				}
			}
			catch (const std::exception&)
			{
				++iBad;
			}
			const int32_t iExpectedMark = !bExpectedException && iExpected == 0 ? 900 + iIndex : -900;
			if (bCaught != bExpectedException || vOutputs[iIndex].iMark != iExpectedMark || !vResources[iIndex].expired())
			{
				++iBad;
			}
		}
	}
	// 超时任务的迟到回包也不能重新使用已释放请求或覆盖保留的输出。
	co_await OI::Coro::yield(OI::Timer::Milliseconds(800));
	if (vOutputs[18].iMark != -900 || pDestroyed->load(std::memory_order_relaxed) != 20)
	{
		++iBad;
	}
	LOG_TRACE("hold completed rpc requests retained=%zu destroyed=%d bad=%d", vTasks.size(), pDestroyed->load(), iBad);
	fflush(stdout);
	co_return iBad;
}

OI::Coro::TaskResult<int> TestRpcService::CheckNamedOutputRpcTask()
{
	int iBad = 0;
	auto& stRoute = OI::Rpc::RouteManager::GetInstance();
	OI::RpcData::HoldReq stRequest;
	stRequest.iMark = 811;
	OI::RpcData::HoldRes stOutput;
	stOutput.iMark = -1;
	auto stAny = stRoute.Call("ipcserver", stRequest, stOutput, OI::Timer::Seconds(3));
	static_assert(std::is_same_v<decltype(stAny), OI::Coro::TaskResult<int32_t>>);
	if (co_await stAny != 0 || stOutput.iMark != 811)
	{
		++iBad;
	}
	stRequest.iMark = 812;
	auto stZone = stRoute.Call("gameserver", 1001, stRequest, stOutput, OI::Timer::Seconds(3));
	static_assert(std::is_same_v<decltype(stZone), OI::Coro::TaskResult<int32_t>>);
	if (co_await stZone != 0 || stOutput.iMark != 812)
	{
		++iBad;
	}
	// 服务名和请求只活在这个函数里，返回后任务必须自己保活。
	auto fnNamed = [pOutput = &stOutput]()
	{
		OI::RpcData::HoldReq stLocal;
		stLocal.iDelayMilliseconds = 100;
		stLocal.iMark = 813;
		std::string sName = "gameserver";
		auto stTask = OI::Rpc::RouteManager::GetInstance().Call(sName, 1002, stLocal, *pOutput, OI::Timer::Seconds(3));
		sName = "missing";
		stLocal.iMark = -813;
		return stTask;
	};
	auto stNamed = fnNamed();
	static_assert(std::is_same_v<decltype(stNamed), OI::Coro::TaskResult<int32_t>>);
	std::vector<OI::Coro::TaskResult<int>> vWaiters;
	for (int iIndex = 0; iIndex < 8; ++iIndex)
	{
		auto fnWaitForNamedCall = [stNamed]()
		{
			return RpcOutputWait::Await(stNamed);
		};
		auto stWaiter = OI::Coro::StartTask(std::move(fnWaitForNamedCall));
		vWaiters.push_back(std::move(stWaiter));
	}
	for (const auto& stWaiter : vWaiters)
	{
		iBad += co_await stWaiter;
	}
	if (stOutput.iMark != 813)
	{
		++iBad;
	}
	stOutput.iMark = 814;
	if (co_await stAny != 0 || co_await stZone != 0 || co_await stNamed != 0 || stOutput.iMark != 814)
	{
		++iBad;
	}
	stRequest.iDelayMilliseconds = -1;
	if (co_await stRoute.Call("gameserver", stRequest, stOutput, OI::Timer::Seconds(3)) != 1001 || stOutput.iMark != 814)
	{
		++iBad;
	}
	if (co_await stRoute.Call("gameserver", 9999, stRequest, stOutput, OI::Timer::Seconds(3)) != OI_ERR_CONNECT || stOutput.iMark != 814)
	{
		++iBad;
	}
	LOG_TRACE("hold output rpc named any zone shared=8 bad=%d", iBad);
	fflush(stdout);
	co_return iBad;
}

OI::Coro::TaskResult<int> TestRpcService::CheckNamedServiceRpc()
{
	int iBad = 0;
	auto& stRoute = OI::Rpc::RouteManager::GetInstance();
	::OI::RpcData::AllEchoReq stRequest;
	int iGlobal = 0;
	int iAny = 0;
	int iZone = 0;
	int iBatch = 0;
	for (int iTry = 0; iTry < 10; ++iTry)
	{
		::OI::RpcData::AllEchoRes stGlobalRes;
		::OI::RpcData::AllEchoRes stAnyRes;
		::OI::RpcData::AllEchoRes stZoneRes;
		const int32_t iGlobalCode = co_await stRoute.Call("ipcserver", stRequest, stGlobalRes, ::OI::Timer::Seconds(3));
		const int32_t iAnyCode = co_await stRoute.Call("gameserver", stRequest, stAnyRes, ::OI::Timer::Seconds(3));
		const int32_t iZoneCode = co_await stRoute.Call("gameserver", 1001, stRequest, stZoneRes, ::OI::Timer::Seconds(3));
		auto vBatch = co_await stRoute.CallAll("gameserver", stRequest, ::OI::Timer::Seconds(3));
		iGlobal = iGlobalCode == 0 && stGlobalRes.iCode == 1 ? 1 : 0;
		iAny = iAnyCode == 0 && stAnyRes.iCode == 1 ? 1 : 0;
		iZone = iZoneCode == 0 && stZoneRes.iCode == 1 ? 1 : 0;
		iBatch = 0;
		for (const auto& stItem : vBatch)
		{
			if (stItem.GetRet() == 0 && stItem.GetRes().iCode == 1)
			{
				++iBatch;
			}
		}
		if (iGlobal == 1 && iAny == 1 && iZone == 1 && iBatch == 2)
		{
			break;
		}
		co_await OI::Coro::yield(::OI::Timer::Milliseconds(200));
	}
	if (iGlobal != 1 || iAny != 1 || iZone != 1 || iBatch != 2)
	{
		++iBad;
	}
	::OI::RpcData::AllEchoRes stWrongRes;
	const int32_t iWrongCode = co_await stRoute.Call("ipcserver", 1001, stRequest, stWrongRes, ::OI::Timer::Seconds(3));
	int iWrong = iWrongCode == OI_ERR_FAIL ? 1 : 0;
	if (iWrong != 1)
	{
		++iBad;
	}
	::OI::RpcData::AllEchoRes stMissedRes;
	const int32_t iMissedCode = co_await stRoute.Call("gameserver", 9999, stRequest, stMissedRes, ::OI::Timer::Seconds(3));
	int iMiss = iMissedCode == OI_ERR_CONNECT ? 1 : 0;
	if (iMiss != 1)
	{
		++iBad;
	}
	std::vector<int32_t> vBothZones{1001, 1002};
	auto vMany = co_await stRoute.CallMany("gameserver", vBothZones, stRequest, ::OI::Timer::Seconds(3));
	int iMany = 0;
	if (vMany.size() == vBothZones.size())
	{
		for (const auto& stItem : vMany)
		{
			if (stItem.GetRet() == 0 && stItem.GetRes().iCode == 1)
			{
				++iMany;
			}
		}
	}
	std::vector<int32_t> vPartialZones{1001, 9999};
	auto vPartial = co_await stRoute.CallMany<::OI::RpcData::AllEchoRes>("gameserver", vPartialZones, stRequest, ::OI::Timer::Seconds(3));
	int iSlot = vPartial.size() == 2 && vPartial[0].GetRet() == 0 && vPartial[0].GetRes().iCode == 1 && vPartial[1].GetRet() == OI_ERR_CONNECT ? 1 : 0;
	auto vInferred = co_await stRoute.CallMany("gameserver", vPartialZones, stRequest, ::OI::Timer::Seconds(3));
	static_assert(std::is_same_v<decltype(vPartial), decltype(vInferred)>);
	if (vInferred.size() != 2 || vInferred[0].GetRet() != 0 || vInferred[0].GetRes().iCode != 1 || vInferred[1].GetRet() != OI_ERR_CONNECT)
	{
		iSlot = 0;
	}
	auto vEmptyMany = co_await stRoute.CallMany<::OI::RpcData::AllEchoRes>("gameserver", std::vector<int32_t>{}, stRequest, ::OI::Timer::Seconds(3));
	auto vGlobalMany = co_await stRoute.CallMany<::OI::RpcData::AllEchoRes>("ipcserver", vBothZones, stRequest, ::OI::Timer::Seconds(3));
	if (iMany != 2 || iSlot != 1 || !vEmptyMany.empty() || vGlobalMany.size() != 2 ||
	    vGlobalMany[0].GetRet() != OI_ERR_FAIL || vGlobalMany[1].GetRet() != OI_ERR_FAIL)
	{
		++iBad;
	}
	::OI::RpcData::HoldReq stHold;
	stHold.iDelayMilliseconds = 400;
	stHold.iMark = 77;
	int64_t iBegin = ::OI::Timer::SteadyMilliseconds();
	auto vHeld = co_await stRoute.CallAll<::OI::RpcData::HoldRes>("gameserver", stHold, ::OI::Timer::Seconds(3));
	long long iElapsed = ::OI::Timer::SteadyMilliseconds() - iBegin;
	int iHeldOk = 0;
	for (const auto& stItem : vHeld)
	{
		if (stItem.GetRet() == 0 && stItem.GetRes().iMark == 77)
		{
			++iHeldOk;
		}
	}
	if (iHeldOk != 2 || iElapsed >= 700)
	{
		++iBad;
	}
	LOG_TRACE("hold service rpc global=%d any=%d zone=%d batch=%d many=%d slot=%d elapsed_ms=%lld wrong=%d miss=%d bad=%d", iGlobal, iAny, iZone, iBatch, iMany, iSlot, iElapsed, iWrong, iMiss, iBad);
	// 服务名只在入口拷进任务；局部字符串和请求销毁后，延迟启动仍能正确发送。
	auto fnDeferred = []()
	{
		::OI::RpcData::HoldReq stLocal;
		stLocal.iMark = 913;
		std::string sName = "gameserver";
		auto stTask = OI::Rpc::RouteManager::GetInstance().CallAll<::OI::RpcData::HoldRes>(sName, stLocal, OI::Timer::Seconds(3));
		sName = "invalid";
		return stTask;
	};
	auto stDeferred = fnDeferred();
	auto fnDeferredMany = []()
	{
		::OI::RpcData::HoldReq stLocal;
		stLocal.iMark = 914;
		return OI::Rpc::RouteManager::GetInstance().CallMany<OI::RpcData::HoldRes>("gameserver", {1001, 1002}, stLocal, OI::Timer::Seconds(3));
	};
	auto stDeferredMany = fnDeferredMany();
	co_await OI::Coro::yield(0);
	auto vDeferred = co_await stDeferred;
	if (vDeferred.size() != 2)
	{
		++iBad;
	}
	for (const auto& stItem : vDeferred)
	{
		if (stItem.GetRet() != 0 || stItem.GetRes().iMark != 913)
		{
			++iBad;
		}
	}
	auto vDeferredMany = co_await stDeferredMany;
	if (vDeferredMany.size() != 2)
	{
		++iBad;
	}
	for (const auto& stItem : vDeferredMany)
	{
		if (stItem.GetRet() != 0 || stItem.GetRes().iMark != 914)
		{
			++iBad;
		}
	}
	::OI::RpcData::HoldRes stShared;
	stShared.iMark = -77;
	auto stSingle = stRoute.Call("gameserver", 1001, stHold, stShared, OI::Timer::Seconds(3));
	std::vector<OI::Coro::TaskResult<NamedOutcome>> vContenders;
	for (int iIndex = 0; iIndex < 8; ++iIndex)
	{
		vContenders.push_back(AwaitNamedHold(stSingle, stShared));
	}
	int iSuccess = 0, iRejected = 0;
	for (const auto& stContender : vContenders)
	{
		const NamedOutcome iOutcome = co_await stContender;
		iSuccess += iOutcome == NamedOutcome::Success ? 1 : 0;
		iRejected += iOutcome == NamedOutcome::Rejected ? 1 : 0;
	}
	const int32_t iAgain = co_await stSingle;
	if (iAgain != 0 || stShared.iMark != 77)
	{
		++iBad;
	}
	if (iSuccess != 8 || iRejected != 0)
	{
		++iBad;
	}
	LOG_TRACE("hold named rpc ownership success=%d rejected=%d bad=%d", iSuccess, iRejected, iBad);
	fflush(stdout);
	co_return iBad;
}

void TestRpcService::OnTaskError(OI::Coro::Token, std::exception_ptr stError)
{
	try
	{
		std::rethrow_exception(stError);
	}
	catch (const std::exception& stException)
	{
		TestRpcService::GetInstance().RememberTaskError(stException.what());
		fprintf(stderr, "task failed: %s\n", stException.what());
		fflush(stderr);
	}
	catch (...)
	{
		fprintf(stderr, "task failed: unknown exception\n");
		fflush(stderr);
	}
}

void TestRpcService::RememberTaskError(const std::string& sText)
{
	::OI::Thread::MutexGuard stLock(m_stErrorLocker);
	m_vTaskErrors.push_back(sText);
}

bool TestRpcService::SawTaskError(const std::string& sNeedle)
{
	::OI::Thread::MutexGuard stLock(m_stErrorLocker);
	for (const auto& sText : m_vTaskErrors)
	{
		if (sText.find(sNeedle) != std::string::npos)
		{
			return true;
		}
	}
	return false;
}

OI::Coro::TaskResult<int32_t> TestRpcService::QueryZoneNotes(int32_t iZoneId, int32_t iTag)
{
	::OI::RpcData::PeerProbeReq stRequest;
	stRequest.iTag = iTag;
	stRequest.iNotify = 0;
	::OI::RpcData::PeerProbeRes stResponse;
	stResponse.iNoteCount = -1;
	const int32_t iCode = co_await OI::Rpc::RouteManager::GetInstance().Call("gameserver", iZoneId, stRequest, stResponse, ::OI::Timer::Seconds(3));
	if (iCode != OI_OK)
	{
		co_return iCode;
	}
	co_return stResponse.iNoteCount;
}

OI::Coro::TaskResult<int32_t> TestRpcService::WaitGameserverNotes(int32_t iTag, int32_t iExpected, int64_t iMilliseconds, int32_t& iZone1001, int32_t& iZone1002)
{
	const int64_t iDeadlineMilliseconds = ::OI::Timer::SteadyMilliseconds() + iMilliseconds;
	while (true)
	{
		iZone1001 = co_await QueryZoneNotes(1001, iTag);
		iZone1002 = co_await QueryZoneNotes(1002, iTag);
		int32_t iSum = -1;
		if (iZone1001 >= 0 && iZone1002 >= 0)
		{
			iSum = iZone1001 + iZone1002;
		}
		if (iSum == iExpected)
		{
			co_return iSum;
		}
		if (::OI::Timer::SteadyMilliseconds() >= iDeadlineMilliseconds)
		{
			if (iZone1001 < 0)
			{
				co_return iZone1001;
			}
			if (iZone1002 < 0)
			{
				co_return iZone1002;
			}
			co_return iSum;
		}
		co_await OI::Coro::yield(20);
	}
}

OI::Coro::TaskResult<bool> TestRpcService::WaitTaskError(const std::string& sNeedle, int64_t iMilliseconds)
{
	const int64_t iDeadlineMilliseconds = ::OI::Timer::SteadyMilliseconds() + iMilliseconds;
	while (!SawTaskError(sNeedle))
	{
		if (::OI::Timer::SteadyMilliseconds() >= iDeadlineMilliseconds)
		{
			co_return false;
		}
		co_await OI::Coro::yield(20);
	}
	co_return true;
}

OI::Coro::TaskResult<int> TestRpcService::CheckRouteNotify()
{
	int iBad = 0;
	OI::Coro::SetTaskErrorHandler(&TestRpcService::OnTaskError);
	auto& stRoute = OI::Rpc::RouteManager::GetInstance();
	constexpr int32_t iSingleTag = 7101;
	constexpr int32_t iZoneTag = 7102;
	constexpr int32_t iNegativeTag = 7103;
	constexpr int32_t iAllTag = 7104;
	constexpr int32_t iManyTag = 7105;
	constexpr int32_t iMissingTag = 7106;
	constexpr int32_t iReconnectTag = 7107;
	constexpr int64_t iWaitMilliseconds = 5000;

	int32_t iSingle1001 = -1;
	int32_t iSingle1002 = -1;
	::OI::RpcData::PeerNoteReq stSingle;
	stSingle.iTag = iSingleTag;
	const int32_t iSingleCode = stRoute.Notify("gameserver", stSingle);
	const int32_t iSingle = co_await WaitGameserverNotes(iSingleTag, 1, iWaitMilliseconds, iSingle1001, iSingle1002);
	if (iSingleCode != OI_OK || iSingle != 1)
	{
		++iBad;
	}

	int32_t iZone1001 = -1;
	int32_t iZone1002 = -1;
	::OI::RpcData::PeerNoteReq stZone;
	stZone.iTag = iZoneTag;
	const int32_t iZoneCode = stRoute.Notify("gameserver", 1001, stZone);
	iZone1001 = co_await QueryZoneNotes(1001, iZoneTag);
	const int64_t iZoneDeadline = ::OI::Timer::SteadyMilliseconds() + iWaitMilliseconds;
	while (iZone1001 != 1 && ::OI::Timer::SteadyMilliseconds() < iZoneDeadline)
	{
		co_await OI::Coro::yield(20);
		iZone1001 = co_await QueryZoneNotes(1001, iZoneTag);
	}
	iZone1002 = co_await QueryZoneNotes(1002, iZoneTag);
	if (iZoneCode != OI_OK || iZone1001 != 1 || iZone1002 != 0)
	{
		++iBad;
	}

	::OI::RpcData::PeerNoteReq stNegative;
	stNegative.iTag = iNegativeTag;
	const int32_t iNegative = stRoute.Notify("gameserver", -1, stNegative);
	const int32_t iNegative1001 = co_await QueryZoneNotes(1001, iNegativeTag);
	const int32_t iNegative1002 = co_await QueryZoneNotes(1002, iNegativeTag);
	if (iNegative != OI_ERR_FAIL || iNegative1001 != 0 || iNegative1002 != 0)
	{
		++iBad;
	}

	int32_t iAll1001 = -1;
	int32_t iAll1002 = -1;
	::OI::RpcData::PeerNoteReq stAll;
	stAll.iTag = iAllTag;
	const int32_t iAllCode = stRoute.NotifyAll("gameserver", stAll);
	const int32_t iAll = co_await WaitGameserverNotes(iAllTag, 2, iWaitMilliseconds, iAll1001, iAll1002);
	if (iAllCode != OI_OK || iAll != 2 || iAll1001 != 1 || iAll1002 != 1)
	{
		++iBad;
	}

	int32_t iMany1001 = -1;
	int32_t iMany1002 = -1;
	::OI::RpcData::PeerNoteReq stMany;
	stMany.iTag = iManyTag;
	const std::vector<int32_t> vZones{1001, 1003, 1002};
	const int32_t iManyCode = stRoute.NotifyMany("gameserver", vZones, stMany);
	const int32_t iMany = co_await WaitGameserverNotes(iManyTag, 2, iWaitMilliseconds, iMany1001, iMany1002);
	const bool bMissingReported = co_await WaitTaskError("zones=1003 protocol=3006 failed=1/3 code=-3", iWaitMilliseconds);
	if (iManyCode != OI_OK || iMany != 2 || iMany1001 != 1 || iMany1002 != 1 || !bMissingReported)
	{
		++iBad;
	}

	::OI::RpcData::PeerNoteReq stMissing;
	stMissing.iTag = iMissingTag;
	const int32_t iAbsentCode = stRoute.Notify("missingnotify", stMissing);
	const bool bAbsentReported = co_await WaitTaskError("service=missingnotify protocol=3006 failed=1/1 code=-3", iWaitMilliseconds);
	const int32_t iAbsent1001 = co_await QueryZoneNotes(1001, iMissingTag);
	const int32_t iAbsent1002 = co_await QueryZoneNotes(1002, iMissingTag);
	if (iAbsentCode != OI_OK || !bAbsentReported || iAbsent1001 != 0 || iAbsent1002 != 0)
	{
		++iBad;
	}

	OI::Rpc::RouteAwaiter stWait;
	stWait.sName = "gameserver";
	stWait.iPick = OI::Rpc::ServicePick::Zone;
	stWait.sZone = "1001";
	auto stResolved = co_await stWait;
	int32_t iReconnect1001 = -1;
	int32_t iReconnectCode = OI_ERR_FAIL;
	if (stResolved.iStatus != OI::Rpc::ServiceRouteStatus::Ok || stResolved.vClients.size() != 1 || !stResolved.vClients[0])
	{
		++iBad;
	}
	else
	{
		auto* pClient = stResolved.vClients[0];
		const sid_t iSid = stRoute.ConnectionInfo(*pClient).iSessionId;
		if (iSid == INVALID_SID)
		{
			++iBad;
		}
		::OI::RpcData::PeerNoteReq stReconnect;
		stReconnect.iTag = iReconnectTag;
		pClient->Manager().CloseSession(iSid);
		iReconnectCode = stRoute.Notify("gameserver", 1001, stReconnect);
		const int64_t iReconnectDeadline = ::OI::Timer::SteadyMilliseconds() + iWaitMilliseconds;
		iReconnect1001 = co_await QueryZoneNotes(1001, iReconnectTag);
		while (iReconnect1001 != 1 && ::OI::Timer::SteadyMilliseconds() < iReconnectDeadline)
		{
			co_await OI::Coro::yield(20);
			iReconnect1001 = co_await QueryZoneNotes(1001, iReconnectTag);
		}
		if (iReconnectCode != OI_OK || iReconnect1001 != 1)
		{
			++iBad;
		}
	}

	if (iBad != 0)
	{
		std::vector<std::string> vErrors;
		{
			::OI::Thread::MutexGuard stLock(m_stErrorLocker);
			vErrors = m_vTaskErrors;
		}
		for (const auto& sText : vErrors)
		{
			LOG_TRACE("hold route notify task error %s", sText.c_str());
		}
		LOG_TRACE("hold route notify detail single=%d/%d zone=%d/%d negative=%d/%d/%d all=%d/%d many=%d/%d absent=%d/%d/%d reconnect=%d",
		    iSingle1001, iSingle1002, iZone1001, iZone1002, iNegative, iNegative1001, iNegative1002, iAll1001, iAll1002, iMany1001, iMany1002,
		    iAbsentCode, iAbsent1001, iAbsent1002, iReconnect1001);
	}
	LOG_TRACE("hold route notify single=%d zone=%d zone_other=%d negative=%d all=%d many=%d missing_reported=%d absent_reported=%d reconnect=%d bad=%d",
	    iSingle, iZone1001, iZone1002, iNegative, iAll, iMany, bMissingReported ? 1 : 0, bAbsentReported ? 1 : 0, iReconnect1001 == 1 ? 1 : 0, iBad);
	fflush(stdout);
	co_return iBad;
}

// 全局、任意、分区、批量和多分区调用打到对应服务。并发争用同一个命名调用时，成功和拒绝的次数对得上。
OI::Coro::TaskResult<int> TestRpcService::RunNamed()
{
	int iBad = co_await CheckNamedServiceRpc();
	iBad += co_await CheckRouteNotify();
	co_return iBad;
}

// 按服务名和分区的输出引用，8 个等待者只提交一次。
OI::Coro::TaskResult<int> TestRpcService::RunOutput()
{
	co_return co_await CheckNamedOutputRpcTask();
}

// 保留已完成任务再等待时，请求附带的资源全部释放。
OI::Coro::TaskResult<int> TestRpcService::RunCompleted(sid_t iSessionId)
{
	co_return co_await CheckCompletedRpcRequests(iSessionId);
}
