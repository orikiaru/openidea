#pragma once
#include "oi_coro.h"
#include "oi_blocking_pool.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace OI::Db
{
using Blob = std::vector<uint8_t>;
using Parameter = std::variant<std::nullptr_t, int64_t, uint64_t, double, std::string, Blob>;

struct Statement
{
	std::string sSql;
	std::vector<Parameter> vParameters;
};

struct Result
{
	std::vector<std::string> vColumns;
	std::vector<std::vector<std::optional<Blob>>> vRows;
	uint64_t iAffected = 0;
	uint64_t iInsertId = 0;
};

struct CleanupError
{
	bool bFailed = false;
	bool bException = false;
	std::string sStage;
	uint32_t iNativeCode = 0;
	std::string sSqlState;
	std::string sMessage;
};

struct Error
{
	std::string sStage;
	uint32_t iNativeCode = 0;
	std::string sSqlState;
	std::string sMessage;
	CleanupError stCleanup;
};

class Connection
{
  public:
	virtual ~Connection() = default;
	virtual int32_t Execute(const Statement& stStatement, Result& stResult, Error& stError) = 0;
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

class Client
{
	struct State;
	std::shared_ptr<State> m_pState;

  public:
	explicit Client(std::shared_ptr<Driver> pDriver, size_t iConnectionLimit,
	    OI::Thread::BlockingType iType = OI::Thread::BlockingType::Mysql);
	// Await all calls, then await Close before stopping the framework or blocking pool.
	// A rejected Close leaves the client closed; retry Close to release idle connections.
	// Without Close, destruction may fall back to synchronous connection cleanup.
	[[nodiscard]] OI::Coro::TaskResult<int32_t> Close() const;
	[[nodiscard]] OI::Coro::TaskResult<int32_t> Call(Statement stStatement, Result& stResult, Error& stError) const;
	// Only transactional DML (and reads) on transactional tables is supported.
	// The caller must not submit DDL, transaction control, stored procedures, or
	// statements with implicit commits. MySQL cannot roll those effects back.
	[[nodiscard]] OI::Coro::TaskResult<int32_t> Transaction(std::vector<Statement> vStatements, std::vector<Result>& vResults, Error& stError) const;
};
}
