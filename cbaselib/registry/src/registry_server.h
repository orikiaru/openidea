#pragma once
#include "oi_registry.h"
#include "framework_protocol/rpc_ids.h"
#include "oi_rpc_session_manager.h"
#include "oi_singleton.h"
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace OI
{
namespace Db
{
class Client;
}
}

// 注册进程。config=dynamic 是动态配置，接受登记，连接断了当场删掉。config=static 是静态配置，只读名单，不接受登记。
// 业务进程拉到的是地址名单，以及要不要上报自己。用哪套引擎只配在这个进程上。
// 动态配置的键都在 oi:{project.env}: 下面。svc 是名字集合，svc:<name> 是 id 集合，svc:<name>:<id> 是一份实例。
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
		std::string sOwner;
	};
	enum
	{
		REG_FAIL = -1,
		REG_TAKEN = 0,
		REG_OK = 1,
		REG_RENEWED = 2
	};
	OI::Coro::Task Boot();
	OI::Coro::Task BootMysql();
	int32_t OpenMysql();
	OI::Coro::Task Reload();
	OI::Coro::Task ReloadMysql();
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
	// 动态配置为真，业务进程要上报自己的地址。静态配置为假。
	bool m_bReport;
	std::unique_ptr<::OI::Db::Client> m_pMysql;
};
