#include "oi_registry.h"
#include "oi_proto_id.h"
#include "oi_rpcdata_regregister.h"
#include "oi_rpcdata_regregisterres.h"
#include "oi_rpcdata_regheartbeat.h"
#include "oi_rpcdata_regheartbeatres.h"
#include "oi_rpcdata_reglookup.h"
#include "oi_rpcdata_reglookupres.h"
#include "oi_net_session.h"
#include "oi_config.h"
#include "oi_log.h"
#include "oi_poll.h"
#include "oi_timer_manager.h"
#include "hiredis.h"
#include <chrono>
#include <cstdlib>

namespace OI { namespace Base {

namespace {
const int kWaitMs = 1000;

::OI::Base::Bytes EncodeFrame(uint32_t id, const ::OI::Rpc::Serialize& msg)
{
	::OI::Base::BytesStream body;
	msg.serialize(body);
	::OI::Base::BytesStream frame;
	uint32_t size = (uint32_t)body.Size();
	frame.WritePbRaw(id, size ? body.Begin() : NULL, size);
	return frame;
}

bool ReadFrames(::OI::Base::Bytes& in, void (*on_frame)(::OI::Net::Session*, uint32_t, ::OI::Base::BytesStream&, void*), ::OI::Net::Session* session, void* self)
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
			on_frame(session, id, body, self);
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

class RegistryServerSession : public ::OI::Net::NetSession
{
public:
	explicit RegistryServerSession(::OI::Net::SessionManager* mgr) : ::OI::Net::NetSession(mgr) {}
	virtual void Send(const ::OI::Base::Bytes& bytes)
	{
		::OI::Thread::MutexGuard lock(_locker);
		_obuff.Append(bytes);
		AllowSend();
	}
	virtual void OnDataOut() {}
	virtual void OnDataIn()
	{
		ReadFrames(_ibuff, [](::OI::Net::Session* session, uint32_t id, ::OI::Base::BytesStream& body, void* self) {
			static_cast<RegistryServer*>(self)->Handle(session, id, body);
		}, this, _p_session_mgr);
	}
};

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
		ReadFrames(_ibuff, [](::OI::Net::Session* session, uint32_t id, ::OI::Base::BytesStream& body, void* self) {
			(void)session;
			static_cast<RegistryClient*>(self)->OnFrame(id, body);
		}, this, _p_session_mgr);
	}
};

}

RedisStore::RedisStore() : _ctx(NULL), _opened(false), _expire_ms(3000) {}

RedisStore::~RedisStore()
{
	Close();
}

void RedisStore::Close()
{
	if (_ctx)
	{
		redisFree((redisContext*)_ctx);
		_ctx = NULL;
	}
	_opened = false;
}

bool RedisStore::Ensure()
{
	if (_ctx)
	{
		return true;
	}
	std::string ip;
	std::string port;
	if (!::OI::Base::Config::GlobalConfig().GetAs("Redis.ip", ip) || ip.empty())
	{
		return false;
	}
	if (!::OI::Base::Config::GlobalConfig().GetAs("Redis.port", port) || port.empty())
	{
		return false;
	}
	TryGetConfig("Registry.expire_ms", _expire_ms);
	if (_expire_ms <= 0)
	{
		_expire_ms = 3000;
	}
	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 200000;
	redisContext* ctx = redisConnectWithTimeout(ip.c_str(), atoi(port.c_str()), tv);
	if (!ctx || ctx->err)
	{
		if (ctx)
		{
			redisFree(ctx);
		}
		return false;
	}
	_ctx = ctx;
	_opened = true;
	return true;
}

bool RedisStore::Enabled()
{
	::OI::Thread::MutexGuard lock(_mu);
	return Ensure();
}

bool RedisStore::SetService(const std::string& name, const std::string& ip, const std::string& port)
{
	if (name.empty() || ip.empty() || port.empty())
	{
		return false;
	}
	std::string value = ip + " " + port;
	std::string key = "oi:svc:" + name;
	::OI::Thread::MutexGuard lock(_mu);
	if (!Ensure())
	{
		return false;
	}
	redisReply* reply = (redisReply*)redisCommand((redisContext*)_ctx, "SET %s %b PX %d",
		key.c_str(), value.data(), (size_t)value.size(), _expire_ms);
	bool ok = reply && reply->type != REDIS_REPLY_ERROR;
	if (reply)
	{
		freeReplyObject(reply);
	}
	if (!ok)
	{
		Close();
	}
	return ok;
}

