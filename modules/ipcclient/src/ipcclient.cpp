#include "ipcclient.h"
#include "echo_mgr.h"
#include "hold_mgr.h"
#include "log_mgr.h"
#include "multi_mgr.h"
#include "redis_mgr.h"
#include "udp_mgr.h"
#include "oi_redis.h"

IpcClient::~IpcClient()
{
}

bool IpcClient::Init()
{
	if (!::OI::Rpc::ProtoSessionManager::Init())
	{
		return false;
	}
	EchoMgr::GetInstance().Load();
	if (EchoMgr::GetInstance().StackOn())
	{
		OI::Coro::StartTask([](OI::Coro::Token)
		{
			return EchoMgr::GetInstance().StackProbe();
		});
	}
	if (!UdpMgr::GetInstance().Load())
	{
		return false;
	}
	UdpMgr::GetInstance().Start();
	if (!MultiMgr::GetInstance().Load())
	{
		return false;
	}
	MultiMgr::GetInstance().Start();
	RedisMgr::GetInstance().Load();
	HoldMgr::GetInstance().Load();
	if (!RedisMgr::GetInstance().Enabled() && !HoldMgr::GetInstance().Enabled())
	{
		return true;
	}
	if (!::OI::Redis::RedisClient::GetInstance().Init())
	{
		return false;
	}
	::OI::Redis::RedisClient::GetInstance().StartClient();
	if (RedisMgr::GetInstance().Enabled())
	{
		RedisMgr::GetInstance().Start();
	}
	return true;
}

const std::string IpcClient::GetIDString()
{
	return "IpcClient";
}

void IpcClient::OnClientConnect(::OI::Net::Session* pSession)
{
	// 服务发现切换或重连可能建立多个会话；每个进程只启动一次测试。
	if (m_bTestsStarted.exchange(true))
	{
		return;
	}
	EchoMgr::GetInstance().Load();
	LogMgr::GetInstance().Load();
	EchoMgr::GetInstance().Start(pSession->GetSessionID());
	LogMgr::GetInstance().Start();
	HoldMgr::GetInstance().Start(pSession->GetSessionID());
}

void IpcClient::OnClientDisconnect(const ::OI::Net::Session* pSession)
{
	(void)pSession;
}
