#include "oi_framework_timer.h"
#include "registry_server.h"
#include "oi_proto_id.h"
#include "framework_protocol/oi_rpcdata_regregister.h"
#include "framework_protocol/oi_rpcdata_regregisterres.h"
#include "framework_protocol/oi_rpcdata_regheartbeat.h"
#include "framework_protocol/oi_rpcdata_regheartbeatres.h"
#include "framework_protocol/oi_rpcdata_reginstance.h"
#include "framework_protocol/oi_rpcdata_regpull.h"
#include "framework_protocol/oi_rpcdata_regpullres.h"
#include "oi_net_session.h"
#include "oi_redis.h"
#include "oi_config.h"
#include "oi_log.h"
#include "oi_timer_manager.h"
#include "oi_blocking_pool.h"
#include "oi_db.h"
#include "oi_db_row.h"
#include "oi_db_table.h"
#include "oi_mysql.h"
#include "framework_protocol/oi_rpcdata_servicelease.h"
#include <uv.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

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
    "if v and ARGV[6] == '1' and redis.call('GET', KEYS[4]) ~= ARGV[5] then return 0 end "
    "redis.call('SET', KEYS[1], ARGV[1], 'PX', ARGV[2]) redis.call('SET', KEYS[4], ARGV[5], 'PX', ARGV[2]) "
    "redis.call('SADD', KEYS[2], ARGV[3]) redis.call('SADD', KEYS[3], ARGV[4]) return 1";

// 还是自己的就延长。过期了没人占就重新占上。被别人占了不动它。
const char* kRefreshLua =
    "local v = redis.call('GET', KEYS[1]) "
    "if v == ARGV[1] and redis.call('GET', KEYS[4]) == ARGV[5] then redis.call('PEXPIRE', KEYS[1], ARGV[2]) redis.call('PEXPIRE', KEYS[4], ARGV[2]) "
    "  redis.call('SADD', KEYS[2], ARGV[3]) redis.call('SADD', KEYS[3], ARGV[4]) return 1 end "
    "if v then return 0 end "
    "redis.call('SET', KEYS[1], ARGV[1], 'PX', ARGV[2]) redis.call('SET', KEYS[4], ARGV[5], 'PX', ARGV[2]) "
    "redis.call('SADD', KEYS[2], ARGV[3]) redis.call('SADD', KEYS[3], ARGV[4]) return 2";

const char* kWithdrawLua =
    "if redis.call('GET', KEYS[3]) ~= ARGV[1] or redis.call('GET', KEYS[1]) ~= ARGV[3] then return 0 end "
    "redis.call('DEL', KEYS[1], KEYS[3]) redis.call('SREM', KEYS[2], ARGV[2]) "
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

class ReloadTimer : public ::OI::Timer::BaseTimer
{
  public:
	bool OnTimer() override
	{
		RegistryServer::GetInstance().StartReload();
		return true;
	}
};
}

RegistryServer::RegistryServer()
    : m_bSnapshotOk(false), m_bReloading(false), m_iPullMilliseconds(1000), m_iExpireMilliseconds(3000), m_bReport(true)
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

// 实例地址、id 集合、名字集合和连接租约在同一 Redis 槽。
std::vector<std::string> RegistryServer::EvalArgs(const char* pLua, const Binding& stSecond) const
{
	const std::string sInstance = stSecond.sName + ":" + std::to_string(stSecond.iId);
	return {"EVAL", pLua, "4", Key("svc:" + sInstance), Key("svc:" + stSecond.sName), Key("svc"), Key("owner:" + sInstance),
	    stSecond.sValue, std::to_string(m_iExpireMilliseconds), std::to_string(stSecond.iId), stSecond.sName, stSecond.sOwner};
}

