#include "registry_server.h"
#include "oi_proto_id.h"
#include "oi_rpcdata_regregister.h"
#include "oi_rpcdata_regregisterres.h"
#include "oi_rpcdata_regheartbeat.h"
#include "oi_rpcdata_regheartbeatres.h"
#include "oi_rpcdata_reginstance.h"
#include "oi_rpcdata_regpull.h"
#include "oi_rpcdata_regpullres.h"
#include "oi_net_session.h"
#include "oi_redis.h"
#include "oi_config.h"
#include "oi_log.h"
#include "oi_timer_manager.h"
#include <cstdio>
#include <cstdlib>

// 表、绑定和快照只在 poll 线程上动，协程也在这条线程上恢复，所以不加锁。
#ifndef _SINGLE_THREAD_
#error registry must be built single-threaded
#endif

namespace {

// 键不在就占上，同一个地址就更新。地址不一样不动它。
const char* kRegisterLua =
	"local v = redis.call('GET', KEYS[1]) "
	"if v and v ~= ARGV[1] then return 0 end "
	"redis.call('SET', KEYS[1], ARGV[1], 'PX', ARGV[2]) "
	"redis.call('SADD', KEYS[2], ARGV[3]) redis.call('SADD', KEYS[3], ARGV[4]) return 1";

// 还是自己的就延长。过期了没人占就重新占上。被别人占了不动它。
const char* kRefreshLua =
	"local v = redis.call('GET', KEYS[1]) "
	"if v == ARGV[1] then redis.call('PEXPIRE', KEYS[1], ARGV[2]) "
	"  redis.call('SADD', KEYS[2], ARGV[3]) redis.call('SADD', KEYS[3], ARGV[4]) return 1 end "
	"if v then return 0 end "
	"redis.call('SET', KEYS[1], ARGV[1], 'PX', ARGV[2]) "
	"redis.call('SADD', KEYS[2], ARGV[3]) redis.call('SADD', KEYS[3], ARGV[4]) return 2";

const char* kWithdrawLua =
	"if redis.call('GET', KEYS[1]) == ARGV[1] then redis.call('DEL', KEYS[1]) redis.call('SREM', KEYS[2], ARGV[2]) end "
	"return 1";

// 一次取回整个空间。过期的 id、空了的名字顺手摘掉。
const char* kListAllLua =
	"local out = {} "
	"for _, n in ipairs(redis.call('SMEMBERS', KEYS[1])) do "
	"  local set = ARGV[1] .. n "
	"  local alive = 0 "
	"  for _, id in ipairs(redis.call('SMEMBERS', set)) do "
	"    local v = redis.call('GET', set .. ':' .. id) "
	"    if v then table.insert(out, n) table.insert(out, id) table.insert(out, v) alive = alive + 1 "
	"    else redis.call('SREM', set, id) end "
	"  end "
	"  if alive == 0 then redis.call('SREM', KEYS[1], n) end "
	"end "
	"return out";

bool ValidSpacePart(const std::string& s)
{
	if (s.empty())
	{
		return false;
	}
	for (size_t i = 0; i < s.size(); ++i)
	{
		char c = s[i];
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
		if (!ok)
		{
			return false;
		}
	}
	return true;
}

// 值是 "ip port"。名字、id、地址都一样就是同一个服务，重新登记直接续上；地址不一样才算被别人占了。
bool ParseValue(const std::string& value, std::string& ip, std::string& port)
{
	size_t a = value.find(' ');
	if (a == std::string::npos || a == 0)
	{
		return false;
	}
	size_t b = value.find(' ', a + 1);
	ip = value.substr(0, a);
	port = value.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
	return !ip.empty() && !port.empty();
}

int ReplyInt(const ::OI::Redis::RedisResult& r)
{
	if (!r || !r.reply || r.reply->type != REDIS_REPLY_INTEGER)
	{
		return -1;
	}
	return (int)r.reply->integer;
}

std::string StrOf(const redisReply* r)
{
	return std::string(r->str, (size_t)r->len);
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
		::OI::Base::ReadRegFrames(_ibuff, this, [this](uint32_t id, ::OI::Base::BytesStream& body) {
			RegistryServer::GetInstance().Handle(this, id, body);
		});
	}
};

class ReloadTimer : public ::OI::Timer::BaseTimer
{
public:
	bool OnTimer()
	{
		RegistryServer::GetInstance().StartReload();
		return true;
	}
};
}

