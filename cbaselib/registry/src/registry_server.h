#pragma once
#include "oi_registry.h"
#include "framework_protocol/rpc_ids.h"
#include "oi_rpc_coro.h"
#include "oi_rpc_session_manager.h"
#include "oi_singleton.h"
#include <map>
#include <set>
#include <string>
#include <vector>

// 注册进程。登记写进 Redis，登记的连接断了当场删掉。按 pull_ms 整量拉一次整理好，客户端来拉时直接给。
// Redis 走 RedisClient 协程，等 Redis 的时候 poll 照转。
// 所有键都在 oi:{project.env}: 下面，不同项目、不同环境互不可见。
// svc 是名字集合，svc:<name> 是这个名字的 id 集合，svc:<name>:<id> 是一份实例，值是 "ip port" 或 "ip port 分区"。
class RegistryServer : public ::OI::Rpc::ProtoSessionManager
{
	DECLARE_SINGLETON(RegistryServer)
	RegistryServer();

  public:
	virtual ~RegistryServer();
	int32_t Init() override;
	const std::string GetIDString() override;
	void StartReload();

  protected:
	void OnAddSession(::OI::Net::Session* pSession) override;
	void OnDelSession(const ::OI::Net::Session* pSession) override;

  private:
	struct Binding
	{
		std::string sName;
		int32_t iId;
		std::string sIp;
		std::string sPort;
		std::string sZone;
		std::string sValue;
	};
	enum
	{
		REG_FAIL = -1,
		REG_TAKEN = 0,
		REG_OK = 1,
		REG_RENEWED = 2
	};
	OI::Coro::Task Boot();
	OI::Coro::Task Reload();
	::OI::Rpc::RpcTask Register(sid_t iSessionId, ::OI::RpcData::RegRegister stRequest);
	::OI::Rpc::RpcTask Refresh(sid_t iSessionId, ::OI::RpcData::RegHeartbeat stRequest);
	int32_t Pull(::OI::RpcData::RegPull& stRequest, ::OI::RpcData::RegPullRes& stResponse);
	OI::Coro::Task Withdraw(Binding stSecond);
	bool LoadSpace();
	std::string Key(const std::string& sTail) const;
	std::vector<std::string> EvalArgs(const char* pLua, const Binding& stSecond) const;
	Binding* FindBinding(sid_t iSessionId, const std::string& sName, int32_t iId);
	void PutSnapshot(const Binding& stSecond);
	void DropSnapshot(const std::string& sName, int32_t iId);

	std::set<sid_t> m_vAlive;
	std::map<sid_t, std::vector<Binding>> m_mBindings;
	std::vector<::OI::Base::ServiceInstance> m_vSnapshot;
	bool m_bSnapshotOk;
	bool m_bReloading;
	int32_t m_iPullMilliseconds;
	int32_t m_iExpireMilliseconds;
	std::string m_sSpace;
	std::string m_sPrefix;
};
