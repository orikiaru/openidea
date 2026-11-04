#include <string.h>
#include <cerrno>
#include <cstdlib>
#include "log_file_instance.h"
#include "oi_thread.h"
#include "oi_log.h"
#include "log_manager.h"

#ifdef _SINGLE_THREAD_
#error LOG_SERVER MUST BE MULTI THREAD
#endif

LogFileInstance::LogFileInstance(LogManager* pManagerParam, LOG_LEVEL iLevelParam)
    : m_pManager(pManagerParam), m_iLevel(iLevelParam), m_bWorking(false), m_bQuit(false)
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

bool LogFileInstance::CreateNewFile(const char* pPathParam)
{
#ifdef _WIN32
	FILE* pFile = NULL;
	if (fopen_s(&pFile, pPathParam, "a+") != 0 || !pFile)
	{
		return false;
	}
#else
	FILE* pFile = fopen(pPathParam, "a+");
	if (!pFile)
	{
		return false;
	}
#endif
	// 交给写线程按块刷。这里只把 stdio 缓冲放大，避免每条日志一次系统调用。
	setvbuf(pFile, NULL, _IOFBF, 1024 * 1024);
	OnCreateNewFile(pFile);
	return true;
}

void LogFileInstance::OnCreateNewFile(FILE* pNewFileParam)
{
	{
		::OI::Thread::MutexGuard stIndexLockState(m_stFileLocker);
		assert(m_vFiles[1] == NULL);
		m_vFiles[1] = pNewFileParam;
	}
	{
		::OI::Thread::MutexGuard stLock(m_stCacheLocker);
		m_bRotationPending = true;
		Active();
	}
}

bool LogFileInstance::StartWriteThread()
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

void LogFileInstance::AddLog(::OI::Base::Bytes& stLogParam)
{
	bool bNotify = false;

	{
		::OI::Thread::MutexGuard stLock(m_stCacheLocker);
		if (m_vLogs.empty())
		{
			bNotify = true;
		}
		m_vLogs.push_back(std::move(stLogParam));
		if (bNotify)
		{
			Active();
		}
	}
}

void* LogFileInstance::WriteLogThreadHandle(void* pArgumentParam)
{
	LogFileInstance* pInst = (LogFileInstance*)pArgumentParam;
	pInst->WriteLog();
	return NULL;
}

static void DiskFailure(const char* pOperationParam)
{
	const int iError = errno;
	// 不经日志网络告警，避免递归记录。停止进程并明确暴露失败，不能清空后假装已落盘。
	fprintf(stderr, "logd %s failed: %s\n", pOperationParam, strerror(iError));
	fflush(stderr);
	std::abort();
}

static bool WriteAll(FILE* pFileParam, const char* pDataParam, size_t iSizeParam)
{
	while (iSizeParam > 0)
	{
		size_t iWrote = fwrite(pDataParam, 1, iSizeParam, pFileParam);
		if (iWrote == 0)
		{
			DiskFailure("fwrite");
		}
		pDataParam += iWrote;
		iSizeParam -= iWrote;
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
