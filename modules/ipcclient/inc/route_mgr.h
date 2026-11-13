#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"

class ZoneService;

class RouteMgr
{
	DECLARE_SINGLETON(RouteMgr)
	RouteMgr() = default;
	// 在 poll 上把探测服务改到另一对地址，用来模拟选路之后换区。
	OI::Coro::TaskResult<int32_t> MoveRouteProbe(ZoneService* pManagerLocal, std::string sIpLocal, std::string sPortLocal);
	// 选路之后、发送之前换区。单次和批量的目标连接保持选路当时的结果。
	OI::Coro::TaskResult<int> CheckRouteTargetStability();
	// 不同发送策略互相隔离。租约还在时不回收，释放后连接销毁。超预算回 BUSY。
	OI::Coro::TaskResult<int> CheckRoutePoolLifetime();
	// 同一条池连接断开后按需重连。出站租约释放时反向 RPC 仍挂着。
	OI::Coro::TaskResult<int> CheckRouteInboundAndReconnect();
	// 路由池满时新调用回 BUSY，腾出位置后小请求仍能完成。
	OI::Coro::TaskResult<int> CheckRoutePoolCapacity();

  public:
	// 登记反向 Hold，供入站租约那一项使用。
	static void Prepare();
	// 选路之后、发送之前换区。单次和批量的目标连接保持选路当时的结果。
	OI::Coro::TaskResult<int> RunTarget();
	// 不同发送策略互相隔离。租约还在时不回收，释放后连接销毁，下次调用重新建连。超预算回 BUSY。
	OI::Coro::TaskResult<int> RunPool();
	// 同一条池连接断开后按需重连。出站租约释放时反向 RPC 仍挂着，回包成功后租约才结束。
	OI::Coro::TaskResult<int> RunInbound();
	// 路由池满时新调用回 BUSY，腾出位置后小请求仍能完成。
	OI::Coro::TaskResult<int> RunCapacity();
};
