#include "test_log.h"
#include "oi_config.h"
#include "oi_log.h"
#include "oi_log_client.h"
#include "test_board.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

TestLog::TestLog()
    : m_bEnabled(false), m_iTaskCount(8), m_iLineBytes(64 * 1024), m_iTargetBytes(1024LL * 1024 * 1024),
      m_iDrainSeconds(15), m_iSent(0)
{
}

void TestLog::Load()
{
	TryGetConfig("Test.log", m_bEnabled);
	TryGetConfig("TestLog.tasks", m_iTaskCount);
	TryGetConfig("TestLog.line", m_iLineBytes);
	TryGetConfig("TestLog.bytes", m_iTargetBytes);
	TryGetConfig("TestLog.drain_sec", m_iDrainSeconds);
}

void TestLog::Start()
{
	if (!m_bEnabled || m_iTaskCount <= 0 || m_iLineBytes <= 0 || m_iTargetBytes <= 0)
	{
		return;
	}
	m_stStart = std::chrono::steady_clock::now();
	TestBoard::GetInstance().Arm();
	OI::Coro::StartTask([]()
	{
		return TestLog::GetInstance().Run();
	});
}

OI::Coro::Task TestLog::Run()
{
	const char* pRotationGate = std::getenv("OI_TEST_LOG_ROTATION_GATE");
	if (pRotationGate)
	{
		std::string sGate(pRotationGate);
		int32_t iBad = 0;
		for (int iPhase = 0; iPhase < 4; ++iPhase)
		{
			for (;;)
			{
				fflush(stdout);
				FILE* pGate = fopen((sGate + "." + std::to_string(iPhase)).c_str(), "rb");
				if (pGate)
				{
					fclose(pGate);
					break;
				}
				co_await OI::Coro::yield(10);
			}
			for (int iSequence = 0; iSequence < 10; ++iSequence)
			{
				int32_t iCode = ::OI::Log::Log(LOG_INFO, "log-rotation phase=%d seq=%d", iPhase, iSequence);
				if (iCode != OI_OK)
				{
					++iBad;
				}
			}
			printf("log-rotation phase=%d bad=%d\n", iPhase, iBad);
			fflush(stdout);
		}
		for (;;)
		{
			FILE* pGate = fopen((sGate + ".done").c_str(), "rb");
			if (pGate)
			{
				fclose(pGate);
				break;
			}
			co_await OI::Coro::yield(10);
		}
		TestBoard::GetInstance().Done(iBad);
		co_return;
	}
	const char* pContractGate = std::getenv("OI_TEST_LOG_CONTRACT_GATE");
	if (pContractGate)
	{
		std::string sGate(pContractGate);
		for (;;)
		{
			fflush(stdout);
			FILE* pGate = fopen((sGate + ".disconnected").c_str(), "rb");
			if (pGate)
			{
				fclose(pGate);
				break;
			}
			co_await OI::Coro::yield(10);
		}
		int iBusySequence = -1;
		for (int iSequence = 0; iSequence < 10000; ++iSequence)
		{
			int32_t iReturnCode = ::OI::Log::Log(LOG_INFO, "log-contract seq=%d", iSequence);
			printf("log-contract result seq=%d code=%d\n", iSequence, iReturnCode);
			if (iReturnCode != OI_OK)
			{
				iBusySequence = iSequence;
				break;
			}
		}
		printf("log-contract pending complete busy=%d\n", iBusySequence);
		fflush(stdout);
		for (;;)
		{
			fflush(stdout);
			FILE* pGate = fopen((sGate + ".connected").c_str(), "rb");
			if (pGate)
			{
				fclose(pGate);
				break;
			}
			co_await OI::Coro::yield(10);
		}
		int32_t iReturnCode = OI_ERR_BUSY;
		for (int iAttempt = 0; iAttempt < 1000 && iReturnCode == OI_ERR_BUSY; ++iAttempt)
		{
			iReturnCode = ::OI::Log::Log(LOG_INFO, "log-contract final");
			printf("log-contract final code=%d\n", iReturnCode);
			if (iReturnCode == OI_ERR_BUSY)
			{
				co_await OI::Coro::yield(10);
			}
		}
		fflush(stdout);
		for (;;)
		{
			FILE* pGate = fopen((sGate + ".drained").c_str(), "rb");
			if (pGate)
			{
				fclose(pGate);
				break;
			}
			co_await OI::Coro::yield(10);
		}
		TestBoard::GetInstance().Done(iBusySequence < 0 || iReturnCode != OI_OK ? 1 : 0);
		co_return;
	}
	std::vector<OI::Coro::Task> vPumps;
	for (int iIndexLocal = 0; iIndexLocal < m_iTaskCount; ++iIndexLocal)
	{
		vPumps.push_back(Pump());
	}
	co_await OI::Coro::WhenAll(std::move(vPumps));
	long long iMillisecondsLocal = std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now() - m_stStart)
	                                   .count();
	if (iMillisecondsLocal < 1)
	{
		iMillisecondsLocal = 1;
	}
	long long iSentLocal = m_iSent.load();
	LOG_TRACE("log tasks=%d sent=%lld ms=%lld enqueue_rate=%lldMB/s",
	    m_iTaskCount, iSentLocal, iMillisecondsLocal, iSentLocal / iMillisecondsLocal * 1000 / (1024 * 1024));
	fflush(stdout);
	if (m_iDrainSeconds > 0)
	{
		// 单线程只有这一条线程。sleep 会把 poll 冻住，套接字里的日志发不出去。
		co_await OI::Coro::yield(m_iDrainSeconds * 1000);
	}
	TestBoard::GetInstance().Done(0);
}

OI::Coro::Task TestLog::Pump()
{
	std::vector<char> vPayload((size_t)m_iLineBytes, 'L');
	if (!vPayload.empty())
	{
		vPayload.back() = '\n';
	}
	for (;;)
	{
		int64_t iPrevious = m_iSent.fetch_add(m_iLineBytes);
		if (iPrevious >= m_iTargetBytes)
		{
			// 结束探测也要归还额度，不能把未发送的占位计入成功字节数。
			m_iSent.fetch_sub(m_iLineBytes);
			break;
		}
		if (::OI::Log::LogClient::GetInstance().SendLog(LOG_INFO, vPayload.data(), vPayload.size()) != OI_OK)
		{
			m_iSent.fetch_sub(m_iLineBytes);
		}
		// 同一协程复用 payload；每条后让出，让 poll 排空缓冲并给其他业务运行机会。
		co_await OI::Coro::yield(0);
	}
}