bool RedisStore::GetService(const std::string& name, std::string& ip, std::string& port)
{
	if (name.empty())
	{
		return false;
	}
	std::string key = "oi:svc:" + name;
	::OI::Thread::MutexGuard lock(_mu);
	if (!Ensure())
	{
		return false;
	}
	redisReply* reply = (redisReply*)redisCommand((redisContext*)_ctx, "GET %s", key.c_str());
	bool ok = false;
	if (reply && reply->type == REDIS_REPLY_STRING && reply->str)
	{
		std::string value(reply->str, (size_t)reply->len);
		size_t sp = value.find(' ');
		if (sp != std::string::npos && sp + 1 < value.size())
		{
			ip = value.substr(0, sp);
			port = value.substr(sp + 1);
			ok = !ip.empty() && !port.empty();
		}
	}
	if (reply)
	{
		freeReplyObject(reply);
	}
	else
	{
		Close();
	}
	return ok;
}

bool RedisStore::DeleteService(const std::string& name)
{
	if (name.empty())
	{
		return false;
	}
	std::string key = "oi:svc:" + name;
	::OI::Thread::MutexGuard lock(_mu);
	if (!Ensure())
	{
		return false;
	}
	redisReply* reply = (redisReply*)redisCommand((redisContext*)_ctx, "DEL %s", key.c_str());
	bool ok = reply != NULL;
	if (reply)
	{
		freeReplyObject(reply);
	}
	else
	{
		Close();
	}
	return ok;
}

bool RedisStore::AppendLog(const void* data, size_t n)
{
	if (!data || n == 0)
	{
		return false;
	}
	::OI::Thread::MutexGuard lock(_mu);
	if (!Ensure())
	{
		return false;
	}
	redisReply* reply = (redisReply*)redisCommand((redisContext*)_ctx, "RPUSH oi:log %b", data, n);
	bool ok = reply && reply->type != REDIS_REPLY_ERROR;
	if (reply)
	{
		freeReplyObject(reply);
	}
	if (!ok)
	{
		Close();
		return false;
	}
	reply = (redisReply*)redisCommand((redisContext*)_ctx, "LTRIM oi:log -10000 -1");
	if (reply)
	{
		freeReplyObject(reply);
	}
	else
	{
		Close();
	}
	return true;
}

bool RedisStore::ListServices(std::map<std::string, std::pair<std::string, std::string> >& out)
{
	out.clear();
	::OI::Thread::MutexGuard lock(_mu);
	if (!Ensure())
	{
		return false;
	}
	redisReply* keys = (redisReply*)redisCommand((redisContext*)_ctx, "KEYS oi:svc:*");
	if (!keys)
	{
		Close();
		return false;
	}
	if (keys->type != REDIS_REPLY_ARRAY)
	{
		freeReplyObject(keys);
		return false;
	}
	for (size_t i = 0; i < (size_t)keys->elements; ++i)
	{
		redisReply* key = keys->element[i];
		if (!key || key->type != REDIS_REPLY_STRING || !key->str)
		{
			continue;
		}
		std::string key_text(key->str, (size_t)key->len);
		const std::string prefix = "oi:svc:";
		if (key_text.size() <= prefix.size() || key_text.compare(0, prefix.size(), prefix) != 0)
		{
			continue;
		}
		redisReply* value = (redisReply*)redisCommand((redisContext*)_ctx, "GET %s", key_text.c_str());
		if (!value)
		{
			freeReplyObject(keys);
			Close();
			return false;
		}
		if (value->type == REDIS_REPLY_STRING && value->str)
		{
			std::string text(value->str, (size_t)value->len);
			size_t sp = text.find(' ');
			if (sp != std::string::npos && sp + 1 < text.size())
			{
				out[key_text.substr(prefix.size())] = std::make_pair(text.substr(0, sp), text.substr(sp + 1));
			}
		}
		freeReplyObject(value);
	}
	freeReplyObject(keys);
	return true;
}

