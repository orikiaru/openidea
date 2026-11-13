#include "rpc_service_mgr.h"
#include "hold_test.h"
#include "oi_rpc_coro.h"
#include "rpc_probe.h"
#include "zone_service.h"
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

template <typename T>
OI::Coro::TaskResult<RpcServiceMgr::NamedOutcome> RpcServiceMgr::AwaitNamedHold(T stCall)
{
	try
	{
		auto stReply = co_await stCall;
		co_return stReply.GetRet() == 0 && stReply.GetRes().iMark == 77 ? NamedOutcome::Success : NamedOutcome::Failed;
	}
	catch (const std::logic_error&)
	{
	}
	co_return NamedOutcome::Rejected;
}

OI::Coro::TaskResult<int> RpcServiceMgr::CheckCompletedRpcRequests(sid_t iSessionId)
{
	int iBad = 0;
	static ZoneService stGames("gameserver");
	auto& stManager = IpcClient::GetInstance();
	auto pDestroyed = std::make_shared<std::atomic<int>>(0);
	std::vector<OI::RpcData::HoldRes> vOutputs(20);
	std::vector<std::weak_ptr<HoldRequestResource>> vResources;
	std::vector<OI::Coro::TaskResult<int32_t>> vTasks;
	// 六个公开输出入口各测成功、业务失败、序列化异常，再测超时和选路失败。
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
			vTasks.push_back(stGames.CallAny(std::move(stRequest), vOutputs[iIndex], OI::Timer::Seconds(3)));
			break;
		case 4:
			vTasks.push_back(stGames.Call(1001, std::move(stRequest), vOutputs[iIndex], OI::Timer::Seconds(3)));
			break;
		case 5:
			vTasks.push_back(OI::Rpc::RpcCall(std::string("gameserver.1002"), std::move(stRequest), vOutputs[iIndex], OI::Timer::Seconds(3)));
			break;
		default:
			if (iIndex == 18)
			{
				stRequest.iDelayMilliseconds = 700;
				vTasks.push_back(stManager.Call(std::move(stRequest), vOutputs[iIndex], OI::Timer::Milliseconds(100)));
			}
			else
			{
				vTasks.push_back(stGames.Call(9999, std::move(stRequest), vOutputs[iIndex], OI::Timer::Seconds(3)));
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
			const int32_t iExpected = iIndex == 18 ? OI_ERR_TIMEOUT : (iIndex == 19 ? OI_ERR_CONNECT : (iIndex % 3 == 1 ? 1001 : 0));
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

OI::Coro::TaskResult<int> RpcServiceMgr::CheckNamedOutputRpcTask()
{
	int iBad = 0;
	static ZoneService stGames("gameserver");
	auto& stManager = IpcClient::GetInstance();
	OI::RpcData::HoldReq stRequest;
	stRequest.iMark = 811;
	OI::RpcData::HoldRes stOutput;
	stOutput.iMark = -1;
	auto stAny = stManager.CallAny(stRequest, stOutput, OI::Timer::Seconds(3));
	static_assert(std::is_same_v<decltype(stAny), OI::Coro::TaskResult<int32_t>>);
	if (co_await stAny != 0 || stOutput.iMark != 811)
	{
		++iBad;
	}
	stRequest.iMark = 812;
	auto stZone = stGames.Call(1001, stRequest, stOutput, OI::Timer::Seconds(3));
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
		std::string sName = "gameserver.1002";
		auto stTask = OI::Rpc::RpcCall(sName, stLocal, *pOutput, OI::Timer::Seconds(3));
		sName = "missing.9999";
		stLocal.iMark = -813;
		return stTask;
	};
	auto stNamed = fnNamed();
	static_assert(std::is_same_v<decltype(stNamed), OI::Coro::TaskResult<int32_t>>);
	std::vector<OI::Coro::TaskResult<int>> vWaiters;
	for (int iIndex = 0; iIndex < 8; ++iIndex)
	{
		vWaiters.push_back(OI::Coro::StartTask([stNamed](OI::Coro::Token)
		{
			return RpcOutputWait::Await(stNamed);
		}));
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
	if (co_await stGames.CallAny(stRequest, stOutput, OI::Timer::Seconds(3)) != 1001 || stOutput.iMark != 814)
	{
		++iBad;
	}
	if (co_await stGames.Call(9999, stRequest, stOutput, OI::Timer::Seconds(3)) != OI_ERR_CONNECT || stOutput.iMark != 814)
	{
		++iBad;
	}
	if (co_await OI::Rpc::RpcCall(std::string("gameserver.*"), stRequest, stOutput, OI::Timer::Seconds(3)) != OI_ERR_FAIL || stOutput.iMark != 814)
	{
		++iBad;
	}
	LOG_TRACE("hold output rpc named any zone shared=8 bad=%d", iBad);
	fflush(stdout);
	co_return iBad;
}

OI::Coro::TaskResult<int> RpcServiceMgr::CheckNamedServiceRpc()
{
	int iBad = 0;
	static ZoneService stGames("gameserver");
	auto& stIpc = IpcClient::GetInstance();
	::OI::RpcData::AllEchoReq stRequest;
	int iGlobal = 0;
	int iAny = 0;
	int iZone = 0;
	int iBatch = 0;
	for (int iTry = 0; iTry < 10; ++iTry)
	{
		auto stGlobal = co_await stIpc.CallAny<::OI::RpcData::AllEchoRes>(stRequest, ::OI::Timer::Seconds(3));
		auto stAny = co_await stGames.CallAny<::OI::RpcData::AllEchoRes>(stRequest, ::OI::Timer::Seconds(3));
		auto stZone = co_await stGames.Call<::OI::RpcData::AllEchoRes>(1001, stRequest, ::OI::Timer::Seconds(3));
		auto vBatch = co_await stGames.CallAll<::OI::RpcData::AllEchoRes>(stRequest, ::OI::Timer::Seconds(3));
		iGlobal = stGlobal.GetRet() == 0 && stGlobal.GetRes().iCode == 1 ? 1 : 0;
		iAny = stAny.GetRet() == 0 && stAny.GetRes().iCode == 1 ? 1 : 0;
		iZone = stZone.GetRet() == 0 && stZone.GetRes().iCode == 1 ? 1 : 0;
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
	auto stWrongZone = co_await stIpc.Call<::OI::RpcData::AllEchoRes>(1001, stRequest, ::OI::Timer::Seconds(3));
	int iWrong = stWrongZone.GetRet() == OI_ERR_FAIL ? 1 : 0;
	if (iWrong != 1)
	{
		++iBad;
	}
	auto stMissed = co_await stGames.Call<::OI::RpcData::AllEchoRes>(9999, stRequest, ::OI::Timer::Seconds(3));
	int iMiss = stMissed.GetRet() == OI_ERR_CONNECT ? 1 : 0;
	if (iMiss != 1)
	{
		++iBad;
	}
	std::vector<int32_t> vBothZones{1001, 1002};
	auto vMany = co_await stGames.CallMany<::OI::RpcData::AllEchoRes>(vBothZones, stRequest, ::OI::Timer::Seconds(3));
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
	auto vPartial = co_await stGames.CallMany<::OI::RpcData::AllEchoRes>(vPartialZones, stRequest, ::OI::Timer::Seconds(3));
	int iSlot = vPartial.size() == 2 && vPartial[0].GetRet() == 0 && vPartial[0].GetRes().iCode == 1 && vPartial[1].GetRet() == OI_ERR_CONNECT ? 1 : 0;
	auto vEmptyMany = co_await stGames.CallMany<::OI::RpcData::AllEchoRes>(std::vector<int32_t>{}, stRequest, ::OI::Timer::Seconds(3));
	auto vGlobalMany = co_await stIpc.CallMany<::OI::RpcData::AllEchoRes>(vBothZones, stRequest, ::OI::Timer::Seconds(3));
	if (iMany != 2 || iSlot != 1 || !vEmptyMany.empty() || vGlobalMany.size() != 2 ||
	    vGlobalMany[0].GetRet() != OI_ERR_FAIL || vGlobalMany[1].GetRet() != OI_ERR_FAIL)
	{
		++iBad;
	}
	::OI::RpcData::HoldReq stHold;
	stHold.iDelayMilliseconds = 400;
	stHold.iMark = 77;
	int64_t iBegin = ::OI::Timer::SteadyMilliseconds();
	auto vHeld = co_await stGames.CallAll<::OI::RpcData::HoldRes>(stHold, ::OI::Timer::Seconds(3));
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
	// 服务名只在入口解析；局部字符串和请求销毁后，延迟启动仍能正确发送。
	static ZoneService stOfflineWatcher("ipcserver");
	OI::Base::ServiceTable::GetInstance().Watch(&stOfflineWatcher, "ipcserver");
	OI::Rpc::RouteAwaiter stFallbackWait;
	stFallbackWait.pManager = &stOfflineWatcher;
	stFallbackWait.sName = "ipcserver";
	auto stFallback = co_await stFallbackWait;
	if (stFallback.iStatus != OI::Rpc::ServiceRouteStatus::Ok || stFallback.vClients.size() != 1 || stFallback.vClients[0] == &stOfflineWatcher.Sender())
	{
		++iBad;
	}
	auto fnDeferred = []()
	{
		::OI::RpcData::HoldReq stLocal;
		stLocal.iMark = 913;
		std::string sTarget = "gameserver.*";
		auto stTask = OI::Rpc::RpcCallBatch<OI::RpcData::HoldRes>(sTarget, stLocal, OI::Timer::Seconds(3));
		sTarget = "invalid.*";
		return stTask;
	};
	auto stDeferred = fnDeferred();
	auto fnDeferredMany = []()
	{
		::OI::RpcData::HoldReq stLocal;
		stLocal.iMark = 914;
		return stGames.CallMany<OI::RpcData::HoldRes>({1001, 1002}, stLocal, OI::Timer::Seconds(3));
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
	for (const auto& stItem : co_await stDeferredMany)
	{
		if (stItem.GetRet() != 0 || stItem.GetRes().iMark != 914)
		{
			++iBad;
		}
	}
	auto stSingle = stGames.Call<OI::RpcData::HoldRes>(1001, stHold, OI::Timer::Seconds(3));
	std::vector<OI::Coro::TaskResult<NamedOutcome>> vContenders;
	for (int iIndex = 0; iIndex < 8; ++iIndex)
	{
		vContenders.push_back(AwaitNamedHold(stSingle));
	}
	int iSuccess = 0, iRejected = 0;
	for (const auto& stContender : vContenders)
	{
		const NamedOutcome iOutcome = co_await stContender;
		iSuccess += iOutcome == NamedOutcome::Success ? 1 : 0;
		iRejected += iOutcome == NamedOutcome::Rejected ? 1 : 0;
	}
	auto stAgain = co_await stSingle;
	if (stAgain.GetRet() != 0 || stAgain.GetRes().iMark != 77)
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

// 全局、任意、分区、批量和多分区调用打到对应服务。并发争用同一个命名调用时，成功和拒绝的次数对得上。
OI::Coro::TaskResult<int> RpcServiceMgr::RunNamed()
{
	co_return co_await CheckNamedServiceRpc();
}

// 按服务名和分区的输出引用，8 个等待者只提交一次。
OI::Coro::TaskResult<int> RpcServiceMgr::RunOutput()
{
	co_return co_await CheckNamedOutputRpcTask();
}

// 保留已完成任务再等待时，请求附带的资源全部释放。
OI::Coro::TaskResult<int> RpcServiceMgr::RunCompleted(sid_t iSessionId)
{
	co_return co_await CheckCompletedRpcRequests(iSessionId);
}
