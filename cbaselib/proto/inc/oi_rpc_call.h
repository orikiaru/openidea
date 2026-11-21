#pragma once

#include "oi_bytes_stream.h"
#include "oi_session_manager.h"
#include "oi_singleton.h"
#include "oi_lock.h"
#include <array>
#include <atomic>
#include <coroutine>
#include <exception>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
namespace OI
{
namespace Rpc
{
class ProtoSessionManager;

// 请求的提交目标。应用客户端和路由连接共用发送/撤销逻辑，连接不再冒充管理器。
struct RpcSender
{
	// 这条调用所属的会话管理器。
	::OI::Net::SessionManager* pManager = nullptr;
	// fnSubmit / fnDrop 的第一个参数，一般是管理器或路由自己。
	void* pContext = nullptr;
	// 连接就绪后把调用号和包体发出去。
	void (*fnSubmit)(void*, uint64_t, const ::OI::Base::Bytes&) = nullptr;
	// 调用取消或失败时丢掉还没发出的那一份。
	void (*fnDrop)(void*, uint64_t) = nullptr;
	::OI::Net::SessionManager& Manager() const
	{
		return *pManager;
	}
	void SubmitCall(uint64_t iCallId, const ::OI::Base::Bytes& stData)
	{
		fnSubmit(pContext, iCallId, stData);
	}
	void DropPendingCall(uint64_t iCallId)
	{
		fnDrop(pContext, iCallId);
	}
};

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
	~RpcAdmissionLease();
};

// 处理函数拿到的游标已经越过调用号；额度和会话租约随根协程闭包存活。
struct RpcDelivery
{
	::OI::Net::SessionManager* pManager = nullptr;
	sid_t iSessionId = INVALID_SID;
	uint64_t iRawCallId = 0;
	RpcAdmissionLease stLease;
	std::shared_ptr<void> pOwner;
};
using RpcHandleFn = std::function<void(::OI::Base::BytesStream&, RpcDelivery&&)>;

// 调用方给出的超时不是正数时，打错误日志并改用 30 秒。省略参数仍走各接口自己的默认 180 秒。
constexpr int64_t kFallbackRpcTimeoutMilliseconds = 30000;
int64_t NormalizeRpcTimeoutMilliseconds(int64_t iTimeoutMilliseconds);

// 分片逐个取快照，用于诊断；有并发请求时不保证全局同一时刻的一致性。
struct RpcWaitStats
{
	// 还在等回包的调用数。
	size_t iCalls = 0;
	// 挂在超时链上的节点数。
	size_t iExpirations = 0;
	// 超时分桶里非空的桶数。
	size_t iBuckets = 0;
	// 按会话索引的调用数。
	size_t iSessionEntries = 0;
	// 有在途调用的会话数。
	size_t iSessionBuckets = 0;
};
// 进程里所有在途 RPC 调用的等待表和请求额度。调用号按分片加锁，任意线程可调；超时由每跳的 OnTickUpdate 扫。
class RpcCallManager
{
	DECLARE_SINGLETON(RpcCallManager)