namespace {
class SweepTimer : public ::OI::Timer::BaseTimer
{
public:
	bool OnTimer()
	{
		RegistryServer::GetInstance().Sweep();
		return true;
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
		ServiceTable::GetInstance().Pull();
		return true;
	}
};
}

ServiceTable::ServiceTable() : _pull_armed(false), _pull_ms(1000) {}
ServiceTable::~ServiceTable() {}

void ServiceTable::ArmPull()
{
	if (_pull_armed)
	{
		return;
	}
	_pull_ms = 1000;
	TryGetConfig("Registry.pull_ms", _pull_ms);
	if (_pull_ms <= 0)
	{
		return;
	}
	::OI::Timer::BaseTimerManager::GetInstance().AddTimer(new PullTimer(), _pull_ms);
	_pull_armed = true;
}

void ServiceTable::Watch(::OI::Net::SessionManager* mgr, const std::string& name)
{
	if (!mgr || name.empty())
	{
		return;
	}
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
	ArmPull();
}

bool ServiceTable::Publish(const std::string& name, const std::string& ip, const std::string& port)
{
	if (name.empty() || ip.empty() || port.empty())
	{
		return false;
	}
	std::vector<::OI::Net::SessionManager*> watchers;
	{
		::OI::Thread::MutexGuard lock(_mu);
		std::map<std::string, State>::iterator it = _services.find(name);
		if (it != _services.end() && it->second.published
			&& it->second.ip == ip && it->second.port == port)
		{
			watchers.clear();
		}
		else if (it != _services.end())
		{
			watchers = it->second.watchers;
		}
	}
	if (!RedisStore::GetInstance().SetService(name, ip, port))
	{
		return false;
	}
	{
		::OI::Thread::MutexGuard lock(_mu);
		State& state = _services[name];
		bool same = state.published && state.ip == ip && state.port == port;
		state.ip = ip;
		state.port = port;
		state.online = true;
		state.published = true;
		if (same)
		{
			return true;
		}
		if (watchers.empty())
		{
			watchers = state.watchers;
		}
	}
	for (size_t i = 0; i < watchers.size(); ++i)
	{
		watchers[i]->ApplyService(ip, port, true);
	}
	return true;
}

void ServiceTable::Touch()
{
	std::vector<std::pair<std::string, std::pair<std::string, std::string> > > owned;
	{
		::OI::Thread::MutexGuard lock(_mu);
		for (std::map<std::string, State>::iterator it = _services.begin(); it != _services.end(); ++it)
		{
			if (!it->second.published)
			{
				continue;
			}
			owned.push_back(std::make_pair(it->first, std::make_pair(it->second.ip, it->second.port)));
		}
	}
	for (size_t i = 0; i < owned.size(); ++i)
	{
		RedisStore::GetInstance().SetService(owned[i].first, owned[i].second.first, owned[i].second.second);
	}
}

void ServiceTable::Pull()
{
	std::map<std::string, std::pair<std::string, std::string> > list;
	if (!RedisStore::GetInstance().ListServices(list))
	{
		return;
	}
	std::vector<std::pair<::OI::Net::SessionManager*, std::pair<std::string, std::pair<std::string, std::string> > > > changed;
	{
		::OI::Thread::MutexGuard lock(_mu);
		for (std::map<std::string, State>::iterator it = _services.begin(); it != _services.end(); ++it)
		{
			if (it->second.watchers.empty())
			{
				continue;
			}
			std::map<std::string, std::pair<std::string, std::string> >::iterator found = list.find(it->first);
			bool online = found != list.end();
			std::string ip = online ? found->second.first : "";
			std::string port = online ? found->second.second : "";
			bool same = it->second.online == online && it->second.ip == ip && it->second.port == port;
			if (same && it->second.online)
			{
				continue;
			}
			it->second.online = online;
			it->second.ip = ip;
			it->second.port = port;
			for (size_t i = 0; i < it->second.watchers.size(); ++i)
			{
				changed.push_back(std::make_pair(it->second.watchers[i], std::make_pair(it->first, std::make_pair(ip, port))));
			}
		}
	}
	for (size_t i = 0; i < changed.size(); ++i)
	{
		bool online = !changed[i].second.second.first.empty();
		changed[i].first->ApplyService(changed[i].second.second.first, changed[i].second.second.second, online);
	}
}


