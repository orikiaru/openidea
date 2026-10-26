#include "ipcserver.h"
#include "allechoreq"
#include "allechores"
#include "parser.h"
#include "rpc.h"
#include "rpc_ids.h"
#include <stdio.h>

IpcServer IpcServer::inst;

IpcServer::~IpcServer()
{

}

const std::string IpcServer::GetIDString()
{
    return "IpcServer";
}

bool IpcServer::Init()
{
	HandleRpc(RpcProtoId<AllEchoReq>::value, [](SessionManager* mgr, sid_t sid, BytesStream& body) {
		AllEchoReq req;
		req.deserialize(body);
		AllEchoRes res;
		res.data = req.data;
		res.code = 1;
		BytesStream out;
		res.serialize(out);
		BytesStream frame = EncodePbFrame(RpcProtoId<AllEchoRes>::value, out);
		mgr->SendBytes(sid, frame.GetMutableBuff());
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
