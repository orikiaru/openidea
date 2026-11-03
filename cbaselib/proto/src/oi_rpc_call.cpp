#include "oi_rpc_call.h"
#include "oi_timer_manager.h"
#include "oi_poll.h"
#include "oi_rpc_coro.h"
#include "oi_thread.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>
namespace OI
{
namespace Rpc
{

static bool DrainStCoro()
{
	return coro::StScheduler::Poll();
}

static bool PendingStCoro()
{
	return coro::StScheduler::Queued();
}

static struct StDrainReg
{
	StDrainReg()
	{
		::OI::Thread::ThreadPool::GetInstance().SetDrain(DrainStCoro);
		::OI::Thread::ThreadPool::GetInstance().SetPending(PendingStCoro);
		::OI::Thread::ThreadPool::GetInstance().SetAfterResume(coro::DrainDetached);
	}
} g_st_drain_reg;

namespace
{
typedef std::function<coro::Task(coro::Token, ::OI::Net::SessionManager*, sid_t, ::OI::Base::BytesStream)> RpcHandleFn;

struct RpcExpire
{
	int64_t iDeadlineMilliseconds;
	uint64_t iCallId;
};
using RpcExpireBuckets = std::map<int64_t, std::list<RpcExpire>>;

struct RpcCoroSlot
{
	::OI::Net::SessionManager* pManager;
	sid_t iSessionId;
	std::coroutine_handle<> stHandle;
	int32_t* pResult;
	std::function<bool(::OI::Base::BytesStream&)> fnDecode;
	std::function<void()> fnResume;
	proto_type_t iResponseId;
	uint64_t iCallId;
	bool bAwaitingConnect;
	bool bHasExpiry = false;
	RpcExpireBuckets::iterator stExpiryBucket;
	std::list<RpcExpire>::iterator stExpiry;
};

// 同时长按登记顺序排队；槽直接持有索引，请求结束时 O(1) 摘除对应节点。
struct RpcCallShard
{
	::OI::Thread::Mutex stMutex;
	std::unordered_map<uint64_t, RpcCoroSlot> mSlots;
	RpcExpireBuckets mExpire;
};

// 调用者持分片锁。删除空桶，避免动态超时参数让桶本身无限增长。
void RemoveRpcExpiry(RpcCallShard& stShardParam, RpcCoroSlot& stSlotParam)
{
	if (!stSlotParam.bHasExpiry)
	{
		return;
	}
	auto stBucket = stSlotParam.stExpiryBucket;
	stBucket->second.erase(stSlotParam.stExpiry);
	if (stBucket->second.empty())
	{
		stShardParam.mExpire.erase(stBucket);
	}
	stSlotParam.bHasExpiry = false;
	stSlotParam.stExpiry = {};
	stSlotParam.stExpiryBucket = {};
}

const uint64_t kCallShards = 64;
const int kSweepMs = 200;
RpcCallShard g_call_shards[kCallShards];
std::atomic<uint64_t> g_call_seq(1);
std::once_flag g_sweep_once;

::OI::Thread::Mutex g_rpc_handle_lock;
std::map<proto_type_t, RpcHandleFn> g_rpc_handle;

int64_t NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

RpcCallShard& ShardOf(uint64_t iCallIdParam)
{
	return g_call_shards[iCallIdParam % kCallShards];
}

void FinishRpcSlot(RpcCoroSlot& stSlotParam, int32_t iCodeParam, ::OI::Base::BytesStream* pBodyParam)
{
	if (stSlotParam.bAwaitingConnect)
	{
		stSlotParam.pManager->DropPendingCall(stSlotParam.iCallId);
	}
	if (pBodyParam)
	{
		if (stSlotParam.fnDecode)
		{
			stSlotParam.fnDecode(*pBodyParam);
		}
	}
	else if (stSlotParam.pResult)
	{
		*stSlotParam.pResult = iCodeParam;
	}
	if (stSlotParam.fnResume)
	{
		stSlotParam.fnResume();
	}
	else if (stSlotParam.stHandle)
	{
		coro::Schedule(stSlotParam.stHandle);
	}
}

bool TakeRpcSlot(uint64_t iCallIdParam, RpcCoroSlot& stOutputParam, ::OI::Net::SessionManager* pManagerParam = NULL, sid_t iSessionIdParam = 0, proto_type_t iResponseIdParam = 0)
{
	RpcCallShard& stShard = ShardOf(iCallIdParam);
	::OI::Thread::MutexGuard stLock(stShard.stMutex);
	auto stIterator = stShard.mSlots.find(iCallIdParam);
	if (stIterator == stShard.mSlots.end())
	{
		return false;
	}
	// 来源或协议错误不能消费其他连接的等待项。
	if (pManagerParam && (stIterator->second.pManager != pManagerParam || stIterator->second.iSessionId != iSessionIdParam || stIterator->second.iResponseId != iResponseIdParam))
	{
		return false;
	}
	RemoveRpcExpiry(stShard, stIterator->second);
	stOutputParam = std::move(stIterator->second);
	stShard.mSlots.erase(stIterator);
	return true;
}

void SweepRpcCalls()
{
	int64_t iNow = NowMs();
	std::vector<RpcCoroSlot> vFired;
	for (uint64_t iIndexLocal = 0; iIndexLocal < kCallShards; ++iIndexLocal)
	{
		RpcCallShard& stShard = g_call_shards[iIndexLocal];
		::OI::Thread::MutexGuard stLock(stShard.stMutex);
		for (auto stBit = stShard.mExpire.begin(); stBit != stShard.mExpire.end();)
		{
			std::list<RpcExpire>& vQ = stBit->second;
			while (!vQ.empty())
			{
				auto stIterator = stShard.mSlots.find(vQ.front().iCallId);
				if (stIterator == stShard.mSlots.end())
				{
					vQ.pop_front();
					continue;
				}
				if (vQ.front().iDeadlineMilliseconds > iNow)
				{
					break;
				}
				stIterator->second.bHasExpiry = false; // 本轮在桶内直接摘除，避免使外层迭代器失效。
				stIterator->second.stExpiry = {};
				stIterator->second.stExpiryBucket = {};
				vFired.push_back(std::move(stIterator->second));
				stShard.mSlots.erase(stIterator);
				vQ.pop_front();
			}
			if (vQ.empty())
			{
				stBit = stShard.mExpire.erase(stBit);
			}
			else
			{
				++stBit;
			}
		}
	}
	for (size_t iIndexLocal = 0; iIndexLocal < vFired.size(); ++iIndexLocal)
	{
		// 还没连上就到期，按连接失败返回，不占超时码。
		int32_t iCodeLocal = vFired[iIndexLocal].bAwaitingConnect ? OI_ERR_CONNECT : OI_ERR_TIMEOUT;
		FinishRpcSlot(vFired[iIndexLocal], iCodeLocal, NULL);
	}
}

class RpcSweepTimer : public ::OI::Timer::BaseTimer
{
  public:
	bool OnTimer()
	{
		SweepRpcCalls();
		return true;
	}
};

void ArmRpcSweep(void*)
{
	::OI::Timer::BaseTimerManager::GetInstance().AddTimer(new RpcSweepTimer(), kSweepMs);
}
}

RpcWaitStats GetRpcWaitStats()
{
	RpcWaitStats stStats{};
	for (auto& stShard : g_call_shards)
	{
		::OI::Thread::MutexGuard stLock(stShard.stMutex);
		stStats.iCalls += stShard.mSlots.size();
		stStats.iBuckets += stShard.mExpire.size();
		for (const auto& stBucket : stShard.mExpire)
		{
			stStats.iExpirations += stBucket.second.size();
		}
	}
	return stStats;
}

bool MarkRpcAwaitingConnect(uint64_t iCallIdParam)
{
	RpcCallShard& stShard = ShardOf(iCallIdParam);
	::OI::Thread::MutexGuard stLock(stShard.stMutex);
	auto stIterator = stShard.mSlots.find(iCallIdParam);
	if (stIterator == stShard.mSlots.end())
	{
		return false;
	}
	stIterator->second.bAwaitingConnect = true;
	return true;
}

bool ClaimRpcSend(uint64_t iCallIdParam, sid_t iSessionIdParam)
{
	RpcCallShard& stShard = ShardOf(iCallIdParam);
	::OI::Thread::MutexGuard stLock(stShard.stMutex);
	auto stIterator = stShard.mSlots.find(iCallIdParam);
	if (stIterator == stShard.mSlots.end())
	{
		return false;
	}
	stIterator->second.bAwaitingConnect = false;
	stIterator->second.iSessionId = iSessionIdParam;
	return true;
}

uint64_t BindRpcCall(::OI::Net::SessionManager* pManagerParam, sid_t iSessionIdParam, proto_type_t iResponseIdParam, std::coroutine_handle<> stHandleParam, int32_t* pResultParam, int64_t iTimeoutMillisecondsParam, std::function<bool(::OI::Base::BytesStream&)> fnDecodeParam, std::function<void()> fnResumeParam)
{
	std::call_once(g_sweep_once, []()
	{
		::OI::Net::Poll::GetInstance().CallOnLoop(ArmRpcSweep, NULL);
	});
	// 最高位留给请求标记，槽里只存去掉标记的号。
	uint64_t iCallIdLocal = g_call_seq.fetch_add(1, std::memory_order_relaxed) & ~(uint64_t)RPC_REQ_MASK;
	RpcCoroSlot stSlot;
	stSlot.pManager = pManagerParam;
	stSlot.iSessionId = iSessionIdParam;
	stSlot.stHandle = stHandleParam;
	stSlot.pResult = pResultParam;
	stSlot.fnDecode = std::move(fnDecodeParam);
	stSlot.fnResume = std::move(fnResumeParam);
	stSlot.iResponseId = iResponseIdParam;
	stSlot.iCallId = iCallIdLocal;
	stSlot.bAwaitingConnect = false;
	RpcCallShard& stShard = ShardOf(iCallIdLocal);
	::OI::Thread::MutexGuard stLock(stShard.stMutex);
	auto stInserted = stShard.mSlots.emplace(iCallIdLocal, std::move(stSlot)).first;
	try
	{
		if (iTimeoutMillisecondsParam > 0)
		{
			auto stBucket = stShard.mExpire.try_emplace(iTimeoutMillisecondsParam).first;
			try
			{
				const int64_t iNow = NowMs();
				const int64_t iMaxTime = (std::numeric_limits<int64_t>::max)();
				stBucket->second.push_back({iTimeoutMillisecondsParam > iMaxTime - iNow ? iMaxTime : iNow + iTimeoutMillisecondsParam, iCallIdLocal});
			}
			catch (...)
			{
				if (stBucket->second.empty())
				{
					stShard.mExpire.erase(stBucket);
				}
				throw;
			}
			stInserted->second.stExpiryBucket = stBucket;
			stInserted->second.stExpiry = std::prev(stBucket->second.end());
			stInserted->second.bHasExpiry = true;
		}
	}
	catch (...)
	{
		// 登记失败必须在放锁前撤销：调用者还没有拿到调用号，不能自行清理。
		stShard.mSlots.erase(stInserted);
		throw;
	}
	return iCallIdLocal;
}

bool WakeRpcCall(uint64_t iCallIdParam, int32_t iCodeParam, ::OI::Base::BytesStream* pBodyParam)
{
	RpcCoroSlot stSlot;
	if (!TakeRpcSlot(iCallIdParam, stSlot))
	{
		return false;
	}
	FinishRpcSlot(stSlot, iCodeParam, pBodyParam);
	return true;
}

void FailRpcCalls(::OI::Net::SessionManager* pManagerParam, sid_t iSessionIdParam)
{
	std::vector<RpcCoroSlot> vFailed;
	for (auto& stShard : g_call_shards)
	{
		::OI::Thread::MutexGuard stLock(stShard.stMutex);
		for (auto stIterator = stShard.mSlots.begin(); stIterator != stShard.mSlots.end();)
		{
			if (stIterator->second.pManager == pManagerParam && stIterator->second.iSessionId == iSessionIdParam)
			{
				RemoveRpcExpiry(stShard, stIterator->second);
				vFailed.push_back(std::move(stIterator->second));
				stIterator = stShard.mSlots.erase(stIterator);
			}
			else
			{
				++stIterator;
			}
		}
	}
	// 唤醒放在锁外，应用可以立即发起新的调用。
	for (auto& stSlot : vFailed)
	{
		FinishRpcSlot(stSlot, OI_ERR_CONNECT, NULL);
	}
}

void BindRpcHandler(proto_type_t iIdParam, RpcHandleFn fnHandleParam)
{
	::OI::Thread::MutexGuard stLock(g_rpc_handle_lock);
	g_rpc_handle[iIdParam] = std::move(fnHandleParam);
}

bool DeliverRpc(proto_type_t iIdParam, ::OI::Base::BytesStream& stBodyParam, ::OI::Net::SessionManager* pManagerParam, sid_t iSessionIdParam)
{
	uint64_t iRaw = 0;
	bool bIsRequest = false;
	try
	{
		stBodyParam.StartTransaction();
		stBodyParam >> iRaw;
		stBodyParam.RollBack();
		bIsRequest = (iRaw & (uint64_t)RPC_REQ_MASK) != 0;
	}
	catch (::OI::Base::ParserException&)
	{
		return false;
	}

	const RpcHandleFn* pHandle = NULL;
	if (bIsRequest)
	{
		::OI::Thread::MutexGuard stLock(g_rpc_handle_lock);
		auto stHandlerIterator = g_rpc_handle.find(iIdParam);
		if (stHandlerIterator == g_rpc_handle.end())
		{
			return false;
		}
		// 处理函数只在启动时注册，map 节点不会再动，出锁后用指针调用。
		pHandle = &stHandlerIterator->second;
	}
	try
	{
		if (!bIsRequest)
		{
			uint64_t iCallIdLocal = 0;
			stBodyParam >> iCallIdLocal;
			iCallIdLocal &= ~(uint64_t)RPC_REQ_MASK;
			RpcCoroSlot stSlot;
			if (!TakeRpcSlot(iCallIdLocal, stSlot, pManagerParam, iSessionIdParam, iIdParam))
			{
				return true;
			}
			FinishRpcSlot(stSlot, 0, &stBodyParam);
		}
		else
		{
			coro::StartTask([pHandle, pManagerParam, iSessionIdParam, &stBodyParam](coro::Token iTokenParam)
			{
				return (*pHandle)(iTokenParam, pManagerParam, iSessionIdParam, std::move(stBodyParam));
			});
		}
	}
	catch (::OI::Base::ParserException&)
	{
	}
	return true;
}
}
}
