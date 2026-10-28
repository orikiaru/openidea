#pragma once
#include "oi_session_manager.h"
#include "oi_singleton.h"
#include <string>
#include <vector>

struct redisAsyncContext;

struct RedisLink
{
	redisAsyncContext* ac;
	int32_t index;
	bool dialing;
	RedisLink() : ac(NULL), index(0), dialing(false) {}
};

// 单线程进程。上游 hiredis 连接数走 Redis.connections。一条客户端会话固定绑一条，回包顺序才不会乱。
class RedisServer : public SessionManager
{
	DECLARE_SINGLETON(RedisServer)
	RedisServer();
	std::string _redis_ip;
	std::string _redis_port;
	int32_t _connections;
	int32_t _next;
	std::vector<RedisLink> _links;
public:
	virtual ~RedisServer();
	virtual const std::string GetIDString() { return "RedisServer"; }
	virtual bool Init();
	// cmd 是一条完整的 RESP 命令，原样转给上游
	void Issue(int32_t link, sid_t sid, const char* cmd, size_t len);
	void Open(RedisLink* link);
	void ArmReconnect(RedisLink* link);
private:
	virtual Session* CloneSession();
	virtual void OnAddSession(Session* pSession);
	virtual void OnDelSession(const Session* pSession);
};