RegistryServer::RegistryServer() : _expire_ms(3000), _sweep_armed(false) {}
RegistryServer::~RegistryServer() {}

const std::string RegistryServer::GetIDString()
{
	return "Registry";
}

bool RegistryServer::Init()
{
	if (!::OI::Net::SessionManager::Init())
	{
		return false;
	}
	TryGetConfig("Registry.expire_ms", _expire_ms);
	if (_expire_ms > 0 && !_sweep_armed)
	{
		::OI::Timer::BaseTimerManager::GetInstance().AddTimer(new SweepTimer(), _expire_ms);
		_sweep_armed = true;
	}
	return true;
}

::OI::Net::Session* RegistryServer::CloneSession()
{
	return new RegistryServerSession(this);
}

void RegistryServer::OnAddSession(::OI::Net::Session* pSession)
{
	LOG_TRACE("RegistryServer::OnAddSession sid=%d", pSession->GetSessionID());
}

void RegistryServer::OnDelSession(const ::OI::Net::Session* pSession)
{
	DropSid(pSession->GetSessionID());
	LOG_TRACE("RegistryServer::OnDelSession sid=%d", pSession->GetSessionID());
}

void RegistryServer::DropSid(sid_t sid)
{
	::OI::Thread::MutexGuard lock(_table_mu);
	for (std::map<std::string, Item>::iterator it = _table.begin(); it != _table.end(); )
	{
		if (it->second.sid == sid)
		{
			std::string name = it->first;
			it = _table.erase(it);
			RedisStore::GetInstance().DeleteService(name);
		}
		else
		{
			++it;
		}
	}
}

void RegistryServer::Sweep()
{
	if (_expire_ms <= 0)
	{
		return;
	}
	std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	::OI::Thread::MutexGuard lock(_table_mu);
	for (std::map<std::string, Item>::iterator it = _table.begin(); it != _table.end(); )
	{
		long long age = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.seen).count();
		if (age > _expire_ms)
		{
			std::string name = it->first;
			it = _table.erase(it);
			RedisStore::GetInstance().DeleteService(name);
		}
		else
		{
			++it;
		}
	}
}

static void ReplyCode(::OI::Net::Session* session, uint32_t id, int32_t code, const std::string& ip, const std::string& port)
{
	if (id == PROTO_REG_LOOKUP_RES)
	{
		::OI::RpcData::RegLookupRes res;
		res.code = code;
		res.ip = ip;
		res.port = port;
		session->Send(EncodeFrame(id, res));
		return;
	}
	if (id == PROTO_REG_HEARTBEAT_RES)
	{
		::OI::RpcData::RegHeartbeatRes res;
		res.code = code;
		session->Send(EncodeFrame(id, res));
		return;
	}
	::OI::RpcData::RegRegisterRes res;
	res.code = code;
	session->Send(EncodeFrame(id, res));
}

void RegistryServer::Handle(::OI::Net::Session* session, uint32_t id, ::OI::Base::BytesStream& body)
{
	if (id == PROTO_REG_REGISTER)
	{
		::OI::RpcData::RegRegister req;
		req.deserialize(body);
		int32_t code = OI_ERR_FAIL;
		if (!req.name.empty() && !req.ip.empty() && !req.port.empty()
			&& RedisStore::GetInstance().SetService(req.name, req.ip, req.port))
		{
			::OI::Thread::MutexGuard lock(_table_mu);
			Item& item = _table[req.name];
			item.ip = req.ip;
			item.port = req.port;
			item.sid = session->GetSessionID();
			item.seen = std::chrono::steady_clock::now();
			code = 0;
		}
		ReplyCode(session, PROTO_REG_REGISTER_RES, code, "", "");
		return;
	}
	if (id == PROTO_REG_HEARTBEAT)
	{
		::OI::RpcData::RegHeartbeat req;
		req.deserialize(body);
		int32_t code = OI_ERR_FAIL;
		{
			::OI::Thread::MutexGuard lock(_table_mu);
			std::map<std::string, Item>::iterator it = _table.find(req.name);
			if (it != _table.end() && it->second.sid == session->GetSessionID()
				&& RedisStore::GetInstance().SetService(req.name, it->second.ip, it->second.port))
			{
				it->second.seen = std::chrono::steady_clock::now();
				code = 0;
			}
		}
		ReplyCode(session, PROTO_REG_HEARTBEAT_RES, code, "", "");
		return;
	}
	if (id == PROTO_REG_LOOKUP)
	{
		::OI::RpcData::RegLookup req;
		req.deserialize(body);
		std::string ip;
		std::string port;
		int32_t code = OI_ERR_FAIL;
		if (RedisStore::GetInstance().GetService(req.name, ip, port))
		{
			code = 0;
		}
		ReplyCode(session, PROTO_REG_LOOKUP_RES, code, ip, port);
		return;
	}
}

