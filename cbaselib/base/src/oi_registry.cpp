#include "oi_registry.h"
#include "oi_proto_id.h"
#include "oi_rpcdata_regregister.h"
#include "oi_rpcdata_regregisterres.h"
#include "oi_rpcdata_regheartbeat.h"
#include "oi_rpcdata_regheartbeatres.h"
#include "oi_rpcdata_reginstance.h"
#include "oi_rpcdata_regpull.h"
#include "oi_rpcdata_regpullres.h"
#include "oi_net_session.h"
#include "oi_config.h"
#include "oi_log.h"
#include "oi_poll.h"
#include "oi_timer_manager.h"
#include <chrono>
#include <cstdio>

namespace OI { namespace Base {

namespace {
const int kWaitMs = 1000;

class RegistryClientSession : public ::OI::Net::NetSession
{
public:
	explicit RegistryClientSession(::OI::Net::SessionManager* mgr) : ::OI::Net::NetSession(mgr) {}
	virtual void Send(const ::OI::Base::Bytes& bytes)
	{
		::OI::Thread::MutexGuard lock(_locker);
		_obuff.Append(bytes);
		AllowSend();
	}
	virtual void OnDataOut() {}
	virtual void OnDataIn()
	{
		ReadRegFrames(_ibuff, this, [](uint32_t id, ::OI::Base::BytesStream& body) {
			RegistryClient::GetInstance().OnFrame(id, body);
		});
	}
};

class HeartbeatTimer : public ::OI::Timer::BaseTimer
{
public:
	bool OnTimer()
	{
		RegistryClient::GetInstance().Touch();
		return true;
	}
};

class PullTimer : public ::OI::Timer::BaseTimer
{
public:
	bool OnTimer()
	{
		RegistryClient::GetInstance().Pull();
		return true;
	}
};
}

::OI::Base::Bytes EncodeRegFrame(uint32_t id, const ::OI::Rpc::Serialize& msg)
{
	::OI::Base::BytesStream body;
	msg.serialize(body);
	::OI::Base::BytesStream frame;
	uint32_t size = (uint32_t)body.Size();
	frame.WritePbRaw(id, size ? body.Begin() : NULL, size);
	return frame;
}

bool ReadRegFrames(::OI::Base::Bytes& in, ::OI::Net::Session* session, const std::function<void(uint32_t, ::OI::Base::BytesStream&)>& on_frame)
{
	::OI::Base::BytesStream os;
	os.Swap(in);
	bool closed = false;
	while (os.RemainSize() && !closed)
	{
		os.StartTransaction();
		uint32_t id = 0;
		uint32_t size = 0;
		::OI::Base::PbHeaderState st = os.PreparePbHeader(id, size);
		if (st == ::OI::Base::PB_HEADER_NEED_MORE)
		{
			os.RollBack();
			break;
		}
		if (st != ::OI::Base::PB_HEADER_OK)
		{
			session->PassiveClose(PARSER_ERR_OTHER);
			closed = true;
			break;
		}
		::OI::Base::BytesStream body;
		if (size > 0)
		{
			body.Append(os.GetCurPtr(), size);
		}
		os.AddCurPos(size);
		try
		{
			on_frame(id, body);
		}
		catch (::OI::Base::ParserException&)
		{
			session->PassiveClose(PARSER_ERR_PROTOCOL_SIZE);
			closed = true;
		}
	}
	if (!closed)
	{
		in.Swap(os);
		in.Erase(0, os.GetCurPos());
	}
	return !closed;
}

// ---------------------------------------------------------------- ServiceTable

ServiceTable::ServiceTable() {}
ServiceTable::~ServiceTable() {}

bool ServiceTable::Pick(const std::vector<ServiceInstance>& list, const std::string& name, int32_t current, ServiceInstance& out)
{
	const ServiceInstance* best = NULL;
	for (size_t i = 0; i < list.size(); ++i)
	{
		if (list[i].name != name)
		{
			continue;
		}
		if (current > 0 && list[i].id == current)
		{
			out = list[i];
			return true;
		}
		if (!best || list[i].id < best->id)
		{
			best = &list[i];
		}
	}
	if (!best)
	{
		return false;
	}
	out = *best;
	return true;
}

void ServiceTable::Watch(::OI::Net::SessionManager* mgr, const std::string& name)
{
	::OI::Thread::MutexGuard lock(_mu);
	State& state = _services[name];
	for (size_t i = 0; i < state.watchers.size(); ++i)
	{
		if (state.watchers[i] == mgr)
		{
			return;
		}
	}
	state.watchers.push_back(mgr);
}

std::vector<std::string> ServiceTable::Names()
{
	std::vector<std::string> names;
	::OI::Thread::MutexGuard lock(_mu);
	for (std::map<std::string, State>::iterator it = _services.begin(); it != _services.end(); ++it)
	{
		if (!it->second.watchers.empty())
		{
			names.push_back(it->first);
		}
	}
	return names;
}

void ServiceTable::Apply(const std::vector<std::string>& names, const std::vector<ServiceInstance>& list)
{
	struct Change
	{
		::OI::Net::SessionManager* mgr;
		std::string ip;
		std::string port;
		bool online;
	};
	std::vector<Change> changed;
	{
		::OI::Thread::MutexGuard lock(_mu);
		for (size_t n = 0; n < names.size(); ++n)
		{
			std::map<std::string, State>::iterator it = _services.find(names[n]);
			if (it == _services.end())
			{
				continue;
			}
			State& state = it->second;
			ServiceInstance pick;
			bool online = Pick(list, names[n], state.online ? state.id : 0, pick);
			if (online == state.online && (!online || (pick.id == state.id && pick.ip == state.ip && pick.port == state.port)))
			{
				continue;
			}
			state.online = online;
			state.id = online ? pick.id : 0;
			state.ip = online ? pick.ip : "";
			state.port = online ? pick.port : "";
			for (size_t i = 0; i < state.watchers.size(); ++i)
			{
				Change c;
				c.mgr = state.watchers[i];
				c.ip = state.ip;
				c.port = state.port;
				c.online = online;
				changed.push_back(c);
			}
		}
	}
	for (size_t i = 0; i < changed.size(); ++i)
	{
		changed[i].mgr->ApplyService(changed[i].ip, changed[i].port, changed[i].online);
	}
}

// ---------------------------------------------------------------- RegistryClient

RegistryClient::RegistryClient()
	: _connected(false), _connecting(false), _failed(false), _sid(INVALID_SID), _address_ready(false),
	_svc_id(0), _published(false), _registered(false), _lost(false),
	_heartbeat_ms(1000), _heartbeat_armed(false), _pull_ms(1000), _pull_armed(false),
	_reg_waiting(false), _got(false), _code(OI_ERR_FAIL)
{
}

RegistryClient::~RegistryClient() {}

const std::string RegistryClient::GetIDString()
{
	return "RegistryClient";
}

::OI::Net::Session* RegistryClient::CloneSession()
{
	return new RegistryClientSession(this);
}

bool RegistryClient::Configured()
{
	std::string ip;
	return ::OI::Base::Config::GlobalConfig().GetAs("Registry.ip", ip) && !ip.empty();
}

void RegistryClient::Notify()
{
	_cv.notify_all();
}

bool RegistryClient::WaitFlag(const std::atomic<bool>& flag)
{
	std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
#ifdef _SINGLE_THREAD_
	while (!flag.load() && !_failed.load() && std::chrono::steady_clock::now() < deadline)
	{
		::OI::Net::Poll::GetInstance().PollWait(1);
	}
	return flag.load();
#else
	std::unique_lock<std::mutex> lock(_mu);
	_cv.wait_until(lock, deadline, [&]() { return flag.load() || _failed.load(); });
	return flag.load();
#endif
}

bool RegistryClient::Connect()
{
	if (!_address_ready)
	{
		std::string ip;
		std::string port;
		std::string type;
		if (!::OI::Base::Config::GlobalConfig().GetAs("Registry.ip", ip)) return false;
		if (!::OI::Base::Config::GlobalConfig().GetAs("Registry.port", port)) return false;
		if (!::OI::Base::Config::GlobalConfig().GetAs("Registry.type", type)) return false;
		if (ip.empty() || port.empty()) return false;
		DefaultInit(ip, port, GetSessionTypeByString(type));
		_address_ready = true;
	}
	bool expected = false;
	if (!_connecting.compare_exchange_strong(expected, true))
	{
		return true;
	}
	_failed = false;
	if (!StartClient())
	{
		_connecting = false;
		return false;
	}
	return true;
}

bool RegistryClient::Ensure()
{
	if (_connected.load())
	{
		return true;
	}
	if (!Connect())
	{
		return false;
	}
	return _connected.load() || WaitFlag(_connected);
}

bool RegistryClient::Send(uint32_t id, const ::OI::Rpc::Serialize& msg)
{
	sid_t sid = _sid.load();
	if (sid == INVALID_SID)
	{
		return false;
	}
	return SendBytes(sid, EncodeRegFrame(id, msg));
}

void RegistryClient::SendRegister()
{
	::OI::RpcData::RegRegister req(_svc_name, _svc_id, _svc_ip, _svc_port);
	Send(PROTO_REG_REGISTER, req);
}

void RegistryClient::OnAddSession(::OI::Net::Session* pSession)
{
	_sid = pSession->GetSessionID();
	_failed = false;
	_connected = true;
	_connecting = false;
	Notify();
	if (_published.load() && !_lost.load())
	{
		SendRegister();
	}
	LOG_INFO("RegistryClient::OnAddSession sid=%d", pSession->GetSessionID());
}

void RegistryClient::OnDelSession(const ::OI::Net::Session* pSession)
{
	_sid = INVALID_SID;
	_connected = false;
	_registered = false;
	_failed = true;
	{
		::OI::Thread::MutexGuard lock(_pending_mu);
		_pending_pulls.clear();
	}
	Notify();
	LOG_INFO("RegistryClient::OnDelSession sid=%d", pSession->GetSessionID());
}

void RegistryClient::OnConnectFailed()
{
	_connecting = false;
	_failed = true;
	Notify();
	LOG_TRACE("RegistryClient::OnConnectFailed");
}

void RegistryClient::OnFrame(uint32_t id, ::OI::Base::BytesStream& body)
{
	if (id == PROTO_REG_REGISTER_RES)
	{
		::OI::RpcData::RegRegisterRes res;
		res.code = OI_ERR_FAIL;
		res.deserialize(body);
		if (res.code == OI_OK)
		{
			_registered = true;
		}
		else if (res.code == OI_ERR_EXIST && !_reg_waiting.load())
		{
			_lost = true;
			fprintf(stderr, "registry %s id=%d 已被别的进程占用，停止登记\n", _svc_name.c_str(), _svc_id);
			fflush(stderr);
			LOG_ERROR("registry %s id=%d taken by another process, stop register", _svc_name.c_str(), _svc_id);
		}
		if (_reg_waiting.load())
		{
			_code = res.code;
			_got = true;
			Notify();
		}
		return;
	}
	if (id == PROTO_REG_HEARTBEAT_RES)
	{
		::OI::RpcData::RegHeartbeatRes res;
		res.code = OI_ERR_FAIL;
		res.deserialize(body);
		if (res.code == OI_ERR_EXIST)
		{
			_lost = true;
			_registered = false;
			fprintf(stderr, "registry %s id=%d 已被别的进程占用，停止登记\n", _svc_name.c_str(), _svc_id);
			fflush(stderr);
			LOG_ERROR("registry %s id=%d taken by another process, stop heartbeat", _svc_name.c_str(), _svc_id);
		}
		else if (res.code != OI_OK)
		{
			// registry 重启过，或 Redis 暂时不可用。下一次心跳重新登记。
			_registered = false;
		}
		return;
	}
	if (id == PROTO_REG_PULL_RES)
	{
		::OI::RpcData::RegPullRes res;
		res.code = OI_ERR_FAIL;
		res.deserialize(body);
		PendingPull pending;
		pending.sync = false;
		{
			::OI::Thread::MutexGuard lock(_pending_mu);
			if (_pending_pulls.empty())
			{
				return;
			}
			pending = _pending_pulls.front();
			_pending_pulls.pop_front();
		}
		std::vector<ServiceInstance> list;
		for (size_t i = 0; i < res.list.size(); ++i)
		{
			ServiceInstance inst;
			inst.name = res.list[i].name;
			inst.id = res.list[i].id;
			inst.ip = res.list[i].ip;
			inst.port = res.list[i].port;
			list.push_back(inst);
		}
		if (pending.sync)
		{
			_res_list.swap(list);
			_code = res.code;
			_got = true;
			Notify();
			return;
		}
		// registry 自己连不上 Redis 时不知道真实情况，保持原状。
		if (res.code == OI_OK)
		{
			ServiceTable::GetInstance().Apply(pending.names, list);
		}
		return;
	}
}

bool RegistryClient::Lookup(const std::string& name, std::string& ip, std::string& port)
{
	if (name.empty() || !Configured())
	{
		return false;
	}
	if (!Ensure())
	{
		LOG_TRACE("registry lookup %s connect failed", name.c_str());
		return false;
	}
	PendingPull pending;
	pending.sync = true;
	pending.names.push_back(name);
	_got = false;
	_code = OI_ERR_FAIL;
	{
		::OI::Thread::MutexGuard lock(_pending_mu);
		_pending_pulls.push_back(pending);
	}
	::OI::RpcData::RegPull req(pending.names);
	if (!Send(PROTO_REG_PULL, req) || !WaitFlag(_got))
	{
		LOG_TRACE("registry lookup %s timeout", name.c_str());
		return false;
	}
	ServiceInstance pick;
	if (_code != OI_OK || !ServiceTable::Pick(_res_list, name, 0, pick))
	{
		LOG_TRACE("registry lookup %s miss", name.c_str());
		return false;
	}
	ip = pick.ip;
	port = pick.port;
	LOG_TRACE("registry lookup %s -> %d %s:%s", name.c_str(), pick.id, ip.c_str(), port.c_str());
	return true;
}

void RegistryClient::Watch(::OI::Net::SessionManager* mgr, const std::string& name)
{
	if (!mgr || name.empty() || !Configured())
	{
		return;
	}
	ServiceTable::GetInstance().Watch(mgr, name);
	ArmPull();
}

void RegistryClient::ArmPull()
{
	if (_pull_armed)
	{
		return;
	}
	TryGetConfig("Registry.pull_ms", _pull_ms);
	if (_pull_ms <= 0)
	{
		_pull_ms = 1000;
	}
	::OI::Timer::BaseTimerManager::GetInstance().AddTimer(new PullTimer(), _pull_ms);
	_pull_armed = true;
}

void RegistryClient::ArmHeartbeat()
{
	if (_heartbeat_armed)
	{
		return;
	}
	TryGetConfig("Registry.heartbeat_ms", _heartbeat_ms);
	if (_heartbeat_ms <= 0)
	{
		_heartbeat_ms = 1000;
	}
	::OI::Timer::BaseTimerManager::GetInstance().AddTimer(new HeartbeatTimer(), _heartbeat_ms);
	_heartbeat_armed = true;
}

void RegistryClient::Pull()
{
	std::vector<std::string> names = ServiceTable::GetInstance().Names();
	if (names.empty())
	{
		return;
	}
	if (!_connected.load())
	{
		Connect();
		return;
	}
	PendingPull pending;
	pending.sync = false;
	pending.names = names;
	{
		::OI::Thread::MutexGuard lock(_pending_mu);
		_pending_pulls.push_back(pending);
	}
	::OI::RpcData::RegPull req(names);
	if (!Send(PROTO_REG_PULL, req))
	{
		::OI::Thread::MutexGuard lock(_pending_mu);
		if (!_pending_pulls.empty())
		{
			_pending_pulls.pop_back();
		}
	}
}

void RegistryClient::Touch()
{
	if (!_published.load() || _lost.load())
	{
		return;
	}
	if (!_connected.load())
	{
		Connect();
		return;
	}
	if (!_registered.load())
	{
		SendRegister();
		return;
	}
	::OI::RpcData::RegHeartbeat req(_svc_name, _svc_id);
	Send(PROTO_REG_HEARTBEAT, req);
}

namespace {
bool TakeAdvertise(const std::string& key, std::string& value)
{
	std::string configured;
	if (!::OI::Base::Config::GlobalConfig().GetAs(key, configured) || configured.empty())
	{
		return false;
	}
	value = configured;
	return true;
}

bool PublishAddress(const std::string& section, const std::string& bind_ip, const std::string& bind_port, std::string& ip, std::string& port)
{
	ip = bind_ip;
	port = bind_port;
	TakeAdvertise("Service.advertise_ip", ip);
	TakeAdvertise("Service.advertise_port", port);
	TakeAdvertise(section + ".advertise_ip", ip);
	TakeAdvertise(section + ".advertise_port", port);
	if (ip.empty() || port.empty() || ip == "0.0.0.0" || ip == "*")
	{
		return false;
	}
	return true;
}
}

bool RegistryClient::Publish(const std::string& section, const std::string& ip, const std::string& port)
{
	if (section == "Registry" || !Configured())
	{
		return true;
	}
	std::string name;
	if (!::OI::Base::Config::GlobalConfig().GetAs("Service.name", name) || name.empty())
	{
		return true;
	}
	int32_t id = 0;
	if (!::OI::Base::Config::GlobalConfig().GetAs("Service.id", id) || id <= 0)
	{
		fprintf(stderr, "registry publish %s: [Service] id 必须是正整数\n", name.c_str());
		fflush(stderr);
		LOG_ERROR("registry publish %s invalid id", name.c_str());
		return false;
	}
	std::string advertise_ip;
	std::string advertise_port;
	if (!PublishAddress(section, ip, port, advertise_ip, advertise_port))
	{
		LOG_TRACE("registry publish %s skip bind %s:%s", name.c_str(), ip.c_str(), port.c_str());
		return true;
	}
	if (_published.load())
	{
		if (_svc_ip != advertise_ip || _svc_port != advertise_port)
		{
			LOG_TRACE("registry publish %s keep %s:%s, skip %s:%s", name.c_str(),
				_svc_ip.c_str(), _svc_port.c_str(), advertise_ip.c_str(), advertise_port.c_str());
		}
		return true;
	}
	_svc_name = name;
	_svc_id = id;
	_svc_ip = advertise_ip;
	_svc_port = advertise_port;
	bool got = false;
	int32_t code = OI_ERR_FAIL;
	if (Ensure())
	{
		_got = false;
		_code = OI_ERR_FAIL;
		_reg_waiting = true;
		SendRegister();
		got = WaitFlag(_got);
		code = _code;
		_reg_waiting = false;
	}
	if (got && code == OI_ERR_EXIST)
	{
		fprintf(stderr, "registry publish %s id=%d 已经在线，拒绝启动\n", name.c_str(), id);
		fflush(stderr);
		LOG_ERROR("registry publish %s id=%d already online", name.c_str(), id);
		return false;
	}
	_published = true;
	if (got && code == OI_OK)
	{
		LOG_TRACE("registry publish %s id=%d %s:%s", name.c_str(), id, advertise_ip.c_str(), advertise_port.c_str());
	}
	else
	{
		LOG_ERROR("registry publish %s id=%d not confirmed, retry on heartbeat", name.c_str(), id);
	}
	ArmHeartbeat();
	return true;
}

}}
