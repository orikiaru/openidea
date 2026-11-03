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

LogFileInstance::LogFileInstance(LogManager* pManagerParam, LOG_LEVEL iLevelParam):m_pManager(pManagerParam), m_iLevel(iLevelParam), m_bWorking(false), m_bQuit(false)
{
	memset(m_vFiles, 0, sizeof(m_vFiles));
}

LogFileInstance::~LogFileInstance()
{
	FILE* pFile = GetActiveFile();	//顺带把跨天换下来的旧文件关掉
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
	Active();
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

std::string LogFileInstance::GetLogTiTle()
{
	return m_pManager->GetLogTitle();
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

	// 攒到一块再写。压力上来时大约 8MB 刷一次，队列空了把尾巴刷掉。
	const size_t iItemIndexChunk = 8 * 1024 * 1024;
	::OI::Base::Bytes stTmp;
	stTmp.Reserve(iItemIndexChunk);
	int64_t iUnflushed = 0;
	for(;;)
	{
		FILE* pFile = GetActiveFile();
		if (!pFile)
		{
			errno = EBADF;
			DiskFailure("missing active file");
		}

		LogCacheList vTmpLogs;
		m_stCacheLocker.Lock();
		vTmpLogs.swap(m_vLogs);
		if (vTmpLogs.empty() && stTmp.Size() == 0)
		{
			if (m_bQuit)
			{
				m_stCacheLocker.UnLock();
				if (fflush(pFile) != 0) DiskFailure("fflush");
				return;
			}
			Passive();
			m_stCacheLocker.UnLock();
			continue;
		}
		m_stCacheLocker.UnLock();

		if (!vTmpLogs.empty())
		{
			const std::string sTimeTitle = GetLogTiTle();
			size_t iTitleLength = sTimeTitle.size();
			for (const auto& stLog : vTmpLogs)
			{
				if (iTitleLength > 0)
				{
					stTmp.Append(sTimeTitle.data(), iTitleLength);
				}
				stTmp.Append(stLog);
				if (stTmp.Size() >= iItemIndexChunk)
				{
					if (pFile && WriteAll(pFile, stTmp.Begin(), stTmp.Size()))
					{
						iUnflushed += (int64_t)stTmp.Size();
					}
					stTmp.Reserve(0);
					if (pFile && iUnflushed >= (int64_t)iItemIndexChunk)
					{
						if (fflush(pFile) != 0) DiskFailure("fflush");
						iUnflushed = 0;
					}
				}
			}
		}

		if (vTmpLogs.empty() && stTmp.Size() > 0)
		{
			if (pFile)
			{
				WriteAll(pFile, stTmp.Begin(), stTmp.Size());
			}
			stTmp.Reserve(0);
			if (pFile)
			{
				if (fflush(pFile) != 0) DiskFailure("fflush");
			}
			iUnflushed = 0;
		}
	}
}

