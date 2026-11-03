#pragma once
#include "oi_session_manager.h"
#include "oi_singleton.h"
#include <string>
#include <vector>

struct redisAsyncContext;

struct RedisLink
{
	redisAsyncContext* pAsyncContext;
	int32_t iIndex;
	bool bDialing;
	RedisLink() : pAsyncContext(NULL), iIndex(0), bDialing(false) {}
};

// 单线程进程。上游 hiredis 连接数走 Redis.connections。一条客户端会话固定绑一条，回包顺序才不会乱。
class RedisServer : public ::OI::Net::SessionManager
{
	DECLARE_SINGLETON(RedisServer)
	RedisServer();
	std::string m_sRedisIp;
	std::string m_sRedisPort;
	int32_t m_iConnections;
	int32_t m_iNext;
	std::vector<RedisLink> m_vLinks;
public:
	virtual ~RedisServer();
	virtual const std::string GetIDString() { return "RedisServer"; }
	virtual bool Init();
	// cmd 是一条完整的 RESP 命令，原样转给上游
	void Issue(int32_t iLinkParam, sid_t iSessionIdParam, const char* pCmdParam, size_t iLengthParam);
	void Open(RedisLink* pLinkParam);
	void ArmReconnect(RedisLink* pLinkParam);
private:
	virtual ::OI::Net::Session* CloneSession();
	virtual void OnAddSession(::OI::Net::Session* pSessionParam);
	virtual void OnDelSession(const ::OI::Net::Session* pSessionParam);
};
