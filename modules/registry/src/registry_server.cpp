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

namespace
{

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

bool ValidSpacePart(const std::string& sText)
{
	if (sText.empty())
	{
		return false;
	}
	for (size_t iIndexLocal = 0; iIndexLocal < sText.size(); ++iIndexLocal)
	{
		char iCharacter = sText[iIndexLocal];
		bool bOk = (iCharacter >= 'a' && iCharacter <= 'z') || (iCharacter >= 'A' && iCharacter <= 'Z') || (iCharacter >= '0' && iCharacter <= '9') || iCharacter == '_' || iCharacter == '-';
		if (!bOk)
		{
			return false;
		}
	}
	return true;
}

// 值是 "ip port" 或 "ip port 分区"。名字、id、地址都一样就是同一个服务，重新登记直接续上；地址不一样才算被别人占了。
bool ParseValue(const std::string& sValue, std::string& sIp, std::string& sPort, std::string& sZone)
{
	size_t iFirst = sValue.find(' ');
	if (iFirst == std::string::npos || iFirst == 0)
	{
		return false;
	}
	size_t iSecond = sValue.find(' ', iFirst + 1);
	sIp = sValue.substr(0, iFirst);
	sPort = sValue.substr(iFirst + 1, iSecond == std::string::npos ? std::string::npos : iSecond - iFirst - 1);
	sZone.clear();
	if (iSecond != std::string::npos)
	{
		sZone = sValue.substr(iSecond + 1);
		if (sZone.empty() || sZone.find(' ') != std::string::npos)
		{
			return false;
		}
	}
	return !sIp.empty() && !sPort.empty();
}

int ReplyInt(const ::OI::Redis::RedisResult& stR)
{
	if (!stR || !stR.pReply || stR.pReply->type != REDIS_REPLY_INTEGER)
	{
		return -1;
	}
	return (int)stR.pReply->integer;
}

std::string StrOf(const redisReply* pR)
{
	return std::string(pR->str, (size_t)pR->len);
}

class RegistryServerSession : public ::OI::Net::NetSession
{
  public:
	explicit RegistryServerSession(::OI::Net::SessionManager* pManager)
	    : ::OI::Net::NetSession(pManager)
	{
	}

