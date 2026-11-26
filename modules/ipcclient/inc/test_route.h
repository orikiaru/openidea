#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"

// 路由连接常驻在 RouteManager，参数取配置段 RpcRoute。
class TestRoute
{
	DECLARE_SINGLETON(TestRoute)
	TestRoute() = default;
	// 同一分区始终是同一个发送目标，刷新之后会话号也不变，回包标记对得上。
	OI::Coro::TaskResult<int> CheckRouteTargetStability();
	// 已连接发送和断线后暂存再补发，超过发送预算都回 BUSY，背压不换会话。
	OI::Coro::TaskResult<int> CheckRouteBackpressure();
	// 关掉当前会话后不让出，立刻向同一个发送目标再发，必须排队重连并成功。
	OI::Coro::TaskResult<int> CheckRouteReconnect();

  public:
	// 选路结果在一次秒更新之后仍指向同一条连接。
	OI::Coro::TaskResult<int> RunTarget();
	// 路由连接上的发送预算。
	OI::Coro::TaskResult<int> RunPool();
	// 同一条路由连接断开后重连。
	OI::Coro::TaskResult<int> RunInbound();
};
