#include <string.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include "log_file_instance.h"
#include "oi_thread.h"
#include "oi_log.h"
#include "log_manager.h"

#ifdef _SINGLE_THREAD_
#error LOG_SERVER MUST BE MULTI THREAD
#endif

LogFileInstance::LogFileInstance(LogManager* pManager, LOG_LEVEL iLevel)
    : m_pManager(pManager), m_iLevel(iLevel), m_bWorking(false), m_bQuit(false)
{
	memset(m_vFiles, 0, sizeof(m_vFiles));
}

LogFileInstance::~LogFileInstance()
{
	FILE* pFile = GetActiveFile(); //顺带把跨天换下来的旧文件关掉
	if (pFile)
	{
		fflush(pFile);
		fclose(pFile);
	}
}

int32_t LogFileInstance::CreateNewFile(const char* pPath)
{
#ifdef _WIN32
	FILE* pFile = NULL;
	const int iError = fopen_s(&pFile, pPath, "ab+");
	if (iError != 0 || !pFile)
	{
		fprintf(stderr, "logd open %s failed: %s\n", pPath, strerror(iError ? iError : EIO));
		return iError ? iError : EIO;
	}
#else
	FILE* pFile = fopen(pPath, "ab+");
	if (!pFile)
	{
		const int iError = errno ? errno : EIO;
		fprintf(stderr, "logd open %s failed: %s\n", pPath, strerror(iError));
		return iError;
	}
#endif
	// 交给写线程按块刷。这里只把 stdio 缓冲放大，避免每条日志一次系统调用。
	setvbuf(pFile, NULL, _IOFBF, 1024 * 1024);
	OnCreateNewFile(pFile);
	return OI_OK;
}

void LogFileInstance::OnCreateNewFile(FILE* pNewFile)
{
	{
		::OI::Thread::MutexGuard stIndexLockState(m_stFileLocker);
		assert(m_vFiles[1] == NULL);
		m_vFiles[1] = pNewFile;
	}
	{
		::OI::Thread::MutexGuard stLock(m_stCacheLocker);
		m_bRotationPending = true;
		Active();
	}
}

int32_t LogFileInstance::StartWriteThread()
{
	return ::OI::Thread::ThreadPool::CreateSpecThread(m_stThread, LogFileInstance::WriteLogThreadHandle, this);
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

FILE* LogFileInstance::GetActiveFile()
{
	::OI::Thread::MutexGuard stIndexLockState(m_stFileLocker);
	if (m_vFiles[1])
	{
		if (m_vFiles[0])
		{
			fclose(m_vFiles[0]);
		}
		m_vFiles[0] = m_vFiles[1];
		m_vFiles[1] = NULL;
	}
	return m_vFiles[0];
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
		m_vLogs.push_back(std::move(stLog));
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
	fprintf(stderr, "logd %s failed: %s\n", pOperation, strerror(iError));
	fflush(stderr);
	std::abort();
}

static bool WriteAll(FILE* pFile, const char* pData, size_t iSize)
{
#ifdef OI_ENABLE_TEST_HOOKS
	// 只在测试构建生效，只阻塞日志写线程，不阻塞 poll 或业务协程。
	const char* pFault = std::getenv("OI_TEST_LOG_FAULT");
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
	assert(std::this_thread::get_id() == m_stThread.get_id());
	m_bWorking = true;
	for (;;)
	{
		LogCacheList vWriting;
		bool bStop = false;
		{
			::OI::Thread::MutexGuard stLock(m_stCacheLocker);
			while (m_vLogs.empty() && !m_bQuit && !m_bRotationPending)
			{
				Passive();
			}
			bStop = m_vLogs.empty() && m_bQuit;
			m_bRotationPending = false;
			vWriting.swap(m_vLogs);
		}
		FILE* pFile = GetActiveFile();
		if (!pFile)
		{
			errno = EBADF;
			DiskFailure("missing active file");
		}
		if (bStop)
		{
			return;
		}
		if (vWriting.empty())
		{
			continue;
		}
		size_t iWrittenBytes = 0;
		for (const auto& stLog : vWriting)
		{
			WriteAll(pFile, stLog.Data(), stLog.Size());
			iWrittenBytes += stLog.Size();
		}
		if (fflush(pFile) != 0)
		{
			DiskFailure("fflush");
		}
		// 移到写线程不释放额度；成功 fflush 且释放批次内存后才允许重新入队。
		vWriting.clear();
		m_pManager->CompleteWrite(iWrittenBytes);
	}
}