	virtual void OnDataOut()
	{
	}
	virtual void OnDataIn()
	{
		::OI::Base::ReadRegFrames(stInputBuffer, this, [this](uint32_t iIdLocal, ::OI::Base::BytesStream& stBody)
		{
			RegistryServer::GetInstance().Handle(this, iIdLocal, stBody);
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

RegistryServer::RegistryServer()
    : m_bSnapshotOk(false), m_bReloading(false), m_iPullMilliseconds(1000), m_iExpireMilliseconds(3000)
{
}
RegistryServer::~RegistryServer()
{
}

const std::string RegistryServer::GetIDString()
{
	return "Registry";
}

bool RegistryServer::LoadSpace()
{
	std::string sProject;
	std::string sEnvironment;
	::OI::Base::Config::GlobalConfig().GetAs("Registry.project", sProject);
	::OI::Base::Config::GlobalConfig().GetAs("Registry.env", sEnvironment);
	if (!ValidSpacePart(sProject) || !ValidSpacePart(sEnvironment))
	{
		fprintf(stderr, "registry space invalid: [Registry] project=\"%s\" env=\"%s\"，只能用字母、数字、_、-，且不能为空\n",
		    sProject.c_str(), sEnvironment.c_str());
		fflush(stderr);
		return false;
	}
	m_sSpace = sProject + "." + sEnvironment;
	m_sPrefix = "oi:{" + m_sSpace + "}:";
	return true;
}

std::string RegistryServer::Key(const std::string& sTail) const
{
	return m_sPrefix + sTail;
}

// 登记和续期共用：三个键是实例、id 集合、名字集合。
std::vector<std::string> RegistryServer::EvalArgs(const char* pLua, const Binding& stSecond) const
{
	return {"EVAL", pLua, "3", Key("svc:" + stSecond.sName + ":" + std::to_string(stSecond.iId)), Key("svc:" + stSecond.sName), Key("svc"),
	    stSecond.sValue, std::to_string(m_iExpireMilliseconds), std::to_string(stSecond.iId), stSecond.sName};
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
	TryGetConfig("Registry.pull_ms", m_iPullMilliseconds);
	if (m_iPullMilliseconds <= 0)
	{
		m_iPullMilliseconds = 1000;
	}
	TryGetConfig("Registry.expire_ms", m_iExpireMilliseconds);
	if (m_iExpireMilliseconds <= 0)
	{
		m_iExpireMilliseconds = 3000;
	}
	if (!::OI::Redis::RedisClient::GetInstance().Init())
	{
		fprintf(stderr, "registry needs [RedisClient] ip/port/type\n");
		fflush(stderr);
		return false;
	}
	::OI::Redis::RedisClient::GetInstance().StartClient();
	OI::Coro::StartTask([this](OI::Coro::Token)
	{
		return Boot();
	});
	LOG_TRACE("registry space=%s", m_sSpace.c_str());
	return true;
}

OI::Coro::Task RegistryServer::Boot()
{
	int iSpins = 0;
	while (!::OI::Redis::RedisClient::GetInstance().IsConnect())
	{
		if (++iSpins % 20 == 0)
		{
			LOG_TRACE("registry waiting redis");
		}
		co_await OI::Coro::yield(50);
	}
	StartReload();
	::OI::Timer::BaseTimerManager::GetInstance().AddTimer(new ReloadTimer(), m_iPullMilliseconds);
}

::OI::Net::Session* RegistryServer::CloneSession()
{
	return new RegistryServerSession(this);
}

void RegistryServer::OnAddSession(::OI::Net::Session* pSession)
{
	m_vAlive.insert(pSession->GetSessionID());
	LOG_INFO("RegistryServer::OnAddSession sid=%d", pSession->GetSessionID());
}

void RegistryServer::OnDelSession(const ::OI::Net::Session* pSession)
{
	sid_t iSessionIdLocal = pSession->GetSessionID();
	m_vAlive.erase(iSessionIdLocal);
	auto stIterator = m_mBindings.find(iSessionIdLocal);
	if (stIterator != m_mBindings.end())
	{
		std::vector<Binding> vList;
		vList.swap(stIterator->second);
		m_mBindings.erase(stIterator);
		for (size_t iIndexLocal = 0; iIndexLocal < vList.size(); ++iIndexLocal)
		{
			Binding stSecond = vList[iIndexLocal];
			DropSnapshot(stSecond.sName, stSecond.iId);
			LOG_TRACE("registry offline %s id=%d (connection closed)", stSecond.sName.c_str(), stSecond.iId);
			OI::Coro::StartTask([this, stSecond](OI::Coro::Token)
			{
				return Withdraw(stSecond);
			});
		}
		fflush(stdout);
	}
	LOG_INFO("RegistryServer::OnDelSession sid=%d", iSessionIdLocal);
}

RegistryServer::Binding* RegistryServer::FindBinding(sid_t iSessionId, const std::string& sNameLocal, int32_t iIdLocal)
{
	auto stIterator = m_mBindings.find(iSessionId);
	if (stIterator == m_mBindings.end())
	{
		return NULL;
	}
	for (size_t iIndexLocal = 0; iIndexLocal < stIterator->second.size(); ++iIndexLocal)
	{
		if (stIterator->second[iIndexLocal].sName == sNameLocal && stIterator->second[iIndexLocal].iId == iIdLocal)
		{
			return &stIterator->second[iIndexLocal];
		}
	}
	return NULL;
}

void RegistryServer::DropSnapshot(const std::string& sNameLocal, int32_t iIdLocal)
{
	for (size_t iIndexLocal = 0; iIndexLocal < m_vSnapshot.size(); ++iIndexLocal)
	{
		if (m_vSnapshot[iIndexLocal].sName == sNameLocal && m_vSnapshot[iIndexLocal].iId == iIdLocal)
		{
			m_vSnapshot.erase(m_vSnapshot.begin() + iIndexLocal);
			return;
		}
	}
}

void RegistryServer::PutSnapshot(const Binding& stSecond)
{
	DropSnapshot(stSecond.sName, stSecond.iId);
	::OI::Base::ServiceInstance stInst;
	stInst.sName = stSecond.sName;
	stInst.iId = stSecond.iId;
	stInst.sIp = stSecond.sIp;
	stInst.sPort = stSecond.sPort;
	stInst.sZone = stSecond.sZone;
	m_vSnapshot.push_back(stInst);
}

void RegistryServer::Reply(sid_t iSessionId, uint32_t iIdLocal, const ::OI::Rpc::Serialize& stMessage)
{
	if (!SendBytes(iSessionId, ::OI::Base::EncodeRegFrame(iIdLocal, stMessage)))
	{
		CloseSession(iSessionId);
	}
}

void RegistryServer::StartReload()
{
	if (m_bReloading)
	{
		return;
	}
	m_bReloading = true;
	OI::Coro::StartTask([this](OI::Coro::Token)
	{
		return Reload();
	});
}

OI::Coro::Task RegistryServer::Reload()
{
	std::vector<std::string> vArgumentsLocal = {"EVAL", kListAllLua, "1", Key("svc"), Key("svc:")};
	::OI::Redis::RedisResult stR = co_await ::OI::Redis::CallRedis(std::move(vArgumentsLocal));
	m_bReloading = false;
	if (!stR || !stR.pReply || stR.pReply->type != REDIS_REPLY_ARRAY)
	{
		if (m_bSnapshotOk)
		{
			LOG_TRACE("registry reload failed status=%d, keep clients as is", (int)stR.iStatus);
			fflush(stdout);
		}
		co_return;
	}
	std::vector<::OI::Base::ServiceInstance> vList;
	const redisReply* pArr = stR.pReply.get();
	for (size_t iIndexLocal = 0; iIndexLocal + 3 <= (size_t)pArr->elements; iIndexLocal += 3)
	{
		const redisReply* pName = pArr->element[iIndexLocal];
		const redisReply* pId = pArr->element[iIndexLocal + 1];
		const redisReply* pValue = pArr->element[iIndexLocal + 2];
		if (!pName || !pId || !pValue || pName->type != REDIS_REPLY_STRING || pId->type != REDIS_REPLY_STRING || pValue->type != REDIS_REPLY_STRING)
		{
			continue;
		}
		::OI::Base::ServiceInstance stInst;
		stInst.sName = StrOf(pName);
		stInst.iId = atoi(StrOf(pId).c_str());
		if (ParseValue(StrOf(pValue), stInst.sIp, stInst.sPort, stInst.sZone))
		{
			vList.push_back(stInst);
		}
	}
	m_vSnapshot.swap(vList);
	m_bSnapshotOk = true;
}

OI::Coro::Task RegistryServer::Register(sid_t iSessionId, Binding stSecond)
{
	std::vector<std::string> vArgumentsLocal = EvalArgs(kRegisterLua, stSecond);
	::OI::Redis::RedisResult stR = co_await ::OI::Redis::CallRedis(std::move(vArgumentsLocal));
	int iReturnCodeLocal = ReplyInt(stR);
	if (m_vAlive.count(iSessionId) == 0)
	{
		// 等 Redis 的时候连接断了。OnDelSession 不知道这一份，这里补删。
		if (iReturnCodeLocal == REG_OK)
		{
			co_await Withdraw(stSecond);
		}
		co_return;
	}
	::OI::RpcData::RegRegisterRes stResponseLocal;
	stResponseLocal.iCode = OI_ERR_FAIL;
	if (iReturnCodeLocal == REG_OK)
	{
		Binding* pFound = FindBinding(iSessionId, stSecond.sName, stSecond.iId);
		if (!pFound)
		{
			m_mBindings[iSessionId].push_back(stSecond);
		}
		else
		{
			*pFound = stSecond;
		}
		PutSnapshot(stSecond);
		stResponseLocal.iCode = OI_OK;
		LOG_TRACE("registry online %s id=%d %s:%s", stSecond.sName.c_str(), stSecond.iId, stSecond.sIp.c_str(), stSecond.sPort.c_str());
		fflush(stdout);
	}
	else if (iReturnCodeLocal == REG_TAKEN)
	{
		stResponseLocal.iCode = OI_ERR_EXIST;
		LOG_TRACE("registry %s id=%d already online, refuse sid=%d", stSecond.sName.c_str(), stSecond.iId, iSessionId);
		fflush(stdout);
	}
	else
	{
		LOG_TRACE("registry register %s id=%d redis failed status=%d", stSecond.sName.c_str(), stSecond.iId, (int)stR.iStatus);
	}
	Reply(iSessionId, ::OI::Base::PROTO_REG_REGISTER_RES, stResponseLocal);
}

OI::Coro::Task RegistryServer::Refresh(sid_t iSessionId, Binding stSecond)
{
	std::vector<std::string> vArgumentsLocal = EvalArgs(kRefreshLua, stSecond);
	::OI::Redis::RedisResult stR = co_await ::OI::Redis::CallRedis(std::move(vArgumentsLocal));
	int iReturnCodeLocal = ReplyInt(stR);
	Binding* pFound = m_vAlive.count(iSessionId) ? FindBinding(iSessionId, stSecond.sName, stSecond.iId) : NULL;
	if (!pFound || pFound->sValue != stSecond.sValue)
	{
		// 续期过程中连接断了或换了登记。续期可能把键又写回去了，按这份值删掉。
		if (iReturnCodeLocal == REG_OK || iReturnCodeLocal == REG_RENEWED)
		{
			co_await Withdraw(stSecond);
		}
		co_return;
	}
	::OI::RpcData::RegHeartbeatRes stResponseLocal;
	stResponseLocal.iCode = OI_ERR_FAIL;
	if (iReturnCodeLocal == REG_OK || iReturnCodeLocal == REG_RENEWED)
	{
		if (iReturnCodeLocal == REG_RENEWED)
		{
			PutSnapshot(stSecond);
			LOG_TRACE("registry %s id=%d registered again", stSecond.sName.c_str(), stSecond.iId);
			fflush(stdout);
		}
		stResponseLocal.iCode = OI_OK;
	}
	else if (iReturnCodeLocal == REG_TAKEN)
	{
		std::vector<Binding>& vList = m_mBindings[iSessionId];
		for (size_t iIndexLocal = 0; iIndexLocal < vList.size(); ++iIndexLocal)
		{
			if (vList[iIndexLocal].sName == stSecond.sName && vList[iIndexLocal].iId == stSecond.iId)
			{
				vList.erase(vList.begin() + iIndexLocal);
				break;
			}
		}
		// 新主人登记时已经换过快照。只摘还指着本连接地址的那份。
		for (size_t iIndexLocal = 0; iIndexLocal < m_vSnapshot.size(); ++iIndexLocal)
		{
			const ::OI::Base::ServiceInstance& stInst = m_vSnapshot[iIndexLocal];
			if (stInst.sName == stSecond.sName && stInst.iId == stSecond.iId && stInst.sIp == stSecond.sIp && stInst.sPort == stSecond.sPort)
			{
				m_vSnapshot.erase(m_vSnapshot.begin() + iIndexLocal);
				break;
			}
		}
		stResponseLocal.iCode = OI_ERR_EXIST;
		LOG_TRACE("registry %s id=%d taken by another process", stSecond.sName.c_str(), stSecond.iId);
		fflush(stdout);
	}
	Reply(iSessionId, ::OI::Base::PROTO_REG_HEARTBEAT_RES, stResponseLocal);
}

OI::Coro::Task RegistryServer::Withdraw(Binding stSecond)
{
	std::vector<std::string> vArgumentsLocal = {"EVAL", kWithdrawLua, "2", Key("svc:" + stSecond.sName + ":" + std::to_string(stSecond.iId)), Key("svc:" + stSecond.sName), stSecond.sValue, std::to_string(stSecond.iId)};
	co_await ::OI::Redis::CallRedis(std::move(vArgumentsLocal));
	// 撤销之前发出的整量拉取可能又把它带回快照。这时还没人重新登记，就再摘一次。
	for (const auto& [iSessionId, vBindings] : m_mBindings)
	{
		for (size_t iIndexLocal = 0; iIndexLocal < vBindings.size(); ++iIndexLocal)
		{
			if (vBindings[iIndexLocal].sName == stSecond.sName && vBindings[iIndexLocal].iId == stSecond.iId)
			{
				co_return;
			}
		}
	}
	DropSnapshot(stSecond.sName, stSecond.iId);
}

void RegistryServer::Handle(::OI::Net::Session* pSession, uint32_t iIdLocal, ::OI::Base::BytesStream& stBody)
{
	sid_t iSessionIdLocal = pSession->GetSessionID();
	if (iIdLocal == ::OI::Base::PROTO_REG_REGISTER)
	{
		::OI::RpcData::RegRegister stRequestLocal;
		stRequestLocal.deserialize(stBody);
		if (!ValidSpacePart(stRequestLocal.sName) || stRequestLocal.iId <= 0 || stRequestLocal.sIp.empty() || stRequestLocal.sPort.empty() || (!stRequestLocal.sZone.empty() && !ValidSpacePart(stRequestLocal.sZone)))
		{
			::OI::RpcData::RegRegisterRes stResponseLocal;
			stResponseLocal.iCode = OI_ERR_FAIL;
			Reply(iSessionIdLocal, ::OI::Base::PROTO_REG_REGISTER_RES, stResponseLocal);
			return;
		}
		Binding stSecond;
		stSecond.sName = stRequestLocal.sName;
		stSecond.iId = stRequestLocal.iId;
		stSecond.sIp = stRequestLocal.sIp;
		stSecond.sPort = stRequestLocal.sPort;
		stSecond.sZone = stRequestLocal.sZone;
		stSecond.sValue = stRequestLocal.sIp + " " + stRequestLocal.sPort;
		if (!stSecond.sZone.empty())
		{
			stSecond.sValue += " " + stSecond.sZone;
		}
		OI::Coro::StartTask([this, iSessionIdLocal, stSecond](OI::Coro::Token)
		{
			return Register(iSessionIdLocal, stSecond);
		});
		return;
	}
	if (iIdLocal == ::OI::Base::PROTO_REG_HEARTBEAT)
	{
		::OI::RpcData::RegHeartbeat stRequestLocal;
		stRequestLocal.deserialize(stBody);
		Binding* pFound = FindBinding(iSessionIdLocal, stRequestLocal.sName, stRequestLocal.iId);
		if (!pFound)
		{
			// registry 重启过，或之前的登记没成。客户端收到失败会重新登记。
			::OI::RpcData::RegHeartbeatRes stResponseLocal;
			stResponseLocal.iCode = OI_ERR_FAIL;
			Reply(iSessionIdLocal, ::OI::Base::PROTO_REG_HEARTBEAT_RES, stResponseLocal);
			return;
		}
		Binding stSecond = *pFound;
		OI::Coro::StartTask([this, iSessionIdLocal, stSecond](OI::Coro::Token)
		{
			return Refresh(iSessionIdLocal, stSecond);
		});
		return;
	}
	if (iIdLocal == ::OI::Base::PROTO_REG_PULL)
	{
		::OI::RpcData::RegPull stRequestLocal;
		stRequestLocal.deserialize(stBody);
		::OI::RpcData::RegPullRes stResponseLocal;
		stResponseLocal.iCode = OI_ERR_FAIL;
		if (m_bSnapshotOk)
		{
			stResponseLocal.iCode = OI_OK;
			for (size_t iIndexLocal = 0; iIndexLocal < m_vSnapshot.size(); ++iIndexLocal)
			{
				const ::OI::Base::ServiceInstance& stInst = m_vSnapshot[iIndexLocal];
				bool bWant = stRequestLocal.vNames.empty();
				for (size_t iNumber = 0; !bWant && iNumber < stRequestLocal.vNames.size(); ++iNumber)
				{
					bWant = stRequestLocal.vNames[iNumber] == stInst.sName;
				}
				if (bWant)
				{
					stResponseLocal.vList.push_back(::OI::RpcData::RegInstance(stInst.sName, stInst.iId, stInst.sIp, stInst.sPort, stInst.sZone));
				}
			}
		}
		Reply(iSessionIdLocal, ::OI::Base::PROTO_REG_PULL_RES, stResponseLocal);
		return;
	}
}
