#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"
#include <cstddef>
#include <cstdint>

class HttpMgr
{
	DECLARE_SINGLETON(HttpMgr)
	HttpMgr() = default;
	// 分帧、中间响应、204、分块和非法长度。坏响应失败，好响应的状态和正文对上。
	OI::Coro::TaskResult<int> CheckHttpFraming(int32_t iHttpPort);
	// 不复用时每条请求都重新连。同一条连接上的后续请求会被服务端标成 reused。
	OI::Coro::TaskResult<int> CheckHttpNoReuse(int32_t iHttpPort);
	// 当前进程已分配的字节，用来看大响应释放后堆有没有留下。
	size_t AllocatedBytes();
	// 长超时期间连续成功的大响应，完成后收包容量要放掉。
	OI::Coro::TaskResult<int> CheckHttpRelease(int32_t iHttpPort);

  public:
	// 分帧、中间响应、204、连接不复用，以及大响应释放后的堆增量。调用方在端口大于 0 时才跑。
	OI::Coro::TaskResult<int> Run(int32_t iHttpPort);
};
