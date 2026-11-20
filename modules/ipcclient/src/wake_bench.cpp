#include "db_mgr.h"
#include "oi_db.h"
#include "oi_mysql.h"
#include "oi_errno.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace
{
using BenchClock = std::chrono::steady_clock;

int64_t BenchNow()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(BenchClock::now().time_since_epoch()).count();
}

void BenchReport(const char* pName, std::vector<int64_t> vSamples)
{
	std::sort(vSamples.begin(), vSamples.end());
	int64_t iSum = 0;
	int32_t iSlow = 0;
	for (const auto& iValue : vSamples)
	{
		iSum += iValue;
		if (iValue >= 1000000)
		{
			++iSlow;
		}
	}
	const auto fnPercentile = [](const std::vector<int64_t>& vValues, size_t iPercent)
	{
		return vValues[(vValues.size() * iPercent + 99) / 100 - 1] / 1000.0;
	};
	std::printf("wake benchmark %s n=%zu p50_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f ge1000us=%d sum_us=%.3f\n",
	    pName, vSamples.size(), fnPercentile(vSamples, 50), fnPercentile(vSamples, 95), fnPercentile(vSamples, 99),
	    vSamples.back() / 1000.0, iSlow, iSum / 1000.0);
	std::fflush(stdout);
}

#ifdef OI_ENABLE_MYSQL
class BenchConnection : public OI::Db::Connection
{
	std::unique_ptr<OI::Db::Connection> m_pConnection;
	std::shared_ptr<std::atomic<int64_t>> m_pFinished;
	std::shared_ptr<std::atomic<int64_t>> m_pReset;

  public:
	BenchConnection(std::unique_ptr<OI::Db::Connection> pConnection, std::shared_ptr<std::atomic<int64_t>> pFinished, std::shared_ptr<std::atomic<int64_t>> pReset)
	    : m_pConnection(std::move(pConnection)), m_pFinished(std::move(pFinished)), m_pReset(std::move(pReset))
	{
	}
	int32_t Execute(const OI::Db::Statement& stStatement, OI::Db::Result& stResult, OI::Db::Error& stError) override
	{
		const int32_t iStatus = m_pConnection->Execute(stStatement, stResult, stError);
		m_pFinished->store(BenchNow(), std::memory_order_release);
		return iStatus;
	}
	bool Healthy() const override
	{
		return m_pConnection->Healthy();
	}
	bool Reset(OI::Db::Error& stError) override
	{
		const bool bReset = m_pConnection->Reset(stError);
		m_pReset->store(BenchNow(), std::memory_order_release);
		return bReset;
	}
};

class BenchDriver : public OI::Db::Driver
{
	std::shared_ptr<OI::Db::Driver> m_pDriver;
	std::shared_ptr<std::atomic<int64_t>> m_pFinished;
	std::shared_ptr<std::atomic<int64_t>> m_pReset;

  public:
	BenchDriver(std::shared_ptr<OI::Db::Driver> pDriver, std::shared_ptr<std::atomic<int64_t>> pFinished, std::shared_ptr<std::atomic<int64_t>> pReset)
	    : m_pDriver(std::move(pDriver)), m_pFinished(std::move(pFinished)), m_pReset(std::move(pReset))
	{
	}
	std::unique_ptr<OI::Db::Connection> Connect(OI::Db::Error& stError) override
	{
		auto pConnection = m_pDriver->Connect(stError);
		if (!pConnection)
		{
			return nullptr;
		}
		return std::make_unique<BenchConnection>(std::move(pConnection), m_pFinished, m_pReset);
	}
};
#endif

}

