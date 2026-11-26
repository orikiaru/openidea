#include "oi_log.h"
#include <cstdio>
#include "oi_rpc_call.h"
#include "oi_poll.h"
#include "oi_rpc_session_manager.h"
#include "oi_thread.h"
#include "oi_config.h"
#include "oi_duration.h"
#include <algorithm>
#include <array>
#include <memory>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <thread>
#include <list>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>
#include <tuple>
#include <type_traits>
namespace OI
{
namespace Rpc
{

int64_t NormalizeRpcTimeoutMilliseconds(int64_t iTimeoutMilliseconds)
{
	if (iTimeoutMilliseconds > 0)
	{
		return iTimeoutMilliseconds;
	}
	LOG_ERROR("invalid rpc timeout milliseconds=%lld force=%lld",
	    static_cast<long long>(iTimeoutMilliseconds), static_cast<long long>(kFallbackRpcTimeoutMilliseconds));
	return kFallbackRpcTimeoutMilliseconds;
}

RpcAdmissionLease::~RpcAdmissionLease()
{
	if (!bAcquired)
	{
		return;
	}
	RpcCallManager::GetInstance().ReleaseAdmission(*this);
}

void RpcCallManager::LoadAdmissionLocked()
{
	if (m_bAdmissionLoaded)
	{
		return;
	}
	const auto fnRead = [](const char* pKey, size_t& iLimit)
	{
		int64_t iValue = static_cast<int64_t>(iLimit);
		const auto& stConfig = ::OI::Base::Config::GlobalConfig();
		if ((stConfig.Contains(pKey) && !stConfig.GetAs(pKey, iValue)) || iValue <= 0 || static_cast<uint64_t>(iValue) > SIZE_MAX)
		{
			throw std::invalid_argument(pKey);
		}
		iLimit = static_cast<size_t>(iValue);
	};
	size_t iMaxCount = m_iMaxCount;
	size_t iMaxBytes = m_iMaxBytes;
	size_t iSessionMaxCount = m_iSessionMaxCount;
	size_t iSessionMaxBytes = m_iSessionMaxBytes;
	fnRead("Rpc.max_inflight_count", iMaxCount);
	fnRead("Rpc.max_inflight_bytes", iMaxBytes);
	fnRead("Rpc.session_max_inflight_count", iSessionMaxCount);
	fnRead("Rpc.session_max_inflight_bytes", iSessionMaxBytes);
	m_iMaxCount = iMaxCount;
	m_iMaxBytes = iMaxBytes;
	m_iSessionMaxCount = iSessionMaxCount;
	m_iSessionMaxBytes = iSessionMaxBytes;
	m_bAdmissionLoaded = true;
}

std::optional<RpcAdmissionLease> RpcCallManager::AcquireAdmission(::OI::Net::SessionManager* pManagerLocal, sid_t iSessionIdLocal, size_t iBytesLocal)
{
	::OI::Thread::MutexGuard stLock(m_stAdmissionLocker);
	LoadAdmissionLocked();
	const auto stKey = std::make_pair(pManagerLocal, iSessionIdLocal);
	auto it = m_mAdmissionSessions.find(stKey);
	const auto stUsage = it == m_mAdmissionSessions.end() ? AdmissionUsage{} : it->second;
	if (m_stAdmissionTotal.iCount >= m_iMaxCount ||
	    iBytesLocal > m_iMaxBytes - m_stAdmissionTotal.iBytes ||
	    stUsage.iCount >= m_iSessionMaxCount ||
	    iBytesLocal > m_iSessionMaxBytes - stUsage.iBytes)
	{
		return {};
	}
	// 所有可能分配的操作先完成，再一次性扣额度；构造或提交失败由 lease 归还。
	RpcAdmissionLease stLease;
	auto& stSession = m_mAdmissionSessions[stKey];
	stLease.stKey = stKey;
	stLease.iBytes = iBytesLocal;
	stLease.bAcquired = true;
	++stSession.iCount;
	stSession.iBytes += iBytesLocal;
	++m_stAdmissionTotal.iCount;
	m_stAdmissionTotal.iBytes += iBytesLocal;
	return stLease;
}

void RpcCallManager::ReleaseAdmission(const RpcAdmissionLease& stLease)
{
	::OI::Thread::MutexGuard stLock(m_stAdmissionLocker);
	auto it = m_mAdmissionSessions.find(stLease.stKey);
	--m_stAdmissionTotal.iCount;
	m_stAdmissionTotal.iBytes -= stLease.iBytes;
	it->second.iBytes -= stLease.iBytes;
	if (--it->second.iCount == 0)
	{
		m_mAdmissionSessions.erase(it);
	}
}

int32_t RpcCallManager::InitAdmission()
{
	try
	{
		::OI::Thread::MutexGuard stLock(m_stAdmissionLocker);
		LoadAdmissionLocked();
		return OI_OK;
	}
	catch (const std::exception& stError)
	{
		fprintf(stderr, "invalid RPC admission configuration: %s\n", stError.what());
		return OI_ERR_INVALID_ARGUMENT;
	}
}

// 与等待项共用分片锁：登记、换绑、超时、回包及断连原子维护会话索引。
std::list<uint64_t>::iterator RpcCallManager::AddSessionIndex(Shard& stShard, const Slot& stSlot, sid_t iSessionIdLocal)
{
	auto it = stShard.mSessions.try_emplace(SessionKey{stSlot.pManager, iSessionIdLocal}).first;
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

void RpcCallManager::RemoveSessionIndex(Shard& stShard, Slot& stSlot)
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

void RpcCallManager::ArmGuard(Slot& stSlot)
{
	if (!stSlot.pGuard || stSlot.bGuardArmed)
	{
		return;
	}
	stSlot.pGuard->fetch_add(1, std::memory_order_acq_rel);
	stSlot.bGuardArmed = true;
}

// 调用者持分片锁。删除空桶，避免动态超时参数让桶本身无限增长。
void RpcCallManager::RemoveExpiry(Shard& stShard, Slot& stSlot)
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

RpcCallManager::Shard& RpcCallManager::ShardOf(uint64_t iCallIdLocal)
{
	return m_vShards[iCallIdLocal % kShardCount];
}

void RpcCallManager::FinishSlot(Slot& stSlot, int32_t iCode, ::OI::Base::BytesStream* pBody)
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
	std::exception_ptr stDecodeError;
	if (pBody)
	{
		if (stSlot.fnDecode)
		{
			try
			{
				stSlot.fnDecode(*pBody, stSlot.iSessionId);
			}
			catch (...)
			{
				stDecodeError = std::current_exception();
				if (stSlot.pResult)
				{
					*stSlot.pResult = OI_ERR_FAIL;
				}
			}
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
	if (stDecodeError)
	{
		std::rethrow_exception(stDecodeError);
	}
}

bool RpcCallManager::TakeSlot(uint64_t iCallIdLocal, Slot& stOutput, ::OI::Net::SessionManager* pManagerLocal, sid_t iSessionIdLocal, proto_type_t iResponseIdLocal)
{
	Shard& stShard = ShardOf(iCallIdLocal);
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
	RemoveExpiry(stShard, stIterator->second);
	RemoveSessionIndex(stShard, stIterator->second);
	ArmGuard(stIterator->second);
	stOutput = std::move(stIterator->second);
	stShard.mSlots.erase(stIterator);
	return true;
}

void RpcCallManager::Sweep()
{
	const int64_t iNow = ::OI::Timer::SteadyMilliseconds();
	std::vector<Slot> vFired;
	try
	{
		for (auto& stShard : m_vShards)
		{
			::OI::Thread::MutexGuard stLock(stShard.stMutex);
			for (auto stBit = stShard.mExpire.begin(); stBit != stShard.mExpire.end();)
			{
				if (stBit->first > iNow / kBucketMilliseconds)
				{
					break; // 后面的整桶尚未到期，不扫描长超时请求。
				}
				std::list<Expire>& vQ = stBit->second;
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
					ArmGuard(stIterator->second);
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
	for (auto& stSlot : vFired)
	{
		// 还没连上就到期，按连接失败返回，不占超时码。
		const int32_t iCodeLocal = stSlot.bAwaitingConnect ? OI_ERR_CONNECT : OI_ERR_TIMEOUT;
		try
		{
			FinishSlot(stSlot, iCodeLocal, nullptr);
		}
		catch (...)
		{
			::OI::Thread::ReportTaskError(std::current_exception());
		}
	}
}

::OI::Coro::Task RpcCallManager::OnTickUpdate(uint64_t)
{
	Sweep();
	co_return;
}

RpcWaitStats RpcCallManager::WaitStats()
{
	RpcWaitStats stStats{};
	for (auto& stShard : m_vShards)
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

size_t RpcCallManager::SessionCalls(::OI::Net::SessionManager* pManager, sid_t iSessionId)
{
	const SessionKey stKey{pManager, iSessionId};
	size_t iCount = 0;
	for (auto& stShard : m_vShards)
	{
		::OI::Thread::MutexGuard stLock(stShard.stMutex);
		auto it = stShard.mSessions.find(stKey);
		if (it != stShard.mSessions.end())
		{
			iCount += it->second.size();
		}
	}
	return iCount;
}

bool RpcCallManager::MarkAwaitingConnect(uint64_t iCallIdLocal)
{
	Shard& stShard = ShardOf(iCallIdLocal);
	::OI::Thread::MutexGuard stLock(stShard.stMutex);
	auto stIterator = stShard.mSlots.find(iCallIdLocal);
	if (stIterator == stShard.mSlots.end())
	{
		return false;
	}
	stIterator->second.bAwaitingConnect = true;
	return true;
}

bool RpcCallManager::ClaimSend(uint64_t iCallIdLocal, sid_t iSessionIdLocal)
{
	Shard& stShard = ShardOf(iCallIdLocal);
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

uint64_t RpcCallManager::Bind(::OI::Net::SessionManager* pManagerLocal,
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
	// 最高位留给请求标记，槽里只存去掉标记的号。
	uint64_t iCallIdLocal = m_iCallSeq.fetch_add(1, std::memory_order_relaxed) & ~(uint64_t)RPC_REQ_MASK;
	Slot stSlot;
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
	Shard& stShard = ShardOf(iCallIdLocal);
	::OI::Thread::MutexGuard stLock(stShard.stMutex);
	auto stInserted = stShard.mSlots.emplace(iCallIdLocal, std::move(stSlot)).first;
	try
	{
		stInserted->second.stSessionIndex = AddSessionIndex(stShard, stInserted->second, iSessionIdLocal);
		stInserted->second.bIndexed = true;
		// 非正时长不进超时桶。生产调用在进这里之前已经换成 30 秒；测试自己结束的槽可以传 0。
		if (iTimeoutMilliseconds > 0)
		{
			const int64_t iNow = ::OI::Timer::SteadyMilliseconds();
			const int64_t iMaxTime = (std::numeric_limits<int64_t>::max)();
			const int64_t iDeadline = iTimeoutMilliseconds > iMaxTime - iNow ? iMaxTime : iNow + iTimeoutMilliseconds;
			// 当前刻度内逐项校验精确截止时间，保持原扫描精度；存刻度编号避免溢出。
			const int64_t iBucket = iDeadline / kBucketMilliseconds;
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

bool RpcCallManager::Wake(uint64_t iCallIdLocal, int32_t iCode, ::OI::Base::BytesStream* pBody)
{
	Slot stSlot;
	if (!TakeSlot(iCallIdLocal, stSlot))
	{
		return false;
	}
	FinishSlot(stSlot, iCode, pBody);
	return true;
}

void RpcCallManager::Abandon(uint64_t iCallIdLocal)
{
	Slot stSlot;
	{
		Shard& stShard = ShardOf(iCallIdLocal);
		::OI::Thread::MutexGuard stLock(stShard.stMutex);
		auto stIterator = stShard.mSlots.find(iCallIdLocal);
		if (stIterator == stShard.mSlots.end())
		{
			return;
		}
		RemoveExpiry(stShard, stIterator->second);
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

void RpcCallManager::WaitGuard(const std::shared_ptr<std::atomic<int32_t>>& pGuardLocal)
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

void RpcCallManager::FailSession(::OI::Net::SessionManager* pManagerLocal, sid_t iSessionIdLocal)
{
	static_assert(std::is_nothrow_move_assignable_v<Slot>, "FailSession moves slots into a fixed array under the shard lock");
	// 固定容量分批摘除，断连清理自身不分配内存。每批出锁后完成，异常不能遗失后续等待项。
	std::array<Slot, 32> vFailed{};
	for (auto& stShard : m_vShards)
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
					RemoveExpiry(stShard, stSlot->second);
					stSlot->second.bIndexed = false;
					ArmGuard(stSlot->second);
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
					FinishSlot(vFailed[iIndex], OI_ERR_CONNECT, nullptr);
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

bool RpcCallManager::Deliver(proto_type_t iId, ::OI::Base::BytesStream& stBodyLocal, ProtoSessionManager* pManagerLocal, sid_t iSessionIdLocal)
{
	uint64_t iRaw = 0;
	const size_t iBodyBytes = stBodyLocal.RemainSize();
	bool bIsRequest = false;
	bool bIsNotify = false;
	try
	{
		stBodyLocal >> iRaw;
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
		pHandle = pManagerLocal->FindRpcHandler(iId);
		if (pHandle == nullptr)
		{
			return false;
		}
	}
	try
	{
		if (!bIsRequest)
		{
			uint64_t iCallIdLocal = iRaw & ~(uint64_t)RPC_REQ_MASK;
			Slot stSlot;
			if (!TakeSlot(iCallIdLocal, stSlot, pManagerLocal, iSessionIdLocal, iId))
			{
				return true;
			}
			FinishSlot(stSlot, 0, &stBodyLocal);
		}
		else
		{
			auto stLease = AcquireAdmission(pManagerLocal, iSessionIdLocal, iBodyBytes);
			if (!stLease && bIsNotify)
			{
				return true;
			}
			if (!stLease)
			{
				::OI::Base::BytesStream stReply;
				stReply << (iRaw & ~(uint64_t)RPC_REQ_MASK) << int32_t(OI_ERR_BUSY);
				auto stFrame = EncodePbFrame(iId, stReply);
				if (pManagerLocal->SendFrame(iSessionIdLocal, stFrame.GetMutableBuff()) != OI_OK)
				{
					pManagerLocal->CloseSession(iSessionIdLocal);
				}
				return true;
			}
			RpcDelivery stDelivery{pManagerLocal, iSessionIdLocal, iRaw, std::move(*stLease), pManagerLocal->HoldSessionTask(iSessionIdLocal)};
			(*pHandle)(stBodyLocal, std::move(stDelivery));
		}
	}
	catch (::OI::Base::ParserException&)
	{
	}
	return true;
}

void RpcCallManager::ReportError(OI::Coro::Token iToken, uint64_t iCallIdLocal, sid_t iSessionIdLocal,
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

void RpcCallManager::SendReply(OI::Coro::Token iToken, ::OI::Net::SessionManager* pManager, sid_t iSessionId,
    uint64_t iCallId, proto_type_t iResponseId, const ::OI::Base::Bytes& stFrame)
{
	try
	{
		if (pManager->SendFrame(iSessionId, stFrame) != OI_OK)
		{
			pManager->CloseSession(iSessionId);
		}
	}
	catch (...)
	{
		ReportError(iToken, iCallId, iSessionId, iResponseId, "send_response", std::current_exception());
		OI::Coro::ReportTaskError(iToken, std::current_exception());
		pManager->CloseSession(iSessionId);
	}
}

void RpcCallManager::SendFailureReply(OI::Coro::Token iToken, ::OI::Net::SessionManager* pManager, sid_t iSessionId,
    uint64_t iCallId, proto_type_t iResponseId)
{
	::OI::Base::BytesStream stFrame;
	try
	{
		::OI::Base::BytesStream stFailure;
		stFailure << iCallId << int32_t(OI_ERR_FAIL);
		stFrame = EncodePbFrame(iResponseId, stFailure);
	}
	catch (...)
	{
		ReportError(iToken, iCallId, iSessionId, iResponseId, "encode_failure_reply", std::current_exception());
		OI::Coro::ReportTaskError(iToken, std::current_exception());
		pManager->CloseSession(iSessionId);
		return;
	}
	SendReply(iToken, pManager, iSessionId, iCallId, iResponseId, stFrame.GetMutableBuff());
}

}
}
