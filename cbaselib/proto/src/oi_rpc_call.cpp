#include "oi_log.h"
#include <cstdio>
#include "oi_rpc_call.h"
#include "oi_timer_manager.h"
#include "oi_poll.h"
#include "oi_rpc_coro.h"
#include "oi_thread.h"
#include "oi_config.h"
#include "oi_registry.h"
#include <algorithm>
#include <array>
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>
#include <tuple>
#include <type_traits>
#include <uv.h>
namespace OI
{
namespace Rpc
{

namespace
{
typedef std::function<OI::Coro::Task(OI::Coro::Token, ::OI::Net::SessionManager*, sid_t, ::OI::Base::BytesStream)> RpcHandleFn;

struct RpcAdmission
{
	struct Usage
	{
		size_t iCount = 0;
		size_t iBytes = 0;
	};
	::OI::Thread::Mutex stMutex;
	Usage stTotal;
	std::map<std::pair<::OI::Net::SessionManager*, sid_t>, Usage> mSessions;
	size_t iMaxCount = 65536;
	size_t iMaxBytes = 64 * 1024 * 1024;
	size_t iSessionMaxCount = 4096;
	size_t iSessionMaxBytes = 16 * 1024 * 1024;
	RpcAdmission()
	{
		ReadLimit("Rpc.max_inflight_count", iMaxCount);
		ReadLimit("Rpc.max_inflight_bytes", iMaxBytes);
		ReadLimit("Rpc.session_max_inflight_count", iSessionMaxCount);
		ReadLimit("Rpc.session_max_inflight_bytes", iSessionMaxBytes);
	}
	static void ReadLimit(const char* pKey, size_t& iLimit)
	{
		int64_t iValue = static_cast<int64_t>(iLimit);
		TryGetConfig(pKey, iValue);
		if (iValue <= 0 || static_cast<uint64_t>(iValue) > SIZE_MAX)
		{
			throw std::invalid_argument(pKey);
		}
		iLimit = static_cast<size_t>(iValue);
	}
};

RpcAdmission& Admission()
{
	static RpcAdmission stAdmission;
	return stAdmission;
}

struct RpcAdmissionLease
{
	std::pair<::OI::Net::SessionManager*, sid_t> stKey;
	size_t iBytes = 0;
	bool bAcquired = false;
	RpcAdmissionLease() = default;
	RpcAdmissionLease(const RpcAdmissionLease&) = delete;
	RpcAdmissionLease(RpcAdmissionLease&& stOther) noexcept
	    : stKey(stOther.stKey), iBytes(stOther.iBytes), bAcquired(std::exchange(stOther.bAcquired, false))
	{
	}
	~RpcAdmissionLease()
	{
		if (!bAcquired)
		{
			return;
		}
		auto& stAdmission = Admission();
		::OI::Thread::MutexGuard stLock(stAdmission.stMutex);
		auto it = stAdmission.mSessions.find(stKey);
		--stAdmission.stTotal.iCount;
		stAdmission.stTotal.iBytes -= iBytes;
		it->second.iBytes -= iBytes;
		if (--it->second.iCount == 0)
		{
			stAdmission.mSessions.erase(it);
		}
	}
};

std::optional<RpcAdmissionLease> AcquireRpc(::OI::Net::SessionManager* pManagerLocal, sid_t iSessionIdLocal, size_t iBytesLocal)
{
	auto& stAdmission = Admission();
	::OI::Thread::MutexGuard stLock(stAdmission.stMutex);
	const auto stKey = std::make_pair(pManagerLocal, iSessionIdLocal);
	auto it = stAdmission.mSessions.find(stKey);
	const auto stUsage = it == stAdmission.mSessions.end() ? RpcAdmission::Usage{} : it->second;
	if (stAdmission.stTotal.iCount >= stAdmission.iMaxCount ||
	    iBytesLocal > stAdmission.iMaxBytes - stAdmission.stTotal.iBytes ||
	    stUsage.iCount >= stAdmission.iSessionMaxCount ||
	    iBytesLocal > stAdmission.iSessionMaxBytes - stUsage.iBytes)
	{
		return {};
	}
	// 所有可能分配的操作先完成，再一次性扣额度；构造或提交失败由 lease 归还。
	RpcAdmissionLease stLease;
	auto& stSession = stAdmission.mSessions[stKey];
	stLease.stKey = stKey;
	stLease.iBytes = iBytesLocal;
	stLease.bAcquired = true;
	++stSession.iCount;
	stSession.iBytes += iBytesLocal;
	++stAdmission.stTotal.iCount;
	stAdmission.stTotal.iBytes += iBytesLocal;
	return stLease;
}

struct RpcExpire
{
	int64_t iDeadlineMilliseconds;
	uint64_t iCallId;
};
using RpcExpireBuckets = std::map<int64_t, std::list<RpcExpire>>;
using RpcSessionKey = std::pair<::OI::Net::SessionManager*, sid_t>;

struct RpcCoroSlot
{
	RpcSender* pSender = nullptr;
	::OI::Net::SessionManager* pManager;
	sid_t iSessionId;
	std::coroutine_handle<> stHandle;
	int32_t* pResult;
	std::function<bool(::OI::Base::BytesStream&, sid_t)> fnDecode;
	std::function<void()> fnResume;
	proto_type_t iResponseId;
	uint64_t iCallId;
	bool bAwaitingConnect;
	bool bGuardArmed = false;
	std::shared_ptr<std::atomic<int32_t>> pGuard;
	bool bHasExpiry = false;
	bool bIndexed = false;
	std::list<uint64_t>::iterator stSessionIndex;
	RpcExpireBuckets::iterator stExpiryBucket;
	std::list<RpcExpire>::iterator stExpiry;
};
static_assert(std::is_nothrow_move_assignable_v<RpcCoroSlot>, "FailRpcCalls moves slots into a fixed array under the shard lock");

// 按绝对截止时间所在刻度分桶；槽持索引，完成时 O(1) 摘除节点。
struct RpcCallShard
{
	::OI::Thread::Mutex stMutex;
	std::unordered_map<uint64_t, RpcCoroSlot> mSlots;
	RpcExpireBuckets mExpire;
	std::map<RpcSessionKey, std::list<uint64_t>> mSessions;
};

// 与等待项共用分片锁：登记、换绑、超时、回包及断连原子维护会话索引。
std::list<uint64_t>::iterator AddSessionIndex(RpcCallShard& stShard, const RpcCoroSlot& stSlot, sid_t iSessionIdLocal)
{
	auto it = stShard.mSessions.try_emplace(RpcSessionKey{stSlot.pManager, iSessionIdLocal}).first;
	try
	{
		it->second.push_back(stSlot.iCallId);
	}
	catch (...)
	{
		if (it->second.empty())
		{
			stShard.mSessions.erase(it);
		}
		throw;
	}
	return std::prev(it->second.end());
}

void RemoveSessionIndex(RpcCallShard& stShard, RpcCoroSlot& stSlot)
{
	if (!stSlot.bIndexed)
	{
		return;
	}
	auto it = stShard.mSessions.find({stSlot.pManager, stSlot.iSessionId});
	it->second.erase(stSlot.stSessionIndex);
	if (it->second.empty())
	{
		stShard.mSessions.erase(it);
	}
	stSlot.bIndexed = false;
}

// 调用者持分片锁。删除空桶，避免动态超时参数让桶本身无限增长。
void ArmRpcGuard(RpcCoroSlot& stSlot)
{
	if (!stSlot.pGuard || stSlot.bGuardArmed)
	{
		return;
	}
	stSlot.pGuard->fetch_add(1, std::memory_order_acq_rel);
	stSlot.bGuardArmed = true;
}

void RemoveRpcExpiry(RpcCallShard& stShard, RpcCoroSlot& stSlot)
{
	if (!stSlot.bHasExpiry)
	{
		return;
	}
	auto stBucket = stSlot.stExpiryBucket;
	stBucket->second.erase(stSlot.stExpiry);
	if (stBucket->second.empty())
	{
		stShard.mExpire.erase(stBucket);
	}
	stSlot.bHasExpiry = false;
	stSlot.stExpiry = {};
	stSlot.stExpiryBucket = {};
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

RpcCallShard& ShardOf(uint64_t iCallIdLocal)
{
	return g_call_shards[iCallIdLocal % kCallShards];
}

void FinishRpcSlot(RpcCoroSlot& stSlot, int32_t iCode, ::OI::Base::BytesStream* pBody)
{
	struct GuardRelease
	{
		std::shared_ptr<std::atomic<int32_t>> pGuard;
		bool bArmed = false;
		~GuardRelease()
		{
			if (bArmed && pGuard)
			{
				pGuard->fetch_sub(1, std::memory_order_acq_rel);
			}
		}
	} stRelease{stSlot.pGuard, stSlot.bGuardArmed};
	if (stSlot.bAwaitingConnect)
	{
		if (stSlot.pSender)
		{
			stSlot.pSender->DropPendingCall(stSlot.iCallId);
		}
		else
		{
			stSlot.pManager->DropPendingCall(stSlot.iCallId);
		}
	}
	if (pBody)
	{
		if (stSlot.fnDecode)
		{
			stSlot.fnDecode(*pBody, stSlot.iSessionId);
		}
	}
	else if (stSlot.pResult)
	{
		*stSlot.pResult = iCode;
	}
	if (stSlot.fnResume)
	{
		stSlot.fnResume();
	}
	else if (stSlot.stHandle)
	{
		OI::Coro::Schedule(stSlot.stHandle);
	}
}

bool TakeRpcSlot(uint64_t iCallIdLocal, RpcCoroSlot& stOutput, ::OI::Net::SessionManager* pManagerLocal = NULL, sid_t iSessionIdLocal = 0, proto_type_t iResponseIdLocal = 0)
{
	RpcCallShard& stShard = ShardOf(iCallIdLocal);
	::OI::Thread::MutexGuard stLock(stShard.stMutex);
	auto stIterator = stShard.mSlots.find(iCallIdLocal);
	if (stIterator == stShard.mSlots.end())
	{
		return false;
	}
	// 来源或协议错误不能消费其他连接的等待项。
	if (pManagerLocal && (stIterator->second.pManager != pManagerLocal || stIterator->second.iSessionId != iSessionIdLocal || stIterator->second.iResponseId != iResponseIdLocal))
	{
		return false;
	}
	RemoveRpcExpiry(stShard, stIterator->second);
	RemoveSessionIndex(stShard, stIterator->second);
	ArmRpcGuard(stIterator->second);
	stOutput = std::move(stIterator->second);
	stShard.mSlots.erase(stIterator);
	return true;
}

void SweepRpcCalls()
{
	int64_t iNow = NowMs();
	std::vector<RpcCoroSlot> vFired;
	try
	{
		for (uint64_t iIndexLocal = 0; iIndexLocal < kCallShards; ++iIndexLocal)
		{
			RpcCallShard& stShard = g_call_shards[iIndexLocal];
			::OI::Thread::MutexGuard stLock(stShard.stMutex);
			for (auto stBit = stShard.mExpire.begin(); stBit != stShard.mExpire.end();)
			{
				if (stBit->first > iNow / kSweepMs)
				{
					break; // 后面的整桶尚未到期，不扫描长超时请求。
				}
				std::list<RpcExpire>& vQ = stBit->second;
				for (auto it = vQ.begin(); it != vQ.end();)
				{
					auto stIterator = stShard.mSlots.find(it->iCallId);
					if (stIterator == stShard.mSlots.end())
					{
						it = vQ.erase(it);
						continue;
					}
					if (it->iDeadlineMilliseconds > iNow)
					{
						// 当前刻度内截止时间不一定按登记顺序排列，不能挡住后面的到期项。
						++it;
						continue;
					}
					vFired.emplace_back();                 // 分配失败时不改变等待项及回调保活状态。
					stIterator->second.bHasExpiry = false; // 本轮在桶内直接摘除，避免使外层迭代器失效。
					stIterator->second.stExpiry = {};
					stIterator->second.stExpiryBucket = {};
					RemoveSessionIndex(stShard, stIterator->second);
					ArmRpcGuard(stIterator->second);
					vFired.back() = std::move(stIterator->second);
					stShard.mSlots.erase(stIterator);
					it = vQ.erase(it);
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
	}
	catch (...)
	{
		::OI::Thread::ReportTaskError(std::current_exception());
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
	bool OnTimer() override
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
		stStats.iSessionBuckets += stShard.mSessions.size();
		for (const auto& [stKey, vIds] : stShard.mSessions)
		{
			stStats.iSessionEntries += vIds.size();
		}
		for (const auto& stBucket : stShard.mExpire)
		{
			stStats.iExpirations += stBucket.second.size();
		}
	}
	return stStats;
}

int32_t InitRpcAdmission()
{
	try
	{
		(void)Admission();
		return OI_OK;
	}
	catch (const std::exception& stError)
	{
		fprintf(stderr, "invalid RPC admission configuration: %s\n", stError.what());
		return OI_ERR_INVALID_ARGUMENT;
	}
}

bool MarkRpcAwaitingConnect(uint64_t iCallIdLocal)
{
	RpcCallShard& stShard = ShardOf(iCallIdLocal);
	::OI::Thread::MutexGuard stLock(stShard.stMutex);
	auto stIterator = stShard.mSlots.find(iCallIdLocal);
	if (stIterator == stShard.mSlots.end())
	{
		return false;
	}
	stIterator->second.bAwaitingConnect = true;
	return true;
}

bool ClaimRpcSend(uint64_t iCallIdLocal, sid_t iSessionIdLocal)
{
	RpcCallShard& stShard = ShardOf(iCallIdLocal);
	::OI::Thread::MutexGuard stLock(stShard.stMutex);
	auto stIterator = stShard.mSlots.find(iCallIdLocal);
	if (stIterator == stShard.mSlots.end())
	{
		return false;
	}
	if (stIterator->second.iSessionId != iSessionIdLocal)
	{
		const auto stIndex = AddSessionIndex(stShard, stIterator->second, iSessionIdLocal);
		RemoveSessionIndex(stShard, stIterator->second);
		stIterator->second.stSessionIndex = stIndex;
		stIterator->second.bIndexed = true;
		stIterator->second.iSessionId = iSessionIdLocal;
	}
	stIterator->second.bAwaitingConnect = false;
	return true;
}

uint64_t BindRpcCall(::OI::Net::SessionManager* pManagerLocal,
    sid_t iSessionIdLocal,
    proto_type_t iResponseIdLocal,
    std::coroutine_handle<> stHandleLocal,
    int32_t* pResultLocal,
    int64_t iTimeoutMilliseconds,
    std::function<bool(::OI::Base::BytesStream&, sid_t)> fnDecodeLocal,
    std::function<void()> fnResumeLocal,
    std::shared_ptr<std::atomic<int32_t>> pGuardLocal,
    RpcSender* pSenderLocal)
{
	std::call_once(g_sweep_once, []()
	{
		::OI::Net::Poll::GetInstance().CallOnLoop(ArmRpcSweep, NULL);
	});
	// 最高位留给请求标记，槽里只存去掉标记的号。
	uint64_t iCallIdLocal = g_call_seq.fetch_add(1, std::memory_order_relaxed) & ~(uint64_t)RPC_REQ_MASK;
	RpcCoroSlot stSlot;
	stSlot.pManager = pManagerLocal;
	stSlot.iSessionId = iSessionIdLocal;
	stSlot.stHandle = stHandleLocal;
	stSlot.pResult = pResultLocal;
	stSlot.fnDecode = std::move(fnDecodeLocal);
	stSlot.fnResume = std::move(fnResumeLocal);
	stSlot.iResponseId = iResponseIdLocal;
	stSlot.iCallId = iCallIdLocal;
	stSlot.bAwaitingConnect = false;
	stSlot.pGuard = std::move(pGuardLocal);
	stSlot.pSender = pSenderLocal;
	RpcCallShard& stShard = ShardOf(iCallIdLocal);
	::OI::Thread::MutexGuard stLock(stShard.stMutex);
	auto stInserted = stShard.mSlots.emplace(iCallIdLocal, std::move(stSlot)).first;
	try
	{
		stInserted->second.stSessionIndex = AddSessionIndex(stShard, stInserted->second, iSessionIdLocal);
		stInserted->second.bIndexed = true;
		if (iTimeoutMilliseconds > 0)
		{
			const int64_t iNow = NowMs();
			const int64_t iMaxTime = (std::numeric_limits<int64_t>::max)();
			const int64_t iDeadline = iTimeoutMilliseconds > iMaxTime - iNow ? iMaxTime : iNow + iTimeoutMilliseconds;
			// 当前刻度内逐项校验精确截止时间，保持原扫描精度；存刻度编号避免溢出。
			const int64_t iBucket = iDeadline / kSweepMs;
			auto stBucket = stShard.mExpire.try_emplace(iBucket).first;
			try
			{
				stBucket->second.push_back({iDeadline, iCallIdLocal});
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
		RemoveSessionIndex(stShard, stInserted->second);
		stShard.mSlots.erase(stInserted);
		throw;
	}
	return iCallIdLocal;
}

bool WakeRpcCall(uint64_t iCallIdLocal, int32_t iCode, ::OI::Base::BytesStream* pBody)
{
	RpcCoroSlot stSlot;
	if (!TakeRpcSlot(iCallIdLocal, stSlot))
	{
		return false;
	}
	FinishRpcSlot(stSlot, iCode, pBody);
	return true;
}

void AbandonRpcCall(uint64_t iCallIdLocal)
{
	RpcCoroSlot stSlot;
	{
		RpcCallShard& stShard = ShardOf(iCallIdLocal);
		::OI::Thread::MutexGuard stLock(stShard.stMutex);
		auto stIterator = stShard.mSlots.find(iCallIdLocal);
		if (stIterator == stShard.mSlots.end())
		{
			return;
		}
		RemoveRpcExpiry(stShard, stIterator->second);
		RemoveSessionIndex(stShard, stIterator->second);
		stSlot = std::move(stIterator->second);
		stShard.mSlots.erase(stIterator);
	}
	if (stSlot.bAwaitingConnect && stSlot.pManager)
	{
		if (stSlot.pSender)
		{
			stSlot.pSender->DropPendingCall(stSlot.iCallId);
		}
		else
		{
			stSlot.pManager->DropPendingCall(stSlot.iCallId);
		}
	}
}

void WaitRpcGuard(const std::shared_ptr<std::atomic<int32_t>>& pGuardLocal)
{
	if (!pGuardLocal)
	{
		return;
	}
	while (pGuardLocal->load(std::memory_order_acquire) != 0)
	{
		std::this_thread::yield();
	}
}

void FailRpcCalls(::OI::Net::SessionManager* pManagerLocal, sid_t iSessionIdLocal)
{
	// 固定容量分批摘除，断连清理自身不分配内存。每批出锁后完成，异常不能遗失后续等待项。
	std::array<RpcCoroSlot, 32> vFailed{};
	for (auto& stShard : g_call_shards)
	{
		while (true)
		{
			size_t iCount = 0;
			{
				::OI::Thread::MutexGuard stLock(stShard.stMutex);
				auto it = stShard.mSessions.find({pManagerLocal, iSessionIdLocal});
				if (it == stShard.mSessions.end())
				{
					break;
				}
				while (!it->second.empty() && iCount < vFailed.size())
				{
					auto stSlot = stShard.mSlots.find(it->second.front());
					RemoveRpcExpiry(stShard, stSlot->second);
					stSlot->second.bIndexed = false;
					ArmRpcGuard(stSlot->second);
					vFailed[iCount++] = std::move(stSlot->second);
					stShard.mSlots.erase(stSlot);
					it->second.pop_front();
				}
				if (it->second.empty())
				{
					stShard.mSessions.erase(it);
				}
			}
			for (size_t iIndex = 0; iIndex < iCount; ++iIndex)
			{
				try
				{
					FinishRpcSlot(vFailed[iIndex], OI_ERR_CONNECT, nullptr);
				}
				catch (...)
				{
					::OI::Thread::ReportTaskError(std::current_exception());
				}
				vFailed[iIndex] = {};
			}
		}
	}
}

void BindRpcHandler(proto_type_t iId, RpcHandleFn fnHandle)
{
	::OI::Thread::MutexGuard stLock(g_rpc_handle_lock);
	g_rpc_handle[iId] = std::move(fnHandle);
}

bool DeliverRpc(proto_type_t iId, ::OI::Base::BytesStream& stBodyLocal, ::OI::Net::SessionManager* pManagerLocal, sid_t iSessionIdLocal)
{
	uint64_t iRaw = 0;
	bool bIsRequest = false;
	bool bIsNotify = false;
	try
	{
		stBodyLocal.StartTransaction();
		stBodyLocal >> iRaw;
		stBodyLocal.RollBack();
		// 调用号 0 是单向消息：和请求一样交给处理协程，不回包。
		bIsNotify = iRaw == 0;
		bIsRequest = (iRaw & (uint64_t)RPC_REQ_MASK) != 0 || bIsNotify;
	}
	catch (::OI::Base::ParserException&)
	{
		return false;
	}

	const RpcHandleFn* pHandle = NULL;
	if (bIsRequest)
	{
		::OI::Thread::MutexGuard stLock(g_rpc_handle_lock);
		auto stHandlerIterator = g_rpc_handle.find(iId);
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
			stBodyLocal >> iCallIdLocal;
			iCallIdLocal &= ~(uint64_t)RPC_REQ_MASK;
			RpcCoroSlot stSlot;
			if (!TakeRpcSlot(iCallIdLocal, stSlot, pManagerLocal, iSessionIdLocal, iId))
			{
				return true;
			}
			FinishRpcSlot(stSlot, 0, &stBodyLocal);
		}
		else
		{
			auto stLease = AcquireRpc(pManagerLocal, iSessionIdLocal, stBodyLocal.Size());
			if (!stLease && bIsNotify)
			{
				return true;
			}
			if (!stLease)
			{
				::OI::Base::BytesStream stReply;
				stReply << (iRaw & ~(uint64_t)RPC_REQ_MASK) << int32_t(OI_ERR_BUSY);
				auto stFrame = EncodePbFrame(iId, stReply);
				if (pManagerLocal->SendBytes(iSessionIdLocal, stFrame.GetMutableBuff()) != OI_OK)
				{
					pManagerLocal->CloseSession(iSessionIdLocal);
				}
				return true;
			}
			// lease 随根协程闭包存活；业务异常、回包失败和正常结束均自动归还额度。
			OI::Coro::StartTask([stLease = std::move(stLease), pOwner = pManagerLocal->HoldSessionTask(iSessionIdLocal), pHandle, pManagerLocal, iSessionIdLocal, stBody = std::move(stBodyLocal)](OI::Coro::Token iToken) mutable
			{
				return (*pHandle)(iToken, pManagerLocal, iSessionIdLocal, std::move(stBody));
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

namespace OI::Rpc
{
void ReportRpcError(OI::Coro::Token iToken, uint64_t iCallIdLocal, sid_t iSessionIdLocal,
    proto_type_t iProtocolId, const char* pStage, std::exception_ptr stErrorLocal) noexcept
{
	const auto fnWrite = [iToken, iCallIdLocal, iSessionIdLocal, iProtocolId, pStage](const char* pReason) noexcept
	{
		const ::OI::Log::DiagnosticText stReason(pReason ? pReason : "unknown exception");
		try
		{
			LOG_ERROR("rpc failure stage=%s token=%llu call=%llu sid=%llu proto=%llu ret=%d reason=%s truncated=%d",
			    pStage, (unsigned long long)iToken, (unsigned long long)iCallIdLocal,
			    (unsigned long long)iSessionIdLocal, (unsigned long long)iProtocolId, OI_ERR_FAIL,
			    stReason.GetText(), stReason.IsTruncated() ? 1 : 0);
			// 错误立即刷新本地输出，避免日志服不可用时仍滞留在 stdout 缓冲。
			fflush(stdout);
		}
		catch (...)
		{
			fprintf(stderr, "rpc failure stage=%s token=%llu call=%llu sid=%llu proto=%llu ret=%d reason=%s truncated=%d logger_failed=1\n",
			    pStage, (unsigned long long)iToken, (unsigned long long)iCallIdLocal,
			    (unsigned long long)iSessionIdLocal, (unsigned long long)iProtocolId, OI_ERR_FAIL,
			    stReason.GetText(), stReason.IsTruncated() ? 1 : 0);
		}
	};
	try
	{
		if (stErrorLocal)
		{
			std::rethrow_exception(stErrorLocal);
		}
		fnWrite("unknown exception");
	}
	catch (const std::exception& stError)
	{
		fnWrite(stError.what());
	}
	catch (...)
	{
		fnWrite("non-std exception");
	}
}
}
