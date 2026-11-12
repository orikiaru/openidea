#pragma once
#include "oi_coro.h"
#include "oi_db.h"
#include "oi_singleton.h"
#include "oi_rpcdata_dbsample.h"
#include <string>

class DbMgr
{
	DECLARE_SINGLETON(DbMgr)
	DbMgr() = default;
	bool m_bEnabled = false;
	OI::Coro::Task Run();
	// 不连库：字段表、按成员指针取列、生成的建表和增删改查语句、PB 往返和固定小端字节、JSON。
	int32_t CheckRowMapping();
	// 真表：生成语句建表、写入、按主键读改删、upsert，NULL 和坏值不改对象。
	OI::Coro::TaskResult<int32_t> CheckMysqlRows(OI::Db::Client& stMysql, const std::string& sTable);
	// 按 stRow 的主键读整行写回 stRow。查到的不是一行返回 OI_ERR_NOT_FOUND，解不开返回 OI_ERR_DECODE 且 stRow 不变。
	OI::Coro::TaskResult<int32_t> LoadRow(OI::Db::Client& stMysql, const std::string& sTable, ::OI::RpcData::DbSample& stRow);
	void Expect(int32_t& iBad, bool bOk, const char* pLabel);
	::OI::RpcData::DbSample MakeFullRow();
	::OI::RpcData::DbSample MakeEdgeRow();
	bool SameRow(const ::OI::RpcData::DbSample& stFirst, const ::OI::RpcData::DbSample& stSecond);
	::OI::Db::Blob TextBlob(const std::string& sText);
	// 参数换成 MySQL 以文本取回的单元格：数值写成十进制，NULL 是空 optional。
	std::optional<::OI::Db::Blob> CellOf(const ::OI::Db::Parameter& stParameter);
	::OI::Db::CellMap CellsOf(const ::OI::Db::ParameterMap& mParameters);

  public:
	void Load();
	void Start();
};
