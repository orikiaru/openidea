#include <string.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <memory>
#include <thread>
#include "log_file_instance.h"
#include "oi_thread.h"
#include "oi_log.h"
#include "oi_net_os.h"
#include "log_manager.h"
#include "oi_file.h"

#ifdef _SINGLE_THREAD_
#error LOG_SERVER MUST BE MULTI THREAD
#endif

LogFileInstance::LogFileInstance(LogManager* pManager)
    : m_pManager(pManager), m_bQuit(false)
{
}

LogFileInstance::~LogFileInstance()
{
}

static FILE* OpenLogFile(const std::string& sPath)
{
	const size_t iSeparator = sPath.find_last_of("/");
	if (iSeparator == std::string::npos)
	{
		errno = EINVAL;
		fprintf(stderr, "logd invalid path: %s\n", sPath.c_str());
		return nullptr;
	}
	const int32_t iStatus = ::OI::File::MakeDirs(sPath.substr(0, iSeparator));
	if (iStatus != OI_OK)
	{
		errno = iStatus > 0 ? iStatus : EIO;
		fprintf(stderr, "logd mkdir failed: %s code=%d\n", sPath.c_str(), iStatus);
		return nullptr;
	}
#ifdef _WIN32
	FILE* pFile = nullptr;
	const int iError = fopen_s(&pFile, sPath.c_str(), "ab+");
	if (iError != 0 || !pFile)
	{
		fprintf(stderr, "logd open %s failed: %s\n", sPath.c_str(), NetOs_StrError(iError ? iError : EIO));
		return nullptr;
	}
#else
	FILE* pFile = fopen(sPath.c_str(), "ab+");
	if (!pFile)
	{
		fprintf(stderr, "logd open %s failed: %s\n", sPath.c_str(), NetOs_StrError(errno));
		return nullptr;
	}
#endif
	setvbuf(pFile, nullptr, _IOFBF, 1024 * 1024);
	return pFile;
}

int32_t LogFileInstance::StartWriteThread(const std::string& sPath)
{
	m_sInitialPath = sPath;
	const int32_t iStatus = ::OI::Thread::ThreadPool::CreateSpecThread(m_stThread, LogFileInstance::WriteLogThreadHandle, this);
	if (iStatus != OI_OK)
	{
		return iStatus;
	}
	::OI::Thread::MutexGuard stLock(m_stCacheLocker);
	while (!m_bStarted)
	{
		m_stCacheCondition.Wait(m_stCacheLocker);
	}
	return m_iStartStatus;
}

void LogFileInstance::StopWriteThread()
{
	{
		::OI::Thread::MutexGuard stLock(m_stCacheLocker);
		m_bQuit = true;
		Active();
	}
	if (m_stThread.joinable())
	{
		m_stThread.join();
	}
}

void LogFileInstance::Rotate(const std::string& sPath)
{
	::OI::Thread::MutexGuard stLock(m_stCacheLocker);
	m_vLogs.emplace_back();
	m_vLogs.back().sRotationPath = sPath;
	Active();
}

void LogFileInstance::Active()
{
	m_stCacheCondition.Signal();
}

void LogFileInstance::Passive()
{
	m_stCacheCondition.Wait(m_stCacheLocker);
}

void LogFileInstance::AddLog(::OI::Base::Bytes& stLog)
{
	bool bNotify = false;

	{
		::OI::Thread::MutexGuard stLock(m_stCacheLocker);
		if (m_vLogs.empty())
		{
			bNotify = true;
		}
		m_vLogs.emplace_back();
		m_vLogs.back().stData = std::move(stLog);
		if (bNotify)
		{
			Active();
		}
	}
}

void* LogFileInstance::WriteLogThreadHandle(void* pArgument)
{
	LogFileInstance* pInst = (LogFileInstance*)pArgument;
	pInst->WriteLog();
	return NULL;
}

static void DiskFailure(const char* pOperation)
{
	const int iError = errno;
	// 不经日志网络告警，避免递归记录。停止进程并明确暴露失败，不能清空后假装已落盘。
	fprintf(stderr, "logd %s failed: %s\n", pOperation, NetOs_StrError(iError));
	fflush(stderr);
	std::abort();
}

static bool WriteAll(FILE* pFile, const char* pData, size_t iSize)
{
#ifdef OI_ENABLE_TEST_HOOKS
	// 只在测试构建生效，只阻塞日志写线程，不阻塞 poll 或业务协程。
#ifdef _WIN32
	char* pFaultBuffer = nullptr;
	size_t iRequiredSize = 0;
	_dupenv_s(&pFaultBuffer, &iRequiredSize, "OI_TEST_LOG_FAULT");
	std::unique_ptr<char, decltype(&std::free)> pFaultOwner(pFaultBuffer, &std::free);
	const char* pFault = pFaultOwner.get();
#else
	const char* pFault = std::getenv("OI_TEST_LOG_FAULT");
#endif
	if (pFault && std::strcmp(pFault, "full") == 0)
	{
#ifdef _WIN32
		_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
		errno = ENOSPC;
		DiskFailure("injected fwrite ENOSPC");
	}
	if (pFault && std::strcmp(pFault, "slow") == 0)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
#endif
	while (iSize > 0)
	{
		size_t iWrote = fwrite(pData, 1, iSize, pFile);
		if (iWrote == 0)
		{
			DiskFailure("fwrite");
		}
		pData += iWrote;
		iSize -= iWrote;
	}
	return true;
}

void LogFileInstance::WriteLog()
{
	FILE* pFile = OpenLogFile(m_sInitialPath);
	{
		::OI::Thread::MutexGuard stLock(m_stCacheLocker);
		m_iStartStatus = pFile ? OI_OK : OI_ERR_FAIL;
		m_bStarted = true;
		m_stCacheCondition.Signal();
	}
	if (!pFile)
	{
		return;
	}
	for (;;)
	{
		LogCacheList vWriting;
		bool bStop = false;
		{
			::OI::Thread::MutexGuard stLock(m_stCacheLocker);
			while (m_vLogs.empty() && !m_bQuit)
			{
				Passive();
			}
			bStop = m_vLogs.empty() && m_bQuit;
			vWriting.swap(m_vLogs);
		}
		if (bStop)
		{
			if (fclose(pFile) != 0)
			{
				DiskFailure("fclose");
			}
			return;
		}
		size_t iWrittenBytes = 0;
		for (const auto& stEntry : vWriting)
		{
			if (!stEntry.sRotationPath.empty())
			{
				if (fflush(pFile) != 0 || fclose(pFile) != 0)
				{
					DiskFailure("rotate close");
				}
				pFile = OpenLogFile(stEntry.sRotationPath);
				if (!pFile)
				{
					DiskFailure("rotate open");
				}
				continue;
			}
			WriteAll(pFile, stEntry.stData.Data(), stEntry.stData.Size());
			iWrittenBytes += stEntry.stData.Size();
		}
		if (fflush(pFile) != 0)
		{
			DiskFailure("fflush");
		}
		vWriting.clear();
		m_pManager->CompleteWrite(iWrittenBytes);
	}
}
