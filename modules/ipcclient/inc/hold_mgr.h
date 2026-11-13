#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"
#include <cstdint>

// 超时、回包撞车和 Redis 迟到回包。其余检查在各自的 mgr，Pump 按原来的顺序调用。
class HoldMgr
{
	DECLARE_SINGLETON(HoldMgr)
	HoldMgr();
	// 单次 Hold 的结局：标记对上的回包、超时，或两者都不是。
	enum class RaceOutcome
	{
		Reply,
		Timeout,
		Bad,
	};
	// 发一次 Hold，按回包标记或超时归成上面三种结局。
	OI::Coro::TaskResult<RaceOutcome> RaceOne(int32_t iMarkLocal, int32_t iDelay);
	// 成功回包、业务错误不改已有输出、缺状态码和截断响应回 FAIL，值任务的结果和异常都能收回。
	OI::Coro::TaskResult<int> CheckStandardizedReply();
	// 超时后迟到的回包只能丢弃，不能配给后面的调用。
	OI::Coro::TaskResult<int> CheckRpcLateReply();
	// 服务端延迟落在客户端超时前后，每个调用只能拿到回包或超时之一。
	OI::Coro::TaskResult<int> CheckRpcRace();
	// 发一次 PING，确认前面的超时没有把 Redis 连接打坏。
	OI::Coro::TaskResult<int> CheckRedisPing(const char* pLabel);
	// Redis 超时后迟到的回复不能串给下一条命令；回复与超时同时到也只能取其一。
	OI::Coro::TaskResult<int> CheckRedisLateReply(int32_t iRedisTimeoutMilliseconds);
	// 先挂起再断开，让断连和已经发出的 RPC 叠在一起。
	OI::Coro::Task CloseSessionLater(sid_t iSessionId);
	// 真断开当前会话：已发出的 RPC 应及时失败，不能继续等十秒超时。
	OI::Coro::TaskResult<int> CheckRpcDisconnect(sid_t iSessionId);
	bool m_bEnabled;
	sid_t m_iSessionId;
	int32_t m_iRedisTimeoutMilliseconds;

  public:
	// 读 Test.hold 开关，并登记反向 Hold。
	void Load();
	bool Enabled() const
	{
		return m_bEnabled;
	}
	// 记下会话并拉起 Pump。
	void Start(sid_t iSessionId);
	// 按固定顺序跑各主题 mgr，再测标准回包、迟到回包、回包撞车、Redis 迟到 PING 和断连收尾。
	OI::Coro::Task Pump();
};
