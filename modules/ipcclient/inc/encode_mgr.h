#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"

class EncodeMgr
{
	DECLARE_SINGLETON(EncodeMgr)
	EncodeMgr() = default;
	// 服务端编码失败和损坏的响应体回 FAIL，不覆盖已有输出。诊断文本要转义并截断。
	OI::Coro::TaskResult<int> CheckServerEncoding();

  public:
	// 服务端编码失败和损坏的响应体回 FAIL，不覆盖已有输出。同时核诊断文本的转义和截断。
	OI::Coro::TaskResult<int> Run();
};
