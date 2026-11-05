#include "oi_rpc_call.h"
#include "oi_timer_manager.h"
#include "oi_poll.h"
#include "oi_rpc_coro.h"
#include "oi_thread.h"
#include "oi_config.h"
#include "oi_registry.h"
#include <algorithm>
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
namespace OI
{
namespace Rpc
{

namespace
{
typedef std::function<coro::Task(coro::Token, ::OI::Net::SessionManager*, sid_t, ::OI::Base::BytesStream)> RpcHandleFn;

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
	static void ReadLimit(const char* pKeyParam, size_t& iLimitParam)
	{
		int64_t iValue = static_cast<int64_t>(iLimitParam);
		TryGetConfig(pKeyParam, iValue);
		if (iValue <= 0 || static_cast<uint64_t>(iValue) > SIZE_MAX)
		{
			throw std::invalid_argument(pKeyParam);
		}
		iLimitParam = static_cast<size_t>(iValue);
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
	RpcAdmissionLease(RpcAdmissionLease&& stOtherParam) noexcept
	    : stKey(stOtherParam.stKey), iBytes(stOtherParam.iBytes), bAcquired(std::exchange(stOtherParam.bAcquired, false))
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

std::optional<RpcAdmissionLease> AcquireRpc(::OI::Net::SessionManager* pManagerParam, sid_t iSessionIdParam, size_t iBytesParam)
{
	auto& stAdmission = Admission();
	::OI::Thread::MutexGuard stLock(stAdmission.stMutex);
	const auto stKey = std::make_pair(pManagerParam, iSessionIdParam);
	auto it = stAdmission.mSessions.find(stKey);
	const auto stUsage = it == stAdmission.mSessions.end() ? RpcAdmission::Usage{} : it->second;
	if (stAdmission.stTotal.iCount >= stAdmission.iMaxCount ||
	    iBytesParam > stAdmission.iMaxBytes - stAdmission.stTotal.iBytes ||
	    stUsage.iCount >= stAdmission.iSessionMaxCount ||
	    iBytesParam > stAdmission.iSessionMaxBytes - stUsage.iBytes)
	{
		return {};
	}
	// 所有可能分配的操作先完成，再一次性扣额度；构造或提交失败由 lease 归还。
	RpcAdmissionLease stLease;
	auto& stSession = stAdmission.mSessions[stKey];
	stLease.stKey = stKey;
	stLease.iBytes = iBytesParam;
	stLease.bAcquired = true;
	++stSession.iCount;
	stSession.iBytes += iBytesParam;
	++stAdmission.stTotal.iCount;
	stAdmission.stTotal.iBytes += iBytesParam;
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
	::OI::Net::SessionManager* pManager;
	sid_t iSessionId;
	std::coroutine_handle<> stHandle;
	int32_t* pResult;
	std::function<bool(::OI::Base::BytesStream&)> fnDecode;
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

// 同时长按登记顺序排队；槽直接持有索引，请求结束时 O(1) 摘除对应节点。
struct RpcCallShard
{
	::OI::Thread::Mutex stMutex;
	std::unordered_map<uint64_t, RpcCoroSlot> mSlots;
	RpcExpireBuckets mExpire;
	std::map<RpcSessionKey, std::list<uint64_t>> mSessions;
};

// 与等待项共用分片锁：登记、换绑、超时、回包及断连原子维护会话索引。
std::list<uint64_t>::iterator AddSessionIndex(RpcCallShard& stShardParam, const RpcCoroSlot& stSlotParam, sid_t iSessionIdParam)
{
	auto it = stShardParam.mSessions.try_emplace(RpcSessionKey{stSlotParam.pManager, iSessionIdParam}).first;
	try
	{
		it->second.push_back(stSlotParam.iCallId);
	}
	catch (...)
	{
		if (it->second.empty())
		{
			stShardParam.mSessions.erase(it);
		}
		throw;
	}
	return std::prev(it->second.end());
}

void RemoveSessionIndex(RpcCallShard& stShardParam, RpcCoroSlot& stSlotParam)
{
	if (!stSlotParam.bIndexed)
	{
		return;
	}
	auto it = stShardParam.mSessions.find({stSlotParam.pManager, stSlotParam.iSessionId});
	it->second.erase(stSlotParam.stSessionIndex);
	if (it->second.empty())
	{
		stShardParam.mSessions.erase(it);
	}
	stSlotParam.bIndexed = false;
}

// 调用者持分片锁。删除空桶，避免动态超时参数让桶本身无限增长。
void ArmRpcGuard(RpcCoroSlot& stSlotParam)
{
	if (!stSlotParam.pGuard || stSlotParam.bGuardArmed)
	{
		return;
	}
	stSlotParam.pGuard->fetch_add(1, std::memory_order_acq_rel);
	stSlotParam.bGuardArmed = true;
}

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
	} stRelease{stSlotParam.pGuard, stSlotParam.bGuardArmed};
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
	RemoveSessionIndex(stShard, stIterator->second);
	ArmRpcGuard(stIterator->second);
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
				RemoveSessionIndex(stShard, stIterator->second);
				ArmRpcGuard(stIterator->second);
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

bool InitRpcAdmission()
{
	try
	{
		(void)Admission();
		return true;
	}
	catch (const std::exception& stError)
	{
		fprintf(stderr, "invalid RPC admission configuration: %s\n", stError.what());
		return false;
	}
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
	if (stIterator->second.iSessionId != iSessionIdParam)
	{
		const auto stIndex = AddSessionIndex(stShard, stIterator->second, iSessionIdParam);
		RemoveSessionIndex(stShard, stIterator->second);
		stIterator->second.stSessionIndex = stIndex;
		stIterator->second.bIndexed = true;
		stIterator->second.iSessionId = iSessionIdParam;
	}
	stIterator->second.bAwaitingConnect = false;
	return true;
}

uint64_t BindRpcCall(::OI::Net::SessionManager* pManagerParam, sid_t iSessionIdParam, proto_type_t iResponseIdParam, std::coroutine_handle<> stHandleParam, int32_t* pResultParam, int64_t iTimeoutMillisecondsParam, std::function<bool(::OI::Base::BytesStream&)> fnDecodeParam, std::function<void()> fnResumeParam, std::shared_ptr<std::atomic<int32_t>> pGuardParam)
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
	stSlot.pGuard = std::move(pGuardParam);
	RpcCallShard& stShard = ShardOf(iCallIdLocal);
	::OI::Thread::MutexGuard stLock(stShard.stMutex);
	auto stInserted = stShard.mSlots.emplace(iCallIdLocal, std::move(stSlot)).first;
	try
	{
		stInserted->second.stSessionIndex = AddSessionIndex(stShard, stInserted->second, iSessionIdParam);
		stInserted->second.bIndexed = true;
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
		RemoveSessionIndex(stShard, stInserted->second);
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

void AbandonRpcCall(uint64_t iCallIdParam)
{
	RpcCoroSlot stSlot;
	{
		RpcCallShard& stShard = ShardOf(iCallIdParam);
		::OI::Thread::MutexGuard stLock(stShard.stMutex);
		auto stIterator = stShard.mSlots.find(iCallIdParam);
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
		stSlot.pManager->DropPendingCall(stSlot.iCallId);
	}
}

void WaitRpcGuard(const std::shared_ptr<std::atomic<int32_t>>& pGuardParam)
{
	if (!pGuardParam)
	{
		return;
	}
	while (pGuardParam->load(std::memory_order_acquire) != 0)
	{
		std::this_thread::yield();
	}
}

void FailRpcCalls(::OI::Net::SessionManager* pManagerParam, sid_t iSessionIdParam)
{
	std::vector<RpcCoroSlot> vFailed;
	for (auto& stShard : g_call_shards)
	{
		::OI::Thread::MutexGuard stLock(stShard.stMutex);
		auto it = stShard.mSessions.find({pManagerParam, iSessionIdParam});
		if (it == stShard.mSessions.end())
		{
			continue;
		}
		vFailed.reserve(vFailed.size() + it->second.size());
		for (const auto& iId : it->second)
		{
			auto stSlot = stShard.mSlots.find(iId);
			RemoveRpcExpiry(stShard, stSlot->second);
			stSlot->second.bIndexed = false;
			ArmRpcGuard(stSlot->second);
			vFailed.push_back(std::move(stSlot->second));
			stShard.mSlots.erase(stSlot);
		}
		stShard.mSessions.erase(it);
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
			auto stLease = AcquireRpc(pManagerParam, iSessionIdParam, stBodyParam.Size());
			if (!stLease)
			{
				::OI::Base::BytesStream stReply;
				stReply << (iRaw & ~(uint64_t)RPC_REQ_MASK) << int32_t(OI_ERR_BUSY);
				auto stFrame = EncodePbFrame(iIdParam, stReply);
				if (!pManagerParam->SendBytes(iSessionIdParam, stFrame.GetMutableBuff()))
				{
					pManagerParam->CloseSession(iSessionIdParam);
				}
				return true;
			}
			// lease 随根协程闭包存活；业务异常、回包失败和正常结束均自动归还额度。
			coro::StartTask([stLease = std::move(stLease), pHandle, pManagerParam, iSessionIdParam, stBody = std::move(stBodyParam)](coro::Token iTokenParam) mutable
			{
				return (*pHandle)(iTokenParam, pManagerParam, iSessionIdParam, std::move(stBody));
			});
		}
	}
	catch (::OI::Base::ParserException&)
	{
	}
	return true;
}

namespace
{
bool ValidServiceToken(const std::string& sTextParam)
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
}

struct ParsedTarget
{
	std::string sName;
	std::string sZone;
	bool bAll = false;
};

bool ParseTarget(const std::string& sTargetParam, ParsedTarget& stTargetParam)
{
	if (sTargetParam.size() >= 3 && sTargetParam.compare(sTargetParam.size() - 2, 2, ".*") == 0)
	{
		stTargetParam.sName = sTargetParam.substr(0, sTargetParam.size() - 2);
		stTargetParam.bAll = true;
		return ValidServiceToken(stTargetParam.sName);
	}
	size_t iDot = sTargetParam.find('.');
	if (iDot == std::string::npos)
	{
		stTargetParam.sName = sTargetParam;
		return ValidServiceToken(stTargetParam.sName);
	}
	if (sTargetParam.find('.', iDot + 1) != std::string::npos)
	{
		return false;
	}
	stTargetParam.sName = sTargetParam.substr(0, iDot);
	stTargetParam.sZone = sTargetParam.substr(iDot + 1);
	return ValidServiceToken(stTargetParam.sName) && ValidServiceToken(stTargetParam.sZone);
}

class RoutedClient : public ProtoClientManager
{
	std::string m_sLabel;

  public:
	explicit RoutedClient(std::string sLabelParam)
	    : m_sLabel(std::move(sLabelParam))
	{
	}
	const std::string GetIDString()
	{
		return m_sLabel;
	}
	void Open(const std::string& sIpParam, const std::string& sPortParam)
	{
		DefaultInit(sIpParam, sPortParam, ::OI::Net::SESSION_TYPE_TCP);
		sEndpoint = sIpParam + ":" + sPortParam;
		bServiceSeen = true;
	}
	void OnClientConnect(::OI::Net::Session*)
	{
	}
	void OnClientDisconnect(const ::OI::Net::Session*)
	{
	}
};

ProtoClientManager* ClientFor(const std::string& sIpParam, const std::string& sPortParam)
{
	struct Pool
	{
		::OI::Thread::Mutex stMutex;
		std::map<std::string, std::unique_ptr<RoutedClient>> mClients;
	};
	static Pool stPool;
	std::string sKey = sIpParam + ":" + sPortParam;
	RoutedClient* pClient = NULL;
	bool bNew = false;
	{
		::OI::Thread::MutexGuard stLock(stPool.stMutex);
		auto stIterator = stPool.mClients.find(sKey);
		if (stIterator == stPool.mClients.end())
		{
			std::unique_ptr<RoutedClient> pNew(new RoutedClient(sKey));
			pNew->Open(sIpParam, sPortParam);
			pClient = pNew.get();
			stPool.mClients.emplace(sKey, std::move(pNew));
			bNew = true;
		}
		else
		{
			pClient = stIterator->second.get();
		}
	}
	if (bNew)
	{
		pClient->StartClient();
	}
	return pClient;
}
}

bool PrepareRoute(::OI::Net::SessionManager* pManagerParam, const std::string& sNameParam, ServicePick iPickParam, const std::string& sZoneParam, ServiceRoute& stRouteParam)
{
	if (!ValidServiceToken(sNameParam) || (iPickParam == ServicePick::Zone && !ValidServiceToken(sZoneParam)))
	{
		stRouteParam.iStatus = ServiceRouteStatus::BadName;
		return true;
	}
	if (iPickParam == ServicePick::Any && pManagerParam)
	{
		std::vector<::OI::Net::SessionManager*> vWatchers = ::OI::Base::ServiceTable::GetInstance().Watchers(sNameParam);
		for (auto* pWatcher : vWatchers)
		{
			if (pWatcher != pManagerParam)
			{
				continue;
			}
			ProtoClientManager* pClient = dynamic_cast<ProtoClientManager*>(pManagerParam);
			if (pClient && pClient->IsConnect())
			{
				stRouteParam.iStatus = ServiceRouteStatus::Ok;
				stRouteParam.vClients.push_back(pClient);
				return true;
			}
		}
	}
	return false;
}

void PullRoute(const std::string& sNameParam, std::function<void(int32_t, const std::vector<::OI::Base::ServiceInstance>&)> fnDoneParam)
{
	::OI::Base::RegistryClient::GetInstance().PullAsync(sNameParam, std::move(fnDoneParam));
}

bool PrepareTarget(const std::string& sTargetParam, bool bBatchParam, ServiceRoute& stRouteParam, std::string& sNameParam, ServicePick& iPickParam, std::string& sZoneParam)
{
	ParsedTarget stTarget;
	if (!ParseTarget(sTargetParam, stTarget) || stTarget.bAll != bBatchParam)
	{
		stRouteParam.iStatus = ServiceRouteStatus::BadName;
		return true;
	}
	sNameParam = stTarget.sName;
	sZoneParam = stTarget.sZone;
	iPickParam = ServicePick::Any;
	if (bBatchParam)
	{
		iPickParam = ServicePick::All;
	}
	else if (!stTarget.sZone.empty())
	{
		iPickParam = ServicePick::Zone;
	}
	return false;
}

bool PrepareZones(::OI::Net::SessionManager* pManagerParam, const std::string& sNameParam, const std::vector<int32_t>& vZoneIdsParam, ServiceRoute& stRouteParam, std::vector<std::string>& vZonesParam)
{
	(void)pManagerParam;
	vZonesParam.clear();
	if (!ValidServiceToken(sNameParam) || vZoneIdsParam.empty())
	{
		stRouteParam.iStatus = ServiceRouteStatus::BadName;
		return true;
	}
	vZonesParam.reserve(vZoneIdsParam.size());
	for (int32_t iZoneId : vZoneIdsParam)
	{
		if (iZoneId < 0)
		{
			stRouteParam.iStatus = ServiceRouteStatus::BadName;
			vZonesParam.clear();
			return true;
		}
		vZonesParam.push_back(std::to_string(iZoneId));
	}
	return false;
}

ServiceRoute FinishRoute(const std::string& sNameParam, ServicePick iPickParam, const std::string& sZoneParam, int32_t iCodeParam, const std::vector<::OI::Base::ServiceInstance>& vListParam)
{
	ServiceRoute stRoute;
	if (iCodeParam != OI_OK)
	{
		stRoute.iStatus = ServiceRouteStatus::Offline;
		return stRoute;
	}
	bool bGlobal = false;
	bool bZoned = false;
	std::vector<::OI::Base::ServiceInstance> vMatched;
	for (const auto& stInst : vListParam)
	{
		if (stInst.sName != sNameParam)
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
		if (iPickParam == ServicePick::Zone)
		{
			if (stInst.sZone == sZoneParam)
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
	if (iPickParam == ServicePick::Zone && bGlobal)
	{
		stRoute.iStatus = ServiceRouteStatus::WrongKind;
		return stRoute;
	}
	if (vMatched.empty())
	{
		stRoute.iStatus = ServiceRouteStatus::Offline;
		return stRoute;
	}
	std::sort(vMatched.begin(), vMatched.end(), [](const ::OI::Base::ServiceInstance& stLeftParam, const ::OI::Base::ServiceInstance& stRightParam)
	{
		return stLeftParam.iId < stRightParam.iId;
	});
	if (iPickParam == ServicePick::All)
	{
		for (const auto& stInst : vMatched)
		{
			stRoute.vClients.push_back(ClientFor(stInst.sIp, stInst.sPort));
		}
	}
	else
	{
		size_t iPick = 0;
		if (iPickParam == ServicePick::Any && vMatched.size() > 1)
		{
			static std::atomic<uint32_t> iCursor{0};
			iPick = iCursor.fetch_add(1, std::memory_order_relaxed) % vMatched.size();
		}
		const ::OI::Base::ServiceInstance& stInst = vMatched[iPick];
		// 名单已选定地址，使用固定地址的连接；订阅管理器可能在提交前改连其他分区。
		stRoute.vClients.push_back(ClientFor(stInst.sIp, stInst.sPort));
	}
	stRoute.iStatus = ServiceRouteStatus::Ok;
	return stRoute;
}

ServiceRoute FinishZones(const std::string& sNameParam, const std::vector<std::string>& vZonesParam, int32_t iCodeParam, const std::vector<::OI::Base::ServiceInstance>& vListParam)
{
	ServiceRoute stRoute;
	if (iCodeParam != OI_OK)
	{
		stRoute.iStatus = ServiceRouteStatus::Offline;
		return stRoute;
	}
	bool bGlobal = false;
	for (const auto& stInst : vListParam)
	{
		if (stInst.sName != sNameParam)
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
	for (const auto& sZone : vZonesParam)
	{
		const ::OI::Base::ServiceInstance* pBest = nullptr;
		for (const auto& stInst : vListParam)
		{
			if (stInst.sName != sNameParam || stInst.sZone != sZone)
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
			continue;
		}
		// 每个槽位固定到本次名单中的地址，不能随调用方管理器的服务切换而漂移。
		stRoute.vClients.push_back(ClientFor(pBest->sIp, pBest->sPort));
	}
	stRoute.iStatus = ServiceRouteStatus::Ok;
	return stRoute;
}
}
}