RegistryServer::RegistryServer() : _snapshot_ok(false), _reloading(false), _pull_ms(1000), _expire_ms(3000) {}
RegistryServer::~RegistryServer() {}

const std::string RegistryServer::GetIDString()
{
	return "Registry";
}

bool RegistryServer::LoadSpace()
{
	std::string project;
	std::string env;
	::OI::Base::Config::GlobalConfig().GetAs("Registry.project", project);
	::OI::Base::Config::GlobalConfig().GetAs("Registry.env", env);
	if (!ValidSpacePart(project) || !ValidSpacePart(env))
	{
		fprintf(stderr, "registry space invalid: [Registry] project=\"%s\" env=\"%s\"，只能用字母、数字、_、-，且不能为空\n",
			project.c_str(), env.c_str());
		fflush(stderr);
		return false;
	}
	_space = project + "." + env;
	_prefix = "oi:{" + _space + "}:";
	return true;
}

std::string RegistryServer::Key(const std::string& tail) const
{
	return _prefix + tail;
}

// 登记和续期共用：三个键是实例、id 集合、名字集合。
std::vector<std::string> RegistryServer::EvalArgs(const char* lua, const Binding& b) const
{
	return { "EVAL", lua, "3", Key("svc:" + b.name + ":" + std::to_string(b.id)), Key("svc:" + b.name), Key("svc"),
		b.value, std::to_string(_expire_ms), std::to_string(b.id), b.name };
}

bool RegistryServer::Init()
{
	if (!::OI::Net::SessionManager::Init())
	{
		return false;
	}
	if (!LoadSpace())
	{
		return false;
	}
	TryGetConfig("Registry.pull_ms", _pull_ms);
	if (_pull_ms <= 0)
	{
		_pull_ms = 1000;
	}
	TryGetConfig("Registry.expire_ms", _expire_ms);
	if (_expire_ms <= 0)
	{
		_expire_ms = 3000;
	}
	if (!::OI::Redis::RedisClient::GetInstance().Init())
	{
		fprintf(stderr, "registry needs [RedisClient] ip/port/type\n");
		fflush(stderr);
		return false;
	}
	::OI::Redis::RedisClient::GetInstance().StartClient();
	coro::StartTask([this](coro::Token) {
		return Boot();
	});
	LOG_TRACE("registry space=%s", _space.c_str());
	return true;
}

coro::Task RegistryServer::Boot()
{
	int spins = 0;
	while (!::OI::Redis::RedisClient::GetInstance().IsConnect())
	{
		if (++spins % 20 == 0)
		{
			LOG_TRACE("registry waiting redis");
		}
		co_await coro::yield(50);
	}
	StartReload();
	::OI::Timer::BaseTimerManager::GetInstance().AddTimer(new ReloadTimer(), _pull_ms);
}

::OI::Net::Session* RegistryServer::CloneSession()
{
	return new RegistryServerSession(this);
}

void RegistryServer::OnAddSession(::OI::Net::Session* pSession)
{
	_alive.insert(pSession->GetSessionID());
	LOG_INFO("RegistryServer::OnAddSession sid=%d", pSession->GetSessionID());
}

void RegistryServer::OnDelSession(const ::OI::Net::Session* pSession)
{
	sid_t sid = pSession->GetSessionID();
	_alive.erase(sid);
	std::map<sid_t, std::vector<Binding> >::iterator it = _bindings.find(sid);
	if (it != _bindings.end())
	{
		std::vector<Binding> list;
		list.swap(it->second);
		_bindings.erase(it);
		for (size_t i = 0; i < list.size(); ++i)
		{
			Binding b = list[i];
			DropSnapshot(b.name, b.id);
			LOG_TRACE("registry offline %s id=%d (connection closed)", b.name.c_str(), b.id);
			coro::StartTask([this, b](coro::Token) {
				return Withdraw(b);
			});
		}
		fflush(stdout);
	}
	LOG_INFO("RegistryServer::OnDelSession sid=%d", sid);
}

RegistryServer::Binding* RegistryServer::FindBinding(sid_t sid, const std::string& name, int32_t id)
{
	std::map<sid_t, std::vector<Binding> >::iterator it = _bindings.find(sid);
	if (it == _bindings.end())
	{
		return NULL;
	}
	for (size_t i = 0; i < it->second.size(); ++i)
	{
		if (it->second[i].name == name && it->second[i].id == id)
		{
			return &it->second[i];
		}
	}
	return NULL;
}

