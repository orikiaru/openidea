#pragma once
#include "oi_registry.h"
#include "oi_rpc_coro.h"
#include "oi_session_manager.h"
#include "oi_singleton.h"
#include <map>
#include <set>
#include <string>
#include <vector>

// 注册进程。登记写进 Redis，登记的连接断了当场删掉。按 pull_ms 整量拉一次整理好，客户端来拉时直接给。
// Redis 走 RedisClient 协程，等 Redis 的时候 poll 照转。
// 所有键都在 oi:{project.env}: 下面，不同项目、不同环境互不可见。
// svc 是名字集合，svc:<name> 是这个名字的 id 集合，svc:<name>:<id> 是一份实例，值是 "ip port"。
class RegistryServer : public ::OI::Net::SessionManager
{
	DECLARE_SINGLETON(RegistryServer)
	RegistryServer();
public:
	virtual ~RegistryServer();
	bool Init();
	virtual const std::string GetIDString();
	void Handle(::OI::Net::Session* session, uint32_t id, ::OI::Base::BytesStream& body);
	void StartReload();
protected:
	virtual ::OI::Net::Session* CloneSession();
	virtual void OnAddSession(::OI::Net::Session* pSession);
	virtual void OnDelSession(const ::OI::Net::Session* pSession);
private:
	struct Binding
	{
		std::string name;
		int32_t id;
		std::string ip;
		std::string port;
		std::string value;
	};
	enum { REG_FAIL = -1, REG_TAKEN = 0, REG_OK = 1, REG_RENEWED = 2 };
	coro::Task Boot();
	coro::Task Reload();
	coro::Task Register(sid_t sid, Binding b);
	coro::Task Refresh(sid_t sid, Binding b);
	coro::Task Withdraw(Binding b);
	bool LoadSpace();
	std::string Key(const std::string& tail) const;
	std::vector<std::string> EvalArgs(const char* lua, const Binding& b) const;
	void Reply(sid_t sid, uint32_t id, const ::OI::Rpc::Serialize& msg);
	Binding* FindBinding(sid_t sid, const std::string& name, int32_t id);
	void PutSnapshot(const Binding& b);
	void DropSnapshot(const std::string& name, int32_t id);

	std::set<sid_t> _alive;
	std::map<sid_t, std::vector<Binding> > _bindings;
	std::vector<::OI::Base::ServiceInstance> _snapshot;
	bool _snapshot_ok;
	bool _reloading;
	int32_t _pull_ms;
	int32_t _expire_ms;
	std::string _space;
	std::string _prefix;
};
