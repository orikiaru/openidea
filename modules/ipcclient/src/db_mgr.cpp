#include "db_mgr.h"
#include "oi_blocking_pool.h"
#include "oi_config.h"
#include "oi_db.h"
#include "oi_errno.h"
#include "oi_mysql.h"
#include "oi_lock.h"
#include "oi_log.h"
#include "test_board.h"
#include <atomic>
#include <cstdlib>
#include <memory>
#include <limits>
#include <random>
#include <stdexcept>

namespace
{
struct FakeState
{
	OI::Thread::NativeMutex stMutex;
	std::vector<std::pair<int32_t, std::string>> vEvents;
};

class FakeConnection : public OI::Db::Connection
{
	int32_t m_iId;
	std::shared_ptr<FakeState> m_pState;

  public:
	FakeConnection(int32_t iId, std::shared_ptr<FakeState> pState)
	    : m_iId(iId), m_pState(std::move(pState))
	{
	}
	int32_t Execute(const OI::Db::Statement& stStatement, OI::Db::Result& stResult, OI::Db::Error& stError) override
	{
		{
			OI::Thread::NativeMutexGuard stGuard(m_pState->stMutex);
			m_pState->vEvents.emplace_back(m_iId, stStatement.sSql);
		}
		if (stStatement.sSql == "throw")
		{
			throw std::runtime_error("fake failure");
		}
		if (stStatement.sSql == "fail")
		{
			stError.sStage = "fake";
			return OI_ERR_CONNECT;
		}
		stResult.iAffected = static_cast<uint64_t>(m_iId);
		return OI_OK;
	}
	bool Healthy() const override
	{
		return true;
	}
	bool Reset(OI::Db::Error&) override
	{
		return true;
	}
};

class FakeDriver : public OI::Db::Driver
{
  public:
	std::atomic<int32_t> iConnections{0};
	std::shared_ptr<FakeState> pState = std::make_shared<FakeState>();
	std::unique_ptr<OI::Db::Connection> Connect(OI::Db::Error&) override
	{
		return std::make_unique<FakeConnection>(iConnections.fetch_add(1) + 1, pState);
	}
};
}

void DbMgr::Load()
{
	TryGetConfig("Test.db", m_bEnabled);
	TryGetConfig("Test.db_mysql", m_bMysql);
}

void DbMgr::Start()
{
	if (!m_bEnabled)
	{
		return;
	}
	TestBoard::GetInstance().Arm();
	OI::Coro::StartTask([](OI::Coro::Token)
	{
		return DbMgr::GetInstance().Run();
	});
}