RegistryClient::RegistryClient()
	: _connected(false), _failed(false), _sid(INVALID_SID), _address_ready(false), _connecting(false),
	_republish(false), _heartbeat_ms(1000), _heartbeat_armed(false), _pull_armed(false), _pull_ms(1000),
	_expect(0), _got(false), _code(OI_ERR_FAIL)
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

bool RegistryClient::Ensure()
{
	if (_connected.load() && _sid != INVALID_SID)
	{
		return true;
	}
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
		TryGetConfig("Registry.heartbeat_ms", _heartbeat_ms);
		_address_ready = true;
	}
	_failed = false;
	_connecting = true;
	if (!StartClient())
	{
		_connecting = false;
		return false;
	}
	if (_connected.load())
	{
		_connecting = false;
		return true;
	}
	bool ok = WaitFlag(_connected);
	_connecting = false;
	return ok;
}

void RegistryClient::OnAddSession(::OI::Net::Session* pSession)
{
	_sid = pSession->GetSessionID();
	_failed = false;
	_connected = true;
	_connecting = false;
	Notify();
	if (_republish)
	{
		SendRegister();
	}
	LOG_TRACE("RegistryClient::OnAddSession sid=%d", _sid);
}

void RegistryClient::OnDelSession(const ::OI::Net::Session* pSession)
{
	(void)pSession;
	_sid = INVALID_SID;
	_connected = false;
	_failed = true;
	Notify();
	LOG_TRACE("RegistryClient::OnDelSession");
}

void RegistryClient::OnConnectFailed()
{
	_connecting = false;
	_failed = true;
	Notify();
	LOG_TRACE("RegistryClient::OnConnectFailed");
}

void RegistryClient::SendRegister()
{
	if (_sid == INVALID_SID || _svc_name.empty())
	{
		return;
	}
	::OI::RpcData::RegRegister req(_svc_name, _svc_ip, _svc_port);
	SendBytes(_sid, EncodeFrame(PROTO_REG_REGISTER, req));
}

void RegistryClient::SendHeartbeat()
{
	if (_sid == INVALID_SID || _svc_name.empty())
	{
		return;
	}
	::OI::RpcData::RegHeartbeat req;
	req.name = _svc_name;
	SendBytes(_sid, EncodeFrame(PROTO_REG_HEARTBEAT, req));
}

void RegistryClient::ArmHeartbeat()
{
	if (_heartbeat_armed || _heartbeat_ms <= 0)
	{
		return;
	}
	::OI::Timer::BaseTimerManager::GetInstance().AddTimer(new HeartbeatTimer(), _heartbeat_ms);
	_heartbeat_armed = true;
}

void RegistryClient::Watch(::OI::Net::SessionManager* mgr, const std::string& name)
{
	if (!mgr || name.empty())
	{
		return;
	}
	ServiceTable::GetInstance().Watch(mgr, name);
}

void RegistryClient::Pull()
{
	ServiceTable::GetInstance().Pull();
}