  public:
	// 返回调用号。回包、超时或 Wake 三者只有一个会触发 decode/resume。
	// 回包回调携带实际匹配的 sid，日志不得用可能已重连的当前会话替代来源。
	uint64_t Bind(::OI::Net::SessionManager* pManager,
	    sid_t iSessionId,
	    proto_type_t iResponseId,
	    std::coroutine_handle<> stHandle,
	    int32_t* pResult,
	    int64_t iTimeoutMilliseconds,
	    std::function<bool(::OI::Base::BytesStream&, sid_t)> fnDecode,
	    std::function<void()> fnResume,
	    std::shared_ptr<std::atomic<int32_t>> pGuard = {},
	    RpcSender* pSender = nullptr);
	bool Wake(uint64_t iCallId, int32_t iCode, ::OI::Base::BytesStream* pBody);
	// 取走等待项且不调用完成回调。已经在回调里的，等 WaitGuard 归零。
	void Abandon(uint64_t iCallId);
	static void WaitGuard(const std::shared_ptr<std::atomic<int32_t>>& pGuard);
	// 这条会话上的在途调用全部回 OI_ERR_CONNECT。
	void FailSession(::OI::Net::SessionManager* pManager, sid_t iSessionId);
	// 连接还没好，这条调用先别发。调用号已经不在表里返回 false。
	bool MarkAwaitingConnect(uint64_t iCallId);
	// 连接好了，把调用记到这个会话上，并清掉等待连接标记。调用号不在返回 false。
	bool ClaimSend(uint64_t iCallId, sid_t iSessionId);
	// 回包按调用号交给 Bind 登记的等待项，请求交给 pManager 自己处理表里该协议号的处理协程。都没有就把包丢掉。
	bool Deliver(proto_type_t iId, ::OI::Base::BytesStream& stBody, ProtoSessionManager* pManager, sid_t iSessionId);
	// 各分片在途调用的合计。并发请求下不是同一时刻的快照。
	RpcWaitStats WaitStats();
	// 这条会话上已发出、还在等回包的调用数。等连接的不算。
	size_t SessionCalls(::OI::Net::SessionManager* pManager, sid_t iSessionId);
	// 读取进程级 RPC 在途限制；非法配置令模块初始化失败。
	int32_t InitAdmission();
	// RpcAdmissionLease 析构时归还额度。
	void ReleaseAdmission(const RpcAdmissionLease& stLease);
	// 记录失败阶段和调用关联信息；日志失败不能打断原来的错误回执/收尾。
	static void ReportError(::OI::Coro::Token iToken, uint64_t iCallId, sid_t iSessionId,
	    proto_type_t iProtocolId, const char* pStage, std::exception_ptr stError) noexcept;
	// 发出已编码的回包。发送失败或抛异常都关这条会话。
	static void SendReply(::OI::Coro::Token iToken, ::OI::Net::SessionManager* pManager, sid_t iSessionId,
	    uint64_t iCallId, proto_type_t iResponseId, const ::OI::Base::Bytes& stFrame);
	// 只带调用号和 OI_ERR_FAIL 的回执。连这个都编不出来就关会话。
	static void SendFailureReply(::OI::Coro::Token iToken, ::OI::Net::SessionManager* pManager, sid_t iSessionId,
	    uint64_t iCallId, proto_type_t iResponseId);
	// 每跳扫一遍到期的调用。
	::OI::Coro::Task OnTickUpdate(uint64_t iNowMilliseconds);

  private:
	RpcCallManager() = default;

	using SessionKey = std::pair<::OI::Net::SessionManager*, sid_t>;
	struct Expire
	{
		int64_t iDeadlineMilliseconds;
		uint64_t iCallId;
	};
	using ExpireBuckets = std::map<int64_t, std::list<Expire>>;
	struct Slot
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
		ExpireBuckets::iterator stExpiryBucket;
		std::list<Expire>::iterator stExpiry;
	};
	// 按绝对截止时间所在刻度分桶；槽持索引，完成时 O(1) 摘除节点。
	struct Shard
	{
		::OI::Thread::Mutex stMutex;
		std::unordered_map<uint64_t, Slot> mSlots;
		ExpireBuckets mExpire;
		std::map<SessionKey, std::list<uint64_t>> mSessions;
	};
	struct AdmissionUsage
	{
		size_t iCount = 0;
		size_t iBytes = 0;
	};
	static constexpr uint64_t kShardCount = 64;
	static constexpr int64_t kBucketMilliseconds = 200;

	Shard& ShardOf(uint64_t iCallId);
	static std::list<uint64_t>::iterator AddSessionIndex(Shard& stShard, const Slot& stSlot, sid_t iSessionId);
	static void RemoveSessionIndex(Shard& stShard, Slot& stSlot);
	static void ArmGuard(Slot& stSlot);
	static void RemoveExpiry(Shard& stShard, Slot& stSlot);
	static void FinishSlot(Slot& stSlot, int32_t iCode, ::OI::Base::BytesStream* pBody);
	bool TakeSlot(uint64_t iCallId, Slot& stOutput, ::OI::Net::SessionManager* pManager = nullptr, sid_t iSessionId = 0, proto_type_t iResponseId = 0);
	void Sweep();
	// 调用者持 m_stAdmissionLocker。第一次用到时读配置，非法抛 invalid_argument，下次再读。
	void LoadAdmissionLocked();
	std::optional<RpcAdmissionLease> AcquireAdmission(::OI::Net::SessionManager* pManager, sid_t iSessionId, size_t iBytes);

	std::array<Shard, kShardCount> m_vShards;
	std::atomic<uint64_t> m_iCallSeq{1};
	::OI::Thread::Mutex m_stAdmissionLocker;
	bool m_bAdmissionLoaded = false;
	AdmissionUsage m_stAdmissionTotal;
	std::map<SessionKey, AdmissionUsage> m_mAdmissionSessions;
	size_t m_iMaxCount = 65536;
	size_t m_iMaxBytes = 64 * 1024 * 1024;
	size_t m_iSessionMaxCount = 4096;
	size_t m_iSessionMaxBytes = 16 * 1024 * 1024;
};

}
}
