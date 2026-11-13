#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"

class ConnectMgr
{
	DECLARE_SINGLETON(ConnectMgr)
	ConnectMgr() = default;
	class ClientConnectProbe;
	// 等这一条探测连接的 StartClient 结束。
	OI::Coro::TaskResult<int> AwaitClientConnect(ClientConnectProbe* pProbe);
	// 协程建连、拒绝，32 个等待者共用一条连接，回调里重入时新一代收不到旧连接的成功。
	OI::Coro::TaskResult<int> CheckClientConnect();

  public:
	// 协程建连、拒绝，以及 32 个等待者共用一条连接。回调里再次 StartClient 时，新一代收不到旧连接的成功。
	OI::Coro::TaskResult<int> Run();
};
