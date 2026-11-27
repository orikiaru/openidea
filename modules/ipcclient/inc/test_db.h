#pragma once
#include "oi_coro.h"
#include "oi_mysql.h"
#include "oi_singleton.h"
#include "oi_rpcdata_dbsample.h"
#include <string>

class TestDb
{
	DECLARE_SINGLETON(TestDb)
	TestDb() = default;
	bool m_bEnabled = false;
	std::string m_sWakeCase;
	OI::Coro::Task Run();
	static OI::Coro::TaskResult<int32_t> RunWakeBenchmark(bool bBlockingOnly);
	// 不连库：字段表、按成员指针取列、PB 往返和固定小端字节、JSON。
	int32_t CheckRowMapping();
	int32_t CheckCellPredicates();
	// 真表：生成语句建表、写入、按主键读改删、upsert，NULL 和坏值不改对象。
	OI::Coro::TaskResult<int32_t> CheckMysqlRows(OI::Db::Client& stMysql, const std::string& sBaseTable, const std::string& sDatabase);
	OI::Coro::TaskResult<int32_t> CheckDefaultTable(OI::Db::Client& stMysql, const std::string& sExplicitTable);
	OI::Coro::TaskResult<int32_t> CheckBatchRead(OI::Db::Client& stMysql, const std::string& sTable, const std::vector<OI::RpcData::DbSample>& vExpected);
	OI::Coro::TaskResult<int32_t> CheckBatchCrud(OI::Db::Client& stMysql, const std::string& sTable);
	OI::Coro::TaskResult<int32_t> CheckColumnTypes(OI::Db::Client& stMysql, const std::string& sTable);
	OI::Coro::TaskResult<int32_t> ReadRequiredId(OI::Db::Client& stMysql, std::string sSql, uint32_t& iId);
#ifdef OI_ENABLE_MYSQL
	OI::Coro::TaskResult<int32_t> CheckDbErrors(OI::Db::Client& stMysql, const OI::Mysql::Config& stConfig);
#endif
	void Expect(int32_t& iBad, bool bOk, const char* pLabel);
	::OI::RpcData::DbSample MakeFullRow();
	::OI::RpcData::DbSample MakeEdgeRow();
	bool SameRow(const ::OI::RpcData::DbSample& stFirst, const ::OI::RpcData::DbSample& stSecond);

  public:
	void Load();
	void Start();
};
