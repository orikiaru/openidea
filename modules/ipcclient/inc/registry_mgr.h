#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"

class RegistryMgr
{
	DECLARE_SINGLETON(RegistryMgr)
	RegistryMgr() = default;
	// 通过故障文件让 registry 暂停、恢复或收紧预算，并等它确认。
	OI::Coro::TaskResult<bool> RegistryFaultHandshake(std::string sPath, std::string sCommand);
	// 暂停制造忙和超时，恢复后回包仍配对。非法分区不改变结果条数。查询加上处理可以超过端到端预算。
	OI::Coro::TaskResult<int> CheckRegistryBoundsAndBudget();
	// 拉取和查找交错，地址和端口都要对上。
	OI::Coro::TaskResult<int> RegistryLookups();
	// 8 路同时做上面的查找。
	OI::Coro::TaskResult<int> CheckRegistryPairing();

  public:
	// 8 路并发查 ipcserver，地址和端口都对上。
	OI::Coro::TaskResult<int> RunPairing();
	// 暂停 registry 制造忙和超时，恢复后回包仍配对。批量里夹非法分区时结果条数不变。查询加上处理可以超过端到端预算。
	OI::Coro::TaskResult<int> RunBounds();
};
