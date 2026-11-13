#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"

class AddressMgr
{
	DECLARE_SINGLETON(AddressMgr)
	AddressMgr() = default;
	class AddressConnectProbe;
	// 监听口用 ipc 端口加 17，避开回归脚本的端口表。
	std::string Ipv6ListenPort();
	// 字面地址当场解析，不把解析留给 StartClient。
	int CheckLiteralAddress(const std::string& sPort);
	// 等这一条地址探测的 StartClient 结束。
	OI::Coro::TaskResult<int32_t> AwaitAddressConnect(AddressConnectProbe* pProbe);
	// 域名、IPv6、方括号、失败主机，以及 64 个等待者反复开关连接。
	OI::Coro::TaskResult<int> CheckAddressConnect();

  public:
	// 字面地址、域名、IPv6 和方括号写法，失败主机，以及 64 个等待者反复开关连接。
	OI::Coro::TaskResult<int> Run();
};