OI::Coro::TaskResult<int32_t> DbMgr::RunWakeBenchmark(bool bBlockingOnly)
{
	constexpr int32_t iWarmup = 100;
	constexpr int32_t iSamples = 1000;
	if (bBlockingOnly)
	{
		constexpr int32_t iDefaultSamples = 10000;
		for (int32_t iRound = 0; iRound < iWarmup; ++iRound)
		{
			const int32_t iStatus = co_await OI::Coro::RunBlocking([]() -> int32_t
			{
				return OI_OK;
			});
			if (iStatus != OI_OK)
			{
				std::printf("wake benchmark default empty warmup failed round=%d status=%d\n", iRound, iStatus);
				co_return 1;
			}
		}
		const int64_t iStart = BenchNow();
		for (int32_t iRound = 0; iRound < iDefaultSamples; ++iRound)
		{
			const int32_t iStatus = co_await OI::Coro::RunBlocking([]() -> int32_t
			{
				return OI_OK;
			});
			if (iStatus != OI_OK)
			{
				std::printf("wake benchmark default empty failed round=%d status=%d\n", iRound, iStatus);
				co_return 1;
			}
		}
		const int64_t iEnd = BenchNow();
		if (iEnd < iStart)
		{
			co_return 1;
		}
		const double dTotalMicroseconds = (iEnd - iStart) / 1000.0;
		std::printf("wake benchmark default_empty_serial n=%d total_us=%.3f mean_us=%.3f\n", iDefaultSamples,
		    dTotalMicroseconds, dTotalMicroseconds / iDefaultSamples);
		auto pDefaultFinished = std::make_shared<std::atomic<int64_t>>(0);
		std::vector<int64_t> vDefaultWake;
		vDefaultWake.reserve(iDefaultSamples);
		int64_t iPairedStart = 0;
		int64_t iPairedEnd = 0;
		for (int32_t iRound = 0; iRound < iWarmup + iDefaultSamples; ++iRound)
		{
			pDefaultFinished->store(0, std::memory_order_release);
			if (iRound == iWarmup)
			{
				iPairedStart = BenchNow();
			}
			const int32_t iStatus = co_await OI::Coro::RunBlocking([pDefaultFinished]() -> int32_t
			{
				pDefaultFinished->store(BenchNow(), std::memory_order_release);
				return OI_OK;
			});
			const int64_t iResumed = BenchNow();
			const int64_t iFinished = pDefaultFinished->load(std::memory_order_acquire);
			if (iStatus != OI_OK || iFinished <= 0 || iResumed < iFinished)
			{
				std::printf("wake benchmark default paired failed round=%d status=%d\n", iRound, iStatus);
				co_return 1;
			}
			if (iRound >= iWarmup)
			{
				vDefaultWake.push_back(iResumed - iFinished);
				iPairedEnd = iResumed;
			}
		}
		if (iPairedEnd < iPairedStart)
		{
			co_return 1;
		}
		std::printf("wake benchmark default_paired_serial n=%d total_us=%.3f mean_us=%.3f\n", iDefaultSamples,
		    (iPairedEnd - iPairedStart) / 1000.0, (iPairedEnd - iPairedStart) / 1000.0 / iDefaultSamples);
		BenchReport("default_paired_finish_to_resume", std::move(vDefaultWake));
	}
	auto pFinished = std::make_shared<std::atomic<int64_t>>(0);
	std::vector<int64_t> vBlocking;
	for (int32_t iRound = 0; iRound < iWarmup + iSamples; ++iRound)
	{
		pFinished->store(0, std::memory_order_release);
		const int64_t iStart = BenchNow();
		const int32_t iStatus = co_await OI::Coro::RunBlocking(OI::Thread::BlockingType::Mysql, [pFinished]() -> int32_t
		{
			pFinished->store(BenchNow(), std::memory_order_release);
			return OI_OK;
		});
		const int64_t iResumed = BenchNow();
		const int64_t iFinished = pFinished->load(std::memory_order_acquire);
		if (iStatus != OI_OK || iFinished < iStart || iResumed < iFinished)
		{
			std::printf("wake benchmark blocking failed status=%d round=%d\n", iStatus, iRound);
			co_return 1;
		}
		if (iRound >= iWarmup)
		{
			vBlocking.push_back(iResumed - iFinished);
		}
	}
	BenchReport("blocking_finish_to_resume", std::move(vBlocking));
	if (bBlockingOnly)
	{
		co_return 0;
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
		struct DirectWork
		{
			std::shared_ptr<OI::Db::Driver> pDriver;
			std::unique_ptr<OI::Db::Connection> pConnection;
			OI::Db::Result stResult;
			OI::Db::Error stError;
			std::atomic<int64_t> iStarted{0};
			std::atomic<int64_t> iFinished{0};
		};
		auto pDirect = std::make_shared<DirectWork>();
		pDirect->pDriver = OI::Mysql::MakeDriver(stConfig);
		std::vector<int64_t> vDirectWake;
		std::vector<int64_t> vDirectWork;
		std::vector<int64_t> vDirectQueue;
		for (int32_t iRound = 0; iRound < iWarmup + iSamples; ++iRound)
		{
			pDirect->iStarted.store(0, std::memory_order_release);
			pDirect->iFinished.store(0, std::memory_order_release);
			const int64_t iStart = BenchNow();
			const int32_t iStatus = co_await OI::Coro::RunBlocking(OI::Thread::BlockingType::Mysql, [pDirect]() -> int32_t
			{
				pDirect->iStarted.store(BenchNow(), std::memory_order_release);
				if (!pDirect->pConnection)
				{
					pDirect->pConnection = pDirect->pDriver->Connect(pDirect->stError);
				}
				if (!pDirect->pConnection)
				{
					return 1;
				}
				OI::Db::Statement stStatement{"SELECT 1", {}};
				const int32_t iCode = pDirect->pConnection->Execute(stStatement, pDirect->stResult, pDirect->stError);
				if (iCode != OI_OK || !pDirect->pConnection->Healthy() || !pDirect->pConnection->Reset(pDirect->stError))
				{
					return 1;
				}
				pDirect->iFinished.store(BenchNow(), std::memory_order_release);
				return OI_OK;
			});
			const int64_t iResumed = BenchNow();
			const int64_t iStarted = pDirect->iStarted.load(std::memory_order_acquire);
			const int64_t iFinished = pDirect->iFinished.load(std::memory_order_acquire);
			if (iStatus != OI_OK || iStarted < iStart || iFinished < iStarted || iResumed < iFinished || pDirect->stResult.vRows.size() != 1 ||
			    pDirect->stResult.vRows[0].size() != 1 || !pDirect->stResult.vRows[0][0] ||
			    *pDirect->stResult.vRows[0][0] != OI::Db::Blob{'1'})
			{
				std::printf("wake benchmark direct mysql failed status=%d round=%d stage=%s native=%u\n", iStatus, iRound,
				    pDirect->stError.sStage.c_str(), pDirect->stError.iNativeCode);
				const int32_t iCleanup = co_await OI::Coro::RunBlocking(OI::Thread::BlockingType::Mysql, [pDirect]() -> int32_t
				{
					pDirect->pConnection.reset();
					return OI_OK;
				});
				(void)iCleanup;
				co_return 1;
			}
			if (iRound >= iWarmup)
			{
				vDirectWake.push_back(iResumed - iFinished);
				vDirectQueue.push_back(iStarted - iStart);
				vDirectWork.push_back(iFinished - iStarted);
			}
		}
		const int32_t iCleanup = co_await OI::Coro::RunBlocking(OI::Thread::BlockingType::Mysql, [pDirect]() -> int32_t
		{
			pDirect->pConnection.reset();
			return OI_OK;
		});
		if (iCleanup != OI_OK)
		{
			co_return 1;
		}
		BenchReport("mysql_direct_submit_to_worker_start", std::move(vDirectQueue));
		BenchReport("mysql_direct_worker_start_to_finish", std::move(vDirectWork));
		BenchReport("mysql_direct_finish_to_resume", std::move(vDirectWake));
		auto pReset = std::make_shared<std::atomic<int64_t>>(0);
		OI::Db::Client stMysql(std::make_shared<BenchDriver>(OI::Mysql::MakeDriver(stConfig), pFinished, pReset), 1);
		std::vector<int64_t> vAfterExecute;
		std::vector<int64_t> vTotal;
		std::vector<int64_t> vAfterReset;
		int32_t iBad = 0;
		for (int32_t iRound = 0; iRound < iWarmup + iSamples; ++iRound)
		{
			OI::Db::Result stResult;
			OI::Db::Error stError;
			pFinished->store(0, std::memory_order_release);
			pReset->store(0, std::memory_order_release);
			OI::Db::Statement stStatement{"SELECT 1", {}};
			const int64_t iStart = BenchNow();
			const int32_t iStatus = co_await stMysql.Call(std::move(stStatement), stResult, stError);
			const int64_t iResumed = BenchNow();
			const int64_t iFinished = pFinished->load(std::memory_order_acquire);
			const int64_t iReset = pReset->load(std::memory_order_acquire);
			if (iStatus != OI_OK || iFinished < iStart || iReset < iFinished || iResumed < iReset || stError.stCleanup.bFailed || stResult.vRows.size() != 1 || stResult.vRows[0].size() != 1 ||
			    !stResult.vRows[0][0] || *stResult.vRows[0][0] != OI::Db::Blob{'1'})
			{
				std::printf("wake benchmark mysql failed status=%d round=%d stage=%s native=%u\n", iStatus, iRound, stError.sStage.c_str(), stError.iNativeCode);
				iBad = 1;
				break;
			}
			if (iRound >= iWarmup)
			{
				vAfterExecute.push_back(iResumed - iFinished);
				vTotal.push_back(iResumed - iStart);
				vAfterReset.push_back(iResumed - iReset);
			}
		}
		const int32_t iClose = co_await stMysql.Close();
		if (iClose != OI_OK || iBad != 0)
		{
			std::printf("wake benchmark mysql close=%d bad=%d\n", iClose, iBad);
			co_return 1;
		}
		BenchReport("mysql_execute_finish_to_resume_including_cleanup", std::move(vAfterExecute));
		BenchReport("mysql_reset_finish_to_resume_including_cleanup", std::move(vAfterReset));
		BenchReport("mysql_call_total", std::move(vTotal));
	}
	else
#endif
	{
		std::printf("wake benchmark mysql unavailable\n");
		co_return 1;
	}
	co_return 0;
}
