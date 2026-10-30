#pragma once
#include "oi_bytes_stream.h"
#include "oi_session_manager.h"
#include "oi_singleton.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <chrono>

namespace OI { namespace Base {

// 服务表和日志落在 Redis。真正那台 Redis 的地址来自配置，redisserver 自己的监听地址也写进这张表。
class RedisStore
{
	DECLARE_SINGLETON(RedisStore)
	RedisStore();
public:
	~RedisStore();
	bool Enabled();
	bool SetService(const std::string& name, const std::string& ip, const std::string& port);
	bool GetService(const std::string& name, std::string& ip, std::string& port);
	bool DeleteService(const std::string& name);
	bool AppendLog(const void* data, size_t n);
	bool ListServices(std::map<std::string, std::pair<std::string, std::string> >& out);
private:
	bool Ensure();
	void Close();
	void* _ctx;
	::OI::Thread::Mutex _mu;
	bool _opened;
	int32_t _expire_ms;
};

// 本进程关心的服务状态。同一个名字只留一条，变了再回调 SessionManager。
class ServiceTable
{
	DECLARE_SINGLETON(ServiceTable)
	ServiceTable();
public:
	~ServiceTable();
	void Watch(::OI::Net::SessionManager* mgr, const std::string& name);
	// 同一个名字已经由别的管理器用另一对地址登记过，就保持原来的，返回 false。
	bool Publish(const std::string& name, const std::string& ip, const std::string& port);
	void Pull();
	void Touch();
private:
	void ArmPull();
	struct State
	{
		std::string ip;
		std::string port;
		bool online;
		bool published;
		std::vector<::OI::Net::SessionManager*> watchers;
		State() : online(false), published(false) {}
	};
	::OI::Thread::Mutex _mu;
	std::map<std::string, State> _services;
	bool _pull_armed;
	int32_t _pull_ms;
};

class RegistryServer : public ::OI::Net::SessionManager
{
	DECLARE_SINGLETON(RegistryServer)
	RegistryServer();
public:
	virtual ~RegistryServer();
	bool Init();
	virtual const std::string GetIDString();
	void Handle(::OI::Net::Session* session, uint32_t id, ::OI::Base::BytesStream& body);
	void Sweep();
protected:
	virtual ::OI::Net::Session* CloneSession();
	virtual void OnAddSession(::OI::Net::Session* pSession);
	virtual void OnDelSession(const ::OI::Net::Session* pSession);
	void DropSid(sid_t sid);

	struct Item
	{
		std::string ip;
		std::string port;
		sid_t sid;
		std::chrono::steady_clock::time_point seen;
	};
	::OI::Thread::Mutex _table_mu;
	std::map<std::string, Item> _table;
	int32_t _expire_ms;
	bool _sweep_armed;
};

class RegistryClient : public ::OI::Net::SessionManager
{
	DECLARE_SINGLETON(RegistryClient)
	RegistryClient();
public:
	virtual ~RegistryClient();
	// 没有 [Registry] 或查不到时返回 false，调用方继续用配置里的 ip/port。
	bool Lookup(const std::string& name, std::string& ip, std::string& port);
	// StartServer 成功后把本进程的服务名和监听地址登记上去。
	bool Publish(const std::string& section, const std::string& ip, const std::string& port);
	void Watch(::OI::Net::SessionManager* mgr, const std::string& name);
	void Pull();
	virtual const std::string GetIDString();
	void OnFrame(uint32_t id, ::OI::Base::BytesStream& body);
	void Touch();
protected:
	virtual ::OI::Net::Session* CloneSession();
	virtual void OnAddSession(::OI::Net::Session* pSession);
	virtual void OnDelSession(const ::OI::Net::Session* pSession);
	virtual void OnConnectFailed();
	bool Configured();
	bool Ensure();
	bool WaitFlag(const std::atomic<bool>& flag);
	void Notify();
	void SendRegister();
	void SendHeartbeat();
	void ArmHeartbeat();

	std::mutex _mu;
	std::condition_variable _cv;
	std::atomic<bool> _connected;
	std::atomic<bool> _failed;
	sid_t _sid;
	bool _address_ready;
	bool _connecting;
	std::string _svc_name;
	std::string _svc_ip;
	std::string _svc_port;
	bool _republish;
	int32_t _heartbeat_ms;
	bool _heartbeat_armed;
	struct WatchItem
	{
		::OI::Net::SessionManager* mgr;
		std::string name;
	};
	std::vector<WatchItem> _watches;
	bool _pull_armed;
	int32_t _pull_ms;
	uint32_t _expect;
	std::atomic<bool> _got;
	int32_t _code;
	std::string _res_ip;
	std::string _res_port;
};

}}
