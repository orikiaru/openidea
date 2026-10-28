#include "ipcserver.h"
#include "oi_rpcdata_allechoreq.h"
#include "oi_rpcdata_allechores.h"
#include "oi_rpcdata_holdreq.h"
#include "oi_rpcdata_holdres.h"
#include "oi_parser.h"
#include "oi_rpc_call.h"
#include "oi_rpc_coro.h"
#include "rpc_ids.h"
#include <stdio.h>

IpcServer::~IpcServer()
{

}

const std::string IpcServer::GetIDString()
{
    return "IpcServer";
}

bool IpcServer::Init()
{
	::OI::Rpc::HandleRpc<::OI::RpcData::AllEchoReq, ::OI::RpcData::AllEchoRes>([](coro::Token token, ::OI::Net::SessionManager* mgr, sid_t sid, ::OI::RpcData::AllEchoReq& req, ::OI::RpcData::AllEchoRes& res) -> coro::Task {
		(void)token;
		(void)mgr;
		(void)sid;
		res.data = req.data;
		res.code = 1;
		co_return;
	});
	::OI::Rpc::HandleRpc<::OI::RpcData::HoldReq, ::OI::RpcData::HoldRes>([](coro::Token token, ::OI::Net::SessionManager* mgr, sid_t sid, ::OI::RpcData::HoldReq& req, ::OI::RpcData::HoldRes& res) -> coro::Task {
		(void)token;
		(void)mgr;
		(void)sid;
		if (req.delay_ms > 0)
		{
			co_await coro::yield(req.delay_ms);
		}
		res.mark = req.mark;
		co_return;
	});
	return ::OI::Net::SessionManager::Init();
}

void IpcServer::OnAddSession(::OI::Net::Session* pSession)
{
	printf("ipc server session %d\n", pSession->GetSessionID());
	fflush(stdout);
}

void IpcServer::OnDelSession(const ::OI::Net::Session* pSession)
{
	(void)pSession;
}