void RegistryServer::DropSnapshot(const std::string& name, int32_t id)
{
	for (size_t i = 0; i < _snapshot.size(); ++i)
	{
		if (_snapshot[i].name == name && _snapshot[i].id == id)
		{
			_snapshot.erase(_snapshot.begin() + i);
			return;
		}
	}
}

void RegistryServer::PutSnapshot(const Binding& b)
{
	DropSnapshot(b.name, b.id);
	::OI::Base::ServiceInstance inst;
	inst.name = b.name;
	inst.id = b.id;
	inst.ip = b.ip;
	inst.port = b.port;
	_snapshot.push_back(inst);
}

void RegistryServer::Reply(sid_t sid, uint32_t id, const ::OI::Rpc::Serialize& msg)
{
	SendBytes(sid, ::OI::Base::EncodeRegFrame(id, msg));
}

void RegistryServer::StartReload()
{
	if (_reloading)
	{
		return;
	}
	_reloading = true;
	coro::StartTask([this](coro::Token) {
		return Reload();
	});
}

coro::Task RegistryServer::Reload()
{
	std::vector<std::string> args = {"EVAL", kListAllLua, "1", Key("svc"), Key("svc:")};
	::OI::Redis::RedisResult r = co_await ::OI::Redis::CallRedis(std::move(args));
	_reloading = false;
	if (!r || !r.reply || r.reply->type != REDIS_REPLY_ARRAY)
	{
		if (_snapshot_ok)
		{
			LOG_TRACE("registry reload failed status=%d, keep clients as is", (int)r.status);
			fflush(stdout);
		}
		_snapshot_ok = false;
		co_return;
	}
	std::vector<::OI::Base::ServiceInstance> list;
	const redisReply* arr = r.reply.get();
	for (size_t i = 0; i + 3 <= (size_t)arr->elements; i += 3)
	{
		const redisReply* name = arr->element[i];
		const redisReply* id = arr->element[i + 1];
		const redisReply* value = arr->element[i + 2];
		if (!name || !id || !value || name->type != REDIS_REPLY_STRING
			|| id->type != REDIS_REPLY_STRING || value->type != REDIS_REPLY_STRING)
		{
			continue;
		}
		::OI::Base::ServiceInstance inst;
		inst.name = StrOf(name);
		inst.id = atoi(StrOf(id).c_str());
		if (ParseValue(StrOf(value), inst.ip, inst.port))
		{
			list.push_back(inst);
		}
	}
	_snapshot.swap(list);
	_snapshot_ok = true;
}

coro::Task RegistryServer::Register(sid_t sid, Binding b)
{
	std::vector<std::string> args = EvalArgs(kRegisterLua, b);
	::OI::Redis::RedisResult r = co_await ::OI::Redis::CallRedis(std::move(args));
	int ret = ReplyInt(r);
	if (_alive.count(sid) == 0)
	{
		// 等 Redis 的时候连接断了。OnDelSession 不知道这一份，这里补删。
		if (ret == REG_OK)
		{
			co_await Withdraw(b);
		}
		co_return;
	}
	::OI::RpcData::RegRegisterRes res;
	res.code = OI_ERR_FAIL;
	if (ret == REG_OK)
	{
		Binding* found = FindBinding(sid, b.name, b.id);
		if (!found)
		{
			_bindings[sid].push_back(b);
		}
		else
		{
			*found = b;
		}
		PutSnapshot(b);
		res.code = OI_OK;
		LOG_TRACE("registry online %s id=%d %s:%s", b.name.c_str(), b.id, b.ip.c_str(), b.port.c_str());
		fflush(stdout);
	}
	else if (ret == REG_TAKEN)
	{
		res.code = OI_ERR_EXIST;
		LOG_TRACE("registry %s id=%d already online, refuse sid=%d", b.name.c_str(), b.id, sid);
		fflush(stdout);
	}
	else
	{
		LOG_TRACE("registry register %s id=%d redis failed status=%d", b.name.c_str(), b.id, (int)r.status);
	}
	Reply(sid, ::OI::Base::PROTO_REG_REGISTER_RES, res);
}