int32_t RegistryServer::Init()
{
	int32_t iCode = ::OI::Rpc::ProtoSessionManager::Init();
	if (iCode != OI_OK)
	{
		return iCode;
	}
	std::string sRetired;
	if (::OI::Base::Config::GlobalConfig().GetAs("Registry.store", sRetired))
	{
		fprintf(stderr, "registry 不再读 Registry.store，改用 Registry.config，取值 dynamic 或 static\n");
		fflush(stderr);
		return OI_ERR_INVALID_ARGUMENT;
	}
	std::string sConfig = "dynamic";
	::OI::Base::Config::GlobalConfig().GetAs("Registry.config", sConfig);
	if (sConfig.empty() || sConfig == "dynamic")
	{
		m_bReport = true;
	}
	else if (sConfig == "static")
	{
		m_bReport = false;
	}
	else
	{
		fprintf(stderr, "registry config=%s 只能是 dynamic 或 static\n", sConfig.c_str());
		fflush(stderr);
		return OI_ERR_INVALID_ARGUMENT;
	}
	RegisterRpcHandler([](const ::OI::Rpc::RpcContext& stContext, ::OI::RpcData::RegRegister& stRequest, ::OI::RpcData::RegRegisterRes&)
	{
		return RegistryServer::GetInstance().Register(stContext.iSessionId, stRequest);
	});
	RegisterRpcHandler([](const ::OI::Rpc::RpcContext& stContext, ::OI::RpcData::RegHeartbeat& stRequest, ::OI::RpcData::RegHeartbeatRes&)
	{
		return RegistryServer::GetInstance().Refresh(stContext.iSessionId, stRequest);
	});
	RegisterRpcHandler([](::OI::RpcData::RegPull& stRequest, ::OI::RpcData::RegPullRes& stResponse)
	{
		return RegistryServer::GetInstance().Pull(stRequest, stResponse);
	});
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
	if (!m_bReport)
	{
		iCode = OpenMysql();
		if (iCode != OI_OK)
		{
			return iCode;
		}
		OI::Coro::StartTask([this](OI::Coro::Token)
		{
			return BootMysql();
		});
		LOG_TRACE("registry config=static");
		return OI_OK;
	}
	if (!LoadSpace())
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	iCode = ::OI::Redis::RedisClient::GetInstance().Init();
	if (iCode != OI_OK)
	{
		fprintf(stderr, "registry needs [RedisClient] ip/port/type\n");
		fflush(stderr);
		return iCode;
	}
	::OI::Redis::RedisClient::GetInstance().StartClient();
	OI::Coro::StartTask([this](OI::Coro::Token)
	{
		return Boot();
	});
	LOG_TRACE("registry space=%s", m_sSpace.c_str());
	return OI_OK;
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
	::OI::Timer::FrameworkTimer::AddTimer(new ReloadTimer(), m_iPullMilliseconds);
}

OI::Coro::Task RegistryServer::BootMysql()
{
	StartReload();
	::OI::Timer::FrameworkTimer::AddTimer(new ReloadTimer(), m_iPullMilliseconds);
	co_return;
}

