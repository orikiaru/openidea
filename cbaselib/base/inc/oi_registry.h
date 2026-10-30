#pragma once
#include "oi_bytes_stream.h"
#include "oi_session_manager.h"
#include "oi_singleton.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace OI { namespace Base {

// 一份在线实例。id 取 [Service] id。
struct ServiceInstance
{
	std::string name;
	int32_t id;
	std::string ip;
	std::string port;
	ServiceInstance() : id(0) {}
};

// registry 和客户端之间的帧：PB 头里是协议号和包体长度。
::OI::Base::Bytes EncodeRegFrame(uint32_t id, const ::OI::Rpc::Serialize& msg);
// 把 in 里完整的帧逐个交给 on_frame，剩下的半帧留在 in。坏帧关掉会话，返回 false。
bool ReadRegFrames(::OI::Base::Bytes& in, ::OI::Net::Session* session, const std::function<void(uint32_t, ::OI::Base::BytesStream&)>& on_frame);

// 本进程关心的服务。一个名字下面可能有多份实例，选 id 最小的固定走，它下线了再换。
class ServiceTable
{
	DECLARE_SINGLETON(ServiceTable)
	ServiceTable();
public:
	~ServiceTable();
	void Watch(::OI::Net::SessionManager* mgr, const std::string& name);
	std::vector<std::string> Names();
	// names 是这次拉取问过的名字，list 里没有的就是下线了。
	void Apply(const std::vector<std::string>& names, const std::vector<ServiceInstance>& list);
	static bool Pick(const std::vector<ServiceInstance>& list, const std::string& name, int32_t current, ServiceInstance& out);
private:
	struct State
	{
		int32_t id;
		std::string ip;
		std::string port;
		bool online;
		std::vector<::OI::Net::SessionManager*> watchers;
		State() : id(0), online(false) {}
	};
	::OI::Thread::Mutex _mu;
	std::map<std::string, State> _services;
};

// 连 registry 的客户端。登记、心跳、拉取都在这里，应用层只在配置里写 service。
class RegistryClient : public ::OI::Net::SessionManager
{
	DECLARE_SINGLETON(RegistryClient)
	RegistryClient();
public:
	virtual ~RegistryClient();
	// 没配 [Registry] 或查不到时返回 false，调用方继续用配置里的 ip/port。
	bool Lookup(const std::string& name, std::string& ip, std::string& port);
	// StartServer 成功后登记本进程。返回 false 表示不能起：id 不对，或同名同 id 已经在线。
	bool Publish(const std::string& section, const std::string& ip, const std::string& port);
	void Watch(::OI::Net::SessionManager* mgr, const std::string& name);
	// 按 heartbeat_ms 报一次在线。没连上就去连，连上了还没登记就补登记。
	void Touch();
	// 按 pull_ms 把关心的名字整量拉一次。
	void Pull();
	virtual const std::string GetIDString();
	void OnFrame(uint32_t id, ::OI::Base::BytesStream& body);
protected:
	virtual ::OI::Net::Session* CloneSession();
	virtual void OnAddSession(::OI::Net::Session* pSession);
	virtual void OnDelSession(const ::OI::Net::Session* pSession);
	virtual void OnConnectFailed();
	bool Configured();
	bool Ensure();
	bool Connect();
	bool WaitFlag(const std::atomic<bool>& flag);
	void Notify();
	bool Send(uint32_t id, const ::OI::Rpc::Serialize& msg);
	void SendRegister();
	void ArmHeartbeat();
	void ArmPull();

	std::mutex _mu;
	std::condition_variable _cv;
	std::atomic<bool> _connected;
	std::atomic<bool> _connecting;
	std::atomic<bool> _failed;
	std::atomic<sid_t> _sid;
	bool _address_ready;

	std::string _svc_name;
	int32_t _svc_id;
	std::string _svc_ip;
	std::string _svc_port;
	std::atomic<bool> _published;
	std::atomic<bool> _registered;
	std::atomic<bool> _lost;
	int32_t _heartbeat_ms;
	bool _heartbeat_armed;
	int32_t _pull_ms;
	bool _pull_armed;

	// 回包按发出的顺序回来。同步等的那一份在 Lookup / Publish 里等。
	struct PendingPull
	{
		bool sync;
		std::vector<std::string> names;
	};
	::OI::Thread::Mutex _pending_mu;
	std::deque<PendingPull> _pending_pulls;
	std::atomic<bool> _reg_waiting;
	std::atomic<bool> _got;
	int32_t _code;
	std::vector<ServiceInstance> _res_list;
};

}}