OI::Coro::Task DbMgr::Run()
{
	int32_t iBad = 0;
	const int32_t iConfig = OI::Thread::ConfigureBlockingPool(OI::Thread::BlockingType::Mysql, 4, 64);
	if (iConfig != OI_OK)
	{
		LOG_TRACE("db blocking pool configure failed code=%d", iConfig);
		TestBoard::GetInstance().Done(1);
		co_return;
	}
	OI::Db::Result stResult;
	OI::Db::Error stError;
	int32_t iStatus = OI_OK;
	if (!m_bMysql)
	{
		auto pDriver = std::make_shared<FakeDriver>();
		OI::Db::Client stClient(pDriver, 1);
		try
		{
			OI::Db::Statement stOne{"one", {}};
			iStatus = co_await stClient.Call(stOne, stResult, stError);
			if (iStatus != OI_OK || stResult.iAffected != 1)
			{
				LOG_TRACE("db fake initial call failed status=%d", iStatus);
				++iBad;
			}
			OI::Db::Statement stTwo{"two", {}};
			iStatus = co_await stClient.Call(stTwo, stResult, stError);
			if (iStatus != OI_OK || stResult.iAffected != 1 || pDriver->iConnections.load() != 1)
			{
				LOG_TRACE("db fake connection reuse failed status=%d", iStatus);
				++iBad;
			}
			std::vector<OI::Db::Result> vResults;
			std::vector<OI::Db::Statement> vCommit{stOne, stTwo};
			iStatus = co_await stClient.Transaction(std::move(vCommit), vResults, stError);
			if (iStatus != OI_OK || vResults.size() != 2 || vResults[0].iAffected != 1 || vResults[1].iAffected != 1)
			{
				LOG_TRACE("db fake commit results failed status=%d", iStatus);
				++iBad;
			}
			if (!vResults.empty())
			{
				vResults[0].iAffected = 99;
			}
			OI::Db::Statement stFail{"fail", {}};
			std::vector<OI::Db::Statement> vRollback{stOne, stFail};
			iStatus = co_await stClient.Transaction(std::move(vRollback), vResults, stError);
			if (iStatus == OI_OK || vResults.size() != 2 || vResults[0].iAffected != 99 || stError.sStage != "fake")
			{
				LOG_TRACE("db fake rollback output failed status=%d", iStatus);
				++iBad;
			}
			stResult.iAffected = 99;
			iStatus = co_await stClient.Call(stFail, stResult, stError);
			if (iStatus == OI_OK || stResult.iAffected != 99 || stError.sStage != "fake")
			{
				LOG_TRACE("db fake failed call changed output status=%d", iStatus);
				++iBad;
			}
			bool bCaught = false;
			try
			{
				OI::Db::Statement stThrow{"throw", {}};
				iStatus = co_await stClient.Call(std::move(stThrow), stResult, stError);
			}
			catch (const std::exception&)
			{
				bCaught = true;
			}
			if (!bCaught || stResult.iAffected != 99)
			{
				LOG_TRACE("db fake exception output failed caught=%d", bCaught);
				++iBad;
			}
			OI::Db::Statement stAfter{"after", {}};
			iStatus = co_await stClient.Call(std::move(stAfter), stResult, stError);
			if (iStatus != OI_OK || stResult.iAffected != 2 || pDriver->iConnections.load() != 2)
			{
				LOG_TRACE("db fake exception eviction failed status=%d", iStatus);
				++iBad;
			}
			const std::vector<std::pair<int32_t, std::string>> vExpected = {
			    {1, "one"}, {1, "two"}, {1, "BEGIN"}, {1, "one"}, {1, "two"}, {1, "COMMIT"},
			    {1, "BEGIN"}, {1, "one"}, {1, "fail"}, {1, "ROLLBACK"}, {1, "fail"}, {1, "throw"}, {2, "after"}};
			{
				OI::Thread::NativeMutexGuard stGuard(pDriver->pState->stMutex);
				if (pDriver->pState->vEvents != vExpected)
				{
					LOG_TRACE("db fake transaction sequence or connection mismatch");
					++iBad;
				}
			}
		}
		catch (const std::exception&)
		{
			LOG_TRACE("db fake unexpected exception");
			++iBad;
		}
		try
		{
			iStatus = co_await stClient.Close();
			if (iStatus != OI_OK)
			{
				LOG_TRACE("db fake close failed status=%d", iStatus);
				++iBad;
			}
		}
		catch (const std::exception&)
		{
			LOG_TRACE("db fake close raised exception");
			++iBad;
		}
		LOG_TRACE("db fake done bad=%d", iBad);
		TestBoard::GetInstance().Done(iBad ? 1 : 0);
		co_return;
	}
#ifdef OI_ENABLE_MYSQL
	const char* pHost = std::getenv("OI_TEST_MYSQL_HOST");
	const char* pPort = std::getenv("OI_TEST_MYSQL_PORT");
	const char* pUser = std::getenv("OI_TEST_MYSQL_USER");
	const char* pPassword = std::getenv("OI_TEST_MYSQL_PASSWORD");
	const char* pDatabase = std::getenv("OI_TEST_MYSQL_DATABASE");
	if (pHost && pPort && pUser && pPassword && pDatabase)
	{
		OI::Mysql::Config stConfig;
		stConfig.sHost = pHost;
		stConfig.iPort = static_cast<uint32_t>(std::strtoul(pPort, nullptr, 10));
		stConfig.sUser = pUser;
		stConfig.sPassword = pPassword;
		stConfig.sDatabase = pDatabase;
		std::random_device stRandom;
		const std::string sTable = "oi_db_reg_" + std::to_string(stRandom()) + "_" + std::to_string(stRandom());
		const std::string sQuotedTable = "`" + sTable + "`";
		bool bTableCreated = false;
		OI::Db::Client stMysql(OI::Mysql::MakeDriver(stConfig), 1);
		OI::Mysql::Config stLimitedConfig = stConfig;
		stLimitedConfig.iMaxResultBytes = 1;
		OI::Db::Client stLimited(OI::Mysql::MakeDriver(stLimitedConfig), 1);
		try
		{
			const auto fnValue = [](const std::optional<OI::Db::Blob>& stValue, const std::string& sExpected)
			{
				return stValue && *stValue == OI::Db::Blob(sExpected.begin(), sExpected.end());
			};
			OI::Db::Statement stRepeatedId{"SELECT CONNECTION_ID()", {}};
			OI::Db::Result stRepeatedResult;
			iStatus = co_await stMysql.Call(stRepeatedId, stRepeatedResult, stError);
			if (iStatus != OI_OK || stRepeatedResult.vRows.size() != 1 || stRepeatedResult.vRows[0].size() != 1 ||
			    !stRepeatedResult.vRows[0][0] || stRepeatedResult.vRows[0][0]->empty())
			{
				LOG_TRACE("mysql repeated connection id failed status=%d", iStatus);
				++iBad;
			}
			const auto vExpectedId = stRepeatedResult.vRows;
			OI::Db::Statement stParameters;
			stParameters.sSql = "SELECT ?, ?, ?, ?, ?, ?, ?, ?";
			stParameters.vParameters = {std::string("a\0b", 3), nullptr, std::string(), OI::Db::Blob(),
			    std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max(),
			    uint64_t{0}, std::numeric_limits<uint64_t>::max()};
			for (int32_t iRound = 0; iRound < 8; ++iRound)
			{
				iStatus = co_await stMysql.Call(stParameters, stResult, stError);
				if (iStatus != OI_OK || stResult.vRows.size() != 1 || stResult.vRows[0].size() != 8 ||
				    !fnValue(stResult.vRows[0][0], std::string("a\0b", 3)) || stResult.vRows[0][1].has_value() ||
				    !fnValue(stResult.vRows[0][2], "") || !fnValue(stResult.vRows[0][3], "") ||
				    !fnValue(stResult.vRows[0][4], "-9223372036854775808") ||
				    !fnValue(stResult.vRows[0][5], "9223372036854775807") || !fnValue(stResult.vRows[0][6], "0") ||
				    !fnValue(stResult.vRows[0][7], "18446744073709551615"))
				{
					LOG_TRACE("mysql parameters round=%d failed status=%d stage=%s native=%u", iRound, iStatus, stError.sStage.c_str(), stError.iNativeCode);
					++iBad;
				}
				iStatus = co_await stMysql.Call(stRepeatedId, stRepeatedResult, stError);
				if (iStatus != OI_OK || stRepeatedResult.vRows != vExpectedId)
				{
					LOG_TRACE("mysql repeated connection reuse round=%d failed status=%d", iRound, iStatus);
					++iBad;
				}
			}
			OI::Db::Statement stConnectionId{"SELECT CONNECTION_ID()", {}};
			iStatus = co_await stMysql.Call(stConnectionId, stResult, stError);
			OI::Db::Result stFirstId = stResult;
			if (iStatus != OI_OK || stFirstId.vRows.size() != 1 || stFirstId.vRows[0].size() != 1 ||
			    !stFirstId.vRows[0][0] || stFirstId.vRows[0][0]->empty())
			{
				LOG_TRACE("mysql first connection id failed status=%d", iStatus);
				++iBad;
			}
			iStatus = co_await stMysql.Call(stConnectionId, stResult, stError);
			if (iStatus != OI_OK || stResult.vRows != stFirstId.vRows)
			{
				LOG_TRACE("mysql connection reuse failed status=%d", iStatus);
				++iBad;
			}
			OI::Db::Statement stSetSession{"SET @oi_regression_session = 42", {}};
			iStatus = co_await stMysql.Call(std::move(stSetSession), stResult, stError);
			if (iStatus != OI_OK)
			{
				LOG_TRACE("mysql session variable set failed status=%d stage=%s native=%u", iStatus, stError.sStage.c_str(), stError.iNativeCode);
				++iBad;
			}
			OI::Db::Statement stSession{"SELECT @oi_regression_session", {}};
			iStatus = co_await stMysql.Call(std::move(stSession), stResult, stError);
			if (iStatus != OI_OK || stResult.vRows.size() != 1 || stResult.vRows[0].size() != 1 ||
			    stResult.vRows[0][0].has_value())
			{
				LOG_TRACE("mysql session reset failed status=%d stage=%s native=%u", iStatus, stError.sStage.c_str(), stError.iNativeCode);
				++iBad;
			}
			OI::Db::Statement stDatabase{"SELECT DATABASE()", {}};
			iStatus = co_await stMysql.Call(std::move(stDatabase), stResult, stError);
			if (iStatus != OI_OK || stResult.vRows.size() != 1 || stResult.vRows[0].size() != 1 ||
			    !fnValue(stResult.vRows[0][0], stConfig.sDatabase))
			{
				LOG_TRACE("mysql schema reset failed status=%d stage=%s native=%u", iStatus, stError.sStage.c_str(), stError.iNativeCode);
				++iBad;
			}
			OI::Db::Statement stCreate{"CREATE TABLE " + sQuotedTable + " (id BIGINT PRIMARY KEY) ENGINE=InnoDB", {}};
			iStatus = co_await stMysql.Call(std::move(stCreate), stResult, stError);
			if (iStatus != OI_OK)
			{
				LOG_TRACE("mysql create test table failed status=%d stage=%s native=%u", iStatus, stError.sStage.c_str(), stError.iNativeCode);
				++iBad;
			}
			else
			{
				bTableCreated = true;
				std::vector<OI::Db::Statement> vStatements;
				vStatements.push_back({"INSERT INTO " + sQuotedTable + " (id) VALUES (1)", {}});
				vStatements.push_back({"INSERT INTO " + sQuotedTable + " (missing_column) VALUES (2)", {}});
				std::vector<OI::Db::Result> vResults;
				iStatus = co_await stMysql.Transaction(std::move(vStatements), vResults, stError);
				if (iStatus == OI_OK)
				{
					LOG_TRACE("mysql transaction bad SQL unexpectedly succeeded");
					++iBad;
				}
				OI::Db::Statement stCount{"SELECT COUNT(*) FROM " + sQuotedTable, {}};
				iStatus = co_await stMysql.Call(std::move(stCount), stResult, stError);
				if (iStatus != OI_OK || stResult.vRows.size() != 1 || stResult.vRows[0].size() != 1 ||
				    !fnValue(stResult.vRows[0][0], "0"))
				{
					LOG_TRACE("mysql transaction rollback or recovery failed status=%d stage=%s native=%u", iStatus, stError.sStage.c_str(), stError.iNativeCode);
					++iBad;
				}
			}
			OI::Db::Statement stBad{"SELECT missing_column", {}};
			iStatus = co_await stMysql.Call(std::move(stBad), stResult, stError);
			if (iStatus == OI_OK)
			{
				LOG_TRACE("mysql bad SQL unexpectedly succeeded");
				++iBad;
			}
			OI::Db::Statement stRecover{"SELECT 1", {}};
			iStatus = co_await stMysql.Call(std::move(stRecover), stResult, stError);
			if (iStatus != OI_OK || stResult.vRows.size() != 1 || stResult.vRows[0].size() != 1 ||
			    !fnValue(stResult.vRows[0][0], "1"))
			{
				LOG_TRACE("mysql bad SQL recovery failed status=%d stage=%s native=%u", iStatus, stError.sStage.c_str(), stError.iNativeCode);
				++iBad;
			}
			OI::Db::Result stSentinel;
			stSentinel.iAffected = 123;
			stSentinel.vColumns.push_back("sentinel");
			OI::Db::Statement stLimitedQuery{"SELECT 'too large'", {}};
			iStatus = co_await stLimited.Call(std::move(stLimitedQuery), stSentinel, stError);
			if (iStatus == OI_OK || stError.sStage != "result_limit" || stSentinel.iAffected != 123 ||
			    stSentinel.vColumns != std::vector<std::string>{"sentinel"} || !stSentinel.vRows.empty())
			{
				LOG_TRACE("mysql result limit failed status=%d stage=%s", iStatus, stError.sStage.c_str());
				++iBad;
			}
		}
		catch (const std::exception&)
		{
			LOG_TRACE("mysql regression raised exception");
			++iBad;
		}
		try
		{
			iStatus = co_await stLimited.Close();
			if (iStatus != OI_OK)
			{
				LOG_TRACE("mysql limited client close failed status=%d", iStatus);
				++iBad;
			}
		}
		catch (const std::exception&)
		{
			LOG_TRACE("mysql limited client close raised exception");
			++iBad;
		}
		if (bTableCreated)
		{
			OI::Db::Statement stDrop{"DROP TABLE " + sQuotedTable, {}};
			try
			{
				iStatus = co_await stMysql.Call(std::move(stDrop), stResult, stError);
				if (iStatus != OI_OK)
				{
					LOG_TRACE("mysql drop test table failed status=%d stage=%s native=%u table=%s", iStatus, stError.sStage.c_str(), stError.iNativeCode, sTable.c_str());
					++iBad;
				}
			}
			catch (const std::exception&)
			{
				LOG_TRACE("mysql drop test table raised exception table=%s", sTable.c_str());
				++iBad;
			}
		}
		try
		{
			iStatus = co_await stMysql.Close();
			if (iStatus != OI_OK)
			{
				LOG_TRACE("mysql client close failed status=%d", iStatus);
				++iBad;
			}
		}
		catch (const std::exception&)
		{
			LOG_TRACE("mysql client close raised exception");
			++iBad;
		}
	}
	else
	{
		LOG_TRACE("mysql environment blocked: missing connection environment variables");
		++iBad;
	}
#else
	LOG_TRACE("mysql environment blocked: driver disabled at build time");
	++iBad;
#endif
	LOG_TRACE("mysql done bad=%d", iBad);
	TestBoard::GetInstance().Done(iBad ? 1 : 0);
}
