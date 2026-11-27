#pragma once
#include "oi_db.h"

namespace OI::Db
{
struct BoundSql
{
	std::string sSql;
	std::vector<Parameter> vParameters;
};

class Connection
{
  public:
	virtual ~Connection() = default;
	virtual int32_t Execute(const BoundSql& stBoundSql, Result& stResult, Error& stError) = 0;
	// 事务控制语句（BEGIN、COMMIT、ROLLBACK）：不带参数，也不产生结果。默认按普通语句执行；
	// 预处理协议不支持事务控制的驱动，由驱动换成文本协议。
	virtual int32_t ExecuteControl(std::string_view sSql, Error& stError)
	{
		Result stResult;
		return Execute(BoundSql{std::string(sSql), {}}, stResult, stError);
	}
	virtual bool Healthy() const = 0;
	// Clear all session state before returning a connection to the pool.
	virtual bool Reset(Error& stError) = 0;
};

class Driver
{
  public:
	virtual ~Driver() = default;
	virtual std::unique_ptr<Connection> Connect(Error& stError) = 0;
};
}
