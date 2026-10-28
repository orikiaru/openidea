#include "ipcserver.h"
#include "allechoreq"
#include "allechores"
#include "holdreq"
#include "holdres"
#include "parser.h"
#include "rpc.h"
#include "rpc_coro.h"
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
	HandleRpc<OI::Rpc::AllEchoReq, OI::Rpc::AllEchoRes>([](coro::Token token, SessionManager* mgr, sid_t sid, OI::Rpc::AllEchoReq& req, OI::Rpc::AllEchoRes& res) -> coro::Task {
		(void)token;
		(void)mgr;
		(void)sid;
		res.data = req.data;
		res.code = 1;
		co_return;
	});
	HandleRpc<OI::Rpc::HoldReq, OI::Rpc::HoldRes>([](coro::Token token, SessionManager* mgr, sid_t sid, OI::Rpc::HoldReq& req, OI::Rpc::HoldRes& res) -> coro::Task {
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
	return SessionManager::Init();
}

void IpcServer::OnAddSession(Session* pSession)
{
	printf("ipc server session %d\n", pSession->GetSessionID());
	fflush(stdout);
}

void IpcServer::OnDelSession(const Session* pSession)
{
	(void)pSession;
}