coro::Task RegistryServer::Refresh(sid_t sid, Binding b)
{
	std::vector<std::string> args = EvalArgs(kRefreshLua, b);
	::OI::Redis::RedisResult r = co_await ::OI::Redis::CallRedis(std::move(args));
	int ret = ReplyInt(r);
	Binding* found = _alive.count(sid) ? FindBinding(sid, b.name, b.id) : NULL;
	if (!found || found->value != b.value)
	{
		// 续期过程中连接断了或换了登记。续期可能把键又写回去了，按这份值删掉。
		if (ret == REG_OK || ret == REG_RENEWED)
		{
			co_await Withdraw(b);
		}
		co_return;
	}
	::OI::RpcData::RegHeartbeatRes res;
	res.code = OI_ERR_FAIL;
	if (ret == REG_OK || ret == REG_RENEWED)
	{
		if (ret == REG_RENEWED)
		{
			PutSnapshot(b);
			LOG_TRACE("registry %s id=%d registered again", b.name.c_str(), b.id);
			fflush(stdout);
		}
		res.code = OI_OK;
	}
	else if (ret == REG_TAKEN)
	{
		std::vector<Binding>& list = _bindings[sid];
		for (size_t i = 0; i < list.size(); ++i)
		{
			if (list[i].name == b.name && list[i].id == b.id)
			{
				list.erase(list.begin() + i);
				break;
			}
		}
		res.code = OI_ERR_EXIST;
		LOG_TRACE("registry %s id=%d taken by another process", b.name.c_str(), b.id);
		fflush(stdout);
	}
	Reply(sid, ::OI::Base::PROTO_REG_HEARTBEAT_RES, res);
}

coro::Task RegistryServer::Withdraw(Binding b)
{
	std::vector<std::string> args = {"EVAL", kWithdrawLua, "2", Key("svc:" + b.name + ":" + std::to_string(b.id)), Key("svc:" + b.name), b.value, std::to_string(b.id)};
	co_await ::OI::Redis::CallRedis(std::move(args));
	// 撤销之前发出的整量拉取可能又把它带回快照。这时还没人重新登记，就再摘一次。
	for (std::map<sid_t, std::vector<Binding> >::iterator it = _bindings.begin(); it != _bindings.end(); ++it)
	{
		for (size_t i = 0; i < it->second.size(); ++i)
		{
			if (it->second[i].name == b.name && it->second[i].id == b.id)
			{
				co_return;
			}
		}
	}
	DropSnapshot(b.name, b.id);
}

void RegistryServer::Handle(::OI::Net::Session* session, uint32_t id, ::OI::Base::BytesStream& body)
{
	sid_t sid = session->GetSessionID();
	if (id == ::OI::Base::PROTO_REG_REGISTER)
	{
		::OI::RpcData::RegRegister req;
		req.deserialize(body);
		if (req.name.empty() || req.id <= 0 || req.ip.empty() || req.port.empty())
		{
			::OI::RpcData::RegRegisterRes res;
			res.code = OI_ERR_FAIL;
			Reply(sid, ::OI::Base::PROTO_REG_REGISTER_RES, res);
			return;
		}
		Binding b;
		b.name = req.name;
		b.id = req.id;
		b.ip = req.ip;
		b.port = req.port;
		b.value = req.ip + " " + req.port;
		coro::StartTask([this, sid, b](coro::Token) {
			return Register(sid, b);
		});
		return;
	}
	if (id == ::OI::Base::PROTO_REG_HEARTBEAT)
	{
		::OI::RpcData::RegHeartbeat req;
		req.deserialize(body);
		Binding* found = FindBinding(sid, req.name, req.id);
		if (!found)
		{
			// registry 重启过，或之前的登记没成。客户端收到失败会重新登记。
			::OI::RpcData::RegHeartbeatRes res;
			res.code = OI_ERR_FAIL;
			Reply(sid, ::OI::Base::PROTO_REG_HEARTBEAT_RES, res);
			return;
		}
		Binding b = *found;
		coro::StartTask([this, sid, b](coro::Token) {
			return Refresh(sid, b);
		});
		return;
	}
	if (id == ::OI::Base::PROTO_REG_PULL)
	{
		::OI::RpcData::RegPull req;
		req.deserialize(body);
		::OI::RpcData::RegPullRes res;
		res.code = OI_ERR_FAIL;
		if (_snapshot_ok)
		{
			res.code = OI_OK;
			for (size_t i = 0; i < _snapshot.size(); ++i)
			{
				const ::OI::Base::ServiceInstance& inst = _snapshot[i];
				bool want = req.names.empty();
				for (size_t n = 0; !want && n < req.names.size(); ++n)
				{
					want = req.names[n] == inst.name;
				}
				if (want)
				{
					res.list.push_back(::OI::RpcData::RegInstance(inst.name, inst.id, inst.ip, inst.port));
				}
			}
		}
		Reply(sid, ::OI::Base::PROTO_REG_PULL_RES, res);
		return;
	}
}