void RegistryClient::Touch()
{
	if (_svc_name.empty())
	{
		return;
	}
	ServiceTable::GetInstance().Touch();
	if (RedisStore::GetInstance().Enabled())
	{
		return;
	}
	if (!_connected.load())
	{
		if (!_connecting)
		{
			_failed = false;
			_connecting = true;
			if (!StartClient())
			{
				_connecting = false;
			}
		}
		return;
	}
	SendHeartbeat();
}

void RegistryClient::OnFrame(uint32_t id, ::OI::Base::BytesStream& body)
{
	if (id == PROTO_REG_REGISTER_RES)
	{
		::OI::RpcData::RegRegisterRes res;
		res.code = OI_ERR_FAIL;
		res.deserialize(body);
		_code = res.code;
		if (_expect == id)
		{
			_got = true;
			Notify();
		}
		return;
	}
	if (id == PROTO_REG_LOOKUP_RES)
	{
		::OI::RpcData::RegLookupRes res;
		res.code = OI_ERR_FAIL;
		res.deserialize(body);
		_code = res.code;
		_res_ip = res.ip;
		_res_port = res.port;
		if (_expect == id)
		{
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
		if (res.code != 0 && _connected.load())
		{
			SendRegister();
		}
	}
}

bool RegistryClient::Lookup(const std::string& name, std::string& ip, std::string& port)
{
	if (name.empty())
	{
		return false;
	}
	if (RedisStore::GetInstance().GetService(name, ip, port))
	{
		LOG_TRACE("registry lookup %s -> %s:%s", name.c_str(), ip.c_str(), port.c_str());
		return true;
	}
	if (!Configured())
	{
		return false;
	}
	if (!Ensure())
	{
		LOG_TRACE("registry lookup %s connect failed", name.c_str());
		return false;
	}
	::OI::RpcData::RegLookup req;
	req.name = name;
	_code = OI_ERR_FAIL;
	_res_ip.clear();
	_res_port.clear();
	_got = false;
	_expect = PROTO_REG_LOOKUP_RES;
	if (!SendBytes(_sid, EncodeFrame(PROTO_REG_LOOKUP, req)))
	{
		_expect = 0;
		return false;
	}
	if (!WaitFlag(_got))
	{
		_expect = 0;
		LOG_TRACE("registry lookup %s timeout", name.c_str());
		return false;
	}
	_expect = 0;
	if (_code != 0 || _res_ip.empty() || _res_port.empty())
	{
		LOG_TRACE("registry lookup %s miss", name.c_str());
		return false;
	}
	ip = _res_ip;
	port = _res_port;
	LOG_TRACE("registry lookup %s -> %s:%s", name.c_str(), ip.c_str(), port.c_str());
	return true;
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
	std::string name;
	if (!::OI::Base::Config::GlobalConfig().GetAs("Service.name", name) || name.empty())
	{
		return false;
	}
	std::string advertise_ip;
	std::string advertise_port;
	if (!PublishAddress(section, ip, port, advertise_ip, advertise_port))
	{
		LOG_TRACE("registry publish %s skip bind %s:%s", name.c_str(), ip.c_str(), port.c_str());
		return false;
	}
	_svc_name = name;
	_svc_ip = advertise_ip;
	_svc_port = advertise_port;
	if (ServiceTable::GetInstance().Publish(name, advertise_ip, advertise_port))
	{
		_republish = true;
		ArmHeartbeat();
		LOG_TRACE("registry publish %s %s:%s", name.c_str(), advertise_ip.c_str(), advertise_port.c_str());
		return true;
	}
	if (RedisStore::GetInstance().Enabled())
	{
		return false;
	}
	if (!Configured())
	{
		return false;
	}
	if (!Ensure())
	{
		LOG_TRACE("registry publish %s connect failed", name.c_str());
		return false;
	}
	_code = OI_ERR_FAIL;
	_got = false;
	_expect = PROTO_REG_REGISTER_RES;
	SendRegister();
	bool ok = WaitFlag(_got) && _code == 0;
	_expect = 0;
	if (ok)
	{
		_republish = true;
		ArmHeartbeat();
		LOG_TRACE("registry publish %s %s:%s", name.c_str(), _svc_ip.c_str(), _svc_port.c_str());
	}
	else
	{
		LOG_TRACE("registry publish %s failed", name.c_str());
	}
	return ok;
}

}}