int32_t RegistryServer::OpenMysql()
{
#ifndef OI_ENABLE_MYSQL
	fprintf(stderr, "registry config=static 需要带 MySQL 客户端的构建\n");
	fflush(stderr);
	LOG_ERROR("registry config=static requires the mysql client");
	return OI_ERR_FAIL;
#else
	OI::Mysql::Config stConfig;
	::OI::Base::Config::GlobalConfig().GetAs("Registry.db_host", stConfig.sHost);
	if (stConfig.sHost.empty())
	{
		stConfig.sHost = "127.0.0.1";
	}
	int32_t iPort = 3306;
	::OI::Base::Config::GlobalConfig().GetAs("Registry.db_port", iPort);
	if (iPort <= 0 || iPort > 65535)
	{
		fprintf(stderr, "registry db_port 不合法\n");
		fflush(stderr);
		return OI_ERR_INVALID_ARGUMENT;
	}
	stConfig.iPort = static_cast<uint32_t>(iPort);
	if (!::OI::Base::Config::GlobalConfig().GetAs("Registry.db_user", stConfig.sUser) || stConfig.sUser.empty() || !::OI::Base::Config::GlobalConfig().GetAs("Registry.db_database", stConfig.sDatabase) || stConfig.sDatabase.empty())
	{
		fprintf(stderr, "registry config=static 需要 Registry.db_user 和 Registry.db_database\n");
		fflush(stderr);
		LOG_ERROR("registry static missing user or database");
		return OI_ERR_INVALID_ARGUMENT;
	}
	::OI::Base::Config::GlobalConfig().GetAs("Registry.db_password", stConfig.sPassword);
	::OI::Base::Config::GlobalConfig().GetAs("Registry.db_tls_ca", stConfig.sTlsCaFile);
	const int32_t iPool = ::OI::Thread::ConfigureBlockingPool(::OI::Thread::BlockingType::Mysql, 1, 16);
	if (iPool != OI_OK && iPool != OI_ERR_BUSY)
	{
		return iPool;
	}
	m_pMysql = std::make_unique<OI::Db::Client>(OI::Mysql::MakeDriver(std::move(stConfig)), 1);
	return OI_OK;
#endif
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
			if (stSecond.bSuperseded)
			{
				continue;
			}
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

void RegistryServer::StartReload()
{
	if (m_bReloading)
	{
		return;
	}
	m_bReloading = true;
	if (!m_bReport)
	{
		OI::Coro::StartTask([this](OI::Coro::Token)
		{
			return ReloadMysql();
		});
		return;
	}
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

OI::Coro::Task RegistryServer::ReloadMysql()
{
#ifndef OI_ENABLE_MYSQL
	m_bReloading = false;
	co_return;
#else
	if (m_pMysql == nullptr)
	{
		m_bReloading = false;
		co_return;
	}
	OI::Db::Statement stSelect;
	const std::vector<OI::Db::ColumnSpec> vColumns = OI::Db::DescribeRow(::OI::RpcData::ServiceLease{});
	std::string sNames;
	for (const OI::Db::ColumnSpec& stColumn : vColumns)
	{
		if (!sNames.empty())
		{
			sNames += ",";
		}
		sNames += OI::Db::QuoteName(stColumn.sName);
	}
	if (sNames.empty())
	{
		m_bReloading = false;
		LOG_ERROR("registry static select build failed");
		co_return;
	}
	stSelect.sSql = "SELECT " + sNames + " FROM " + OI::Db::QuoteName("oi_service_instance");
	OI::Db::Result stResult;
	OI::Db::Error stError;
	const int32_t iCode = co_await m_pMysql->Call(std::move(stSelect), stResult, stError);
	m_bReloading = false;
	if (iCode != OI_OK)
	{
		const std::string sMessage = stError.sMessage.size() > 200 ? stError.sMessage.substr(0, 200) : stError.sMessage;
		LOG_ERROR("registry static pull stage=%s native=%u message=%s", stError.sStage.c_str(), stError.iNativeCode, sMessage.c_str());
		co_return;
	}
	std::vector<::OI::Base::ServiceInstance> vList;
	vList.reserve(stResult.vRows.size());
	for (size_t iRow = 0; iRow < stResult.vRows.size(); ++iRow)
	{
		::OI::Db::CellMap mCells;
		::OI::RpcData::ServiceLease stRow;
		if (!::OI::Db::RowCells(stResult, iRow, mCells) || !stRow.FromDbMap(mCells) || stRow.sName.empty() || stRow.iId <= 0 || stRow.sIp.empty() || stRow.sPort.empty())
		{
			LOG_ERROR("registry static pull bad row=%zu", iRow);
			co_return;
		}
		::OI::Base::ServiceInstance stInst;
		stInst.sName = stRow.sName;
		stInst.iId = stRow.iId;
		stInst.sIp = stRow.sIp;
		stInst.sPort = stRow.sPort;
		stInst.sZone = stRow.sZone;
		vList.push_back(std::move(stInst));
	}
	m_vSnapshot.swap(vList);
	m_bSnapshotOk = true;
	co_return;
#endif
}

::OI::Rpc::RpcTask RegistryServer::Register(sid_t iSessionId, ::OI::RpcData::RegRegister stRequest)
{
	if (!m_bReport)
	{
		LOG_ERROR("registry does not accept register");
		co_return OI_ERR_FAIL;
	}
	if (!ValidSpacePart(stRequest.sName) || stRequest.iId <= 0 || stRequest.sIp.empty() || stRequest.sPort.empty() || (!stRequest.sZone.empty() && !ValidSpacePart(stRequest.sZone)))
	{
		co_return OI_ERR_FAIL;
	}
	if (!m_vAlive.contains(iSessionId))
	{
		co_return OI_ERR_CONNECT;
	}
	Binding stSecond;
	stSecond.sName = stRequest.sName;
	stSecond.iId = stRequest.iId;
	stSecond.sIp = stRequest.sIp;
	stSecond.sPort = stRequest.sPort;
	stSecond.sZone = stRequest.sZone;
	stSecond.sValue = stRequest.sIp + " " + stRequest.sPort;
	if (!stSecond.sZone.empty())
	{
		stSecond.sValue += " " + stSecond.sZone;
	}
	// 实例值保留原格式，相同地址的两次登记用独立租约区分。
	bool bExisting = false;
	if (const Binding* pPrevious = FindBinding(iSessionId, stSecond.sName, stSecond.iId))
	{
		if (pPrevious->bSuperseded)
		{
			co_return OI_ERR_EXIST;
		}
		bExisting = true;
		stSecond.sOwner = pPrevious->sOwner;
	}
	else
	{
		stSecond.sOwner.resize(16);
		if (uv_random(nullptr, nullptr, stSecond.sOwner.data(), stSecond.sOwner.size(), 0, nullptr) != 0)
		{
			LOG_ERROR("registry lease token generation failed sid=%d", iSessionId);
			co_return OI_ERR_FAIL;
		}
	}
	std::vector<std::string> vArgumentsLocal = EvalArgs(kRegisterLua, stSecond);
	vArgumentsLocal.emplace_back(bExisting ? "1" : "0");
	::OI::Redis::RedisResult stR = co_await ::OI::Redis::CallRedis(std::move(vArgumentsLocal));
	int iReturnCodeLocal = ReplyInt(stR);
	if (m_vAlive.count(iSessionId) == 0)
	{
		// 等 Redis 的时候连接断了。OnDelSession 不知道这一份，这里补删。
		if (iReturnCodeLocal == REG_OK)
		{
			co_await Withdraw(stSecond);
		}
		co_return OI_ERR_CONNECT;
	}
	Binding* pFound = FindBinding(iSessionId, stSecond.sName, stSecond.iId);
	if (pFound && pFound->bSuperseded)
	{
		co_return OI_ERR_EXIST;
	}
	if (iReturnCodeLocal == REG_OK)
	{
		for (auto& [iOtherSessionId, vBindings] : m_mBindings)
		{
			if (iOtherSessionId != iSessionId)
			{
				for (auto& stBinding : vBindings)
				{
					if (!stBinding.bSuperseded && stBinding.sName == stSecond.sName && stBinding.iId == stSecond.iId)
					{
						stBinding.bSuperseded = true;
						LOG_TRACE("registry takeover %s id=%d old_sid=%d new_sid=%d", stSecond.sName.c_str(), stSecond.iId, iOtherSessionId, iSessionId);
					}
				}
			}
		}
		if (!pFound)
		{
			m_mBindings[iSessionId].push_back(stSecond);
		}
		else
		{
			*pFound = stSecond;
		}
		PutSnapshot(stSecond);
		LOG_TRACE("registry online %s id=%d %s:%s", stSecond.sName.c_str(), stSecond.iId, stSecond.sIp.c_str(), stSecond.sPort.c_str());
		fflush(stdout);
		co_return OI_OK;
	}
	else if (iReturnCodeLocal == REG_TAKEN)
	{
		LOG_TRACE("registry %s id=%d already online, refuse sid=%d", stSecond.sName.c_str(), stSecond.iId, iSessionId);
		fflush(stdout);
		co_return OI_ERR_EXIST;
	}
	else
	{
		LOG_TRACE("registry register %s id=%d redis failed status=%d", stSecond.sName.c_str(), stSecond.iId, (int)stR.iStatus);
	}
	co_return OI_ERR_FAIL;
}

::OI::Rpc::RpcTask RegistryServer::Refresh(sid_t iSessionId, ::OI::RpcData::RegHeartbeat stRequest)
{
	if (!m_bReport)
	{
		LOG_ERROR("registry does not accept heartbeat");
		co_return OI_ERR_FAIL;
	}
	Binding stSecond;
	{
		Binding* pBinding = FindBinding(iSessionId, stRequest.sName, stRequest.iId);
		if (!pBinding)
		{
			co_return OI_ERR_FAIL;
		}
		if (pBinding->bSuperseded)
		{
			co_return OI_ERR_EXIST;
		}
		stSecond = *pBinding;
	}
	std::vector<std::string> vArgumentsLocal = EvalArgs(kRefreshLua, stSecond);
	::OI::Redis::RedisResult stR = co_await ::OI::Redis::CallRedis(std::move(vArgumentsLocal));
	int iReturnCodeLocal = ReplyInt(stR);
	Binding* pCurrent = FindBinding(iSessionId, stSecond.sName, stSecond.iId);
	if (pCurrent && pCurrent->bSuperseded)
	{
		co_return OI_ERR_EXIST;
	}
	if (!pCurrent || pCurrent->sOwner != stSecond.sOwner)
	{
		// 续期过程中连接断了或换了登记。续期可能把键又写回去了，按这份值删掉。
		if (iReturnCodeLocal == REG_OK || iReturnCodeLocal == REG_RENEWED)
		{
			co_await Withdraw(stSecond);
		}
		co_return OI_ERR_CONNECT;
	}
	if (iReturnCodeLocal == REG_OK || iReturnCodeLocal == REG_RENEWED)
	{
		if (iReturnCodeLocal == REG_RENEWED)
		{
			PutSnapshot(stSecond);
			LOG_TRACE("registry %s id=%d registered again", stSecond.sName.c_str(), stSecond.iId);
			fflush(stdout);
		}
		co_return OI_OK;
	}
	else if (iReturnCodeLocal == REG_TAKEN)
	{
		// Redis 可能先看到接管，登记协程稍后才发布快照。保留旧会话的失效记录直到断开。
		pCurrent->bSuperseded = true;
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
		LOG_TRACE("registry %s id=%d taken by another process", stSecond.sName.c_str(), stSecond.iId);
		fflush(stdout);
		co_return OI_ERR_EXIST;
	}
	co_return OI_ERR_FAIL;
}

OI::Coro::Task RegistryServer::Withdraw(Binding stSecond)
{
	const std::string sInstance = stSecond.sName + ":" + std::to_string(stSecond.iId);
	std::vector<std::string> vArgumentsLocal = {"EVAL", kWithdrawLua, "3", Key("svc:" + sInstance), Key("svc:" + stSecond.sName), Key("owner:" + sInstance), stSecond.sOwner, std::to_string(stSecond.iId), stSecond.sValue};
	auto stResult = co_await ::OI::Redis::CallRedis(std::move(vArgumentsLocal));
	if (ReplyInt(stResult) != REG_OK)
	{
		co_return;
	}
	// 撤销之前发出的整量拉取可能又把它带回快照。这时还没人重新登记，就再摘一次。
	for (const auto& [iSessionId, vBindings] : m_mBindings)
	{
		for (size_t iIndexLocal = 0; iIndexLocal < vBindings.size(); ++iIndexLocal)
		{
			if (!vBindings[iIndexLocal].bSuperseded && vBindings[iIndexLocal].sName == stSecond.sName && vBindings[iIndexLocal].iId == stSecond.iId)
			{
				co_return;
			}
		}
	}
	DropSnapshot(stSecond.sName, stSecond.iId);
}

int32_t RegistryServer::Pull(::OI::RpcData::RegPull& stRequest, ::OI::RpcData::RegPullRes& stResponse)
{
	stResponse.bStatic = !m_bReport;
	if (!m_bSnapshotOk)
	{
		return OI_ERR_FAIL;
	}
	for (const auto& stInst : m_vSnapshot)
	{
		bool bWant = stRequest.vNames.empty();
		for (const auto& sName : stRequest.vNames)
		{
			if (sName == stInst.sName)
			{
				bWant = true;
				break;
			}
		}
		if (bWant)
		{
			stResponse.vList.push_back(::OI::RpcData::RegInstance(stInst.sName, stInst.iId, stInst.sIp, stInst.sPort, stInst.sZone));
		}
	}
	return OI_OK;
}
