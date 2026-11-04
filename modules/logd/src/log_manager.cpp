#include "log_manager.h"
#include <stdio.h>
#include <string.h>
#include <cstdlib>
#include <algorithm>
#include "oi_timer_helper.h"
#include "oi_file.h"
#include "log_file_instance.h"
#include "oi_config.h"
#include "oi_log_frame.h"
#include "log_server_session.h"

bool LogManager::Init()
{
	int64_t iLimit = 64 * 1024 * 1024;
	TryGetConfig("Log.max_pending_bytes", iLimit);
	if (iLimit < ::OI::Log::LOG_FRAME_MAX + 8 || static_cast<uint64_t>(iLimit) > SIZE_MAX)
	{
		fprintf(stderr, "logd Log.max_pending_bytes must be at least %d and fit size_t\n", ::OI::Log::LOG_FRAME_MAX + 8);
		return false;
	}
	m_iMaxPendingBytes = static_cast<size_t>(iLimit);
	m_iLastTimestamp = ::OI::Timer::GetTime();
	m_stLastCalendarTime = ::OI::Timer::GetLocalTime();
	char vInitialTitle[LOG_TITLE_BUFF_SIZE];
	const size_t iTitleSize = strftime(vInitialTitle, sizeof(vInitialTitle), "%T ", &m_stLastCalendarTime);
	m_sLogTitle.assign(vInitialTitle, iTitleSize);

	std::string sLogRootPath = "./";
	TryGetConfig("Log.root_path", sLogRootPath);
	m_sLogRootPath = sLogRootPath;

	if (!CheckAndCreateRootPath())
	{
		LOG_TRACE("LogManager::Init error CheckAndCreateRootPath=%s", m_sLogRootPath.c_str());
		return false;
	}

	if (!OpenAllLogFile())
	{
		LOG_TRACE("LogManager::Init error OpenAllLogFile=%s", m_sLogRootPath.c_str());
		return false;
	}

	if (!StartLogInstanceWork())
	{
		LOG_TRACE("LogManager::Init error StartLogInstanceWork");
		Stop();
		return false;
	}

	//每秒触发一定定时器
	::OI::Timer::BaseTimerManager::GetInstance().AddTimer(this, 1000);
	return true;
}

void LogManager::Stop()
{
	// poll 和普通任务已停止。先同步排空已读的帧，再停止写线程。
	{
		::OI::Thread::MutexGuard stLock(m_stBacklogLocker);
		m_bStopping = true;
	}
	for (;;)
	{
		ResumeWaiting(true);
		::OI::Thread::MutexGuard stLock(m_stBacklogLocker);
		if (m_vWaitingSessions.empty())
		{
			break;
		}
		if (m_vWaitingSessions.front().iBytes > m_iMaxPendingBytes - m_iPendingBytes)
		{
			m_stBacklogCondition.Wait(m_stBacklogLocker);
		}
	}
	for (const auto& [sName, pLogFile] : m_mLogFileMap)
	{
		pLogFile->StopWriteThread();
	}
	fprintf(stderr, "logd backlog peak=%zu limit=%zu pauses=%zu pending=%zu\n",
	    m_iPeakPendingBytes, m_iMaxPendingBytes, m_iPauseCount, m_iPendingBytes);
}

bool LogManager::OnTimer()
{
	tm stNowCalendarTime = ::OI::Timer::GetLocalTime();
	time_t iNowTimestamp = ::OI::Timer::GetTime();

	bool bIsCrossDay = ::OI::Timer::CrossDay(m_iLastTimestamp, iNowTimestamp);
	m_stLastCalendarTime = stNowCalendarTime;
	m_iLastTimestamp = iNowTimestamp;

	if (bIsCrossDay)
	{
		OnCrossDay();
	}

	char vLogTitleText[LOG_TITLE_BUFF_SIZE];
	//只需要时分秒.
	size_t iLength = strftime(vLogTitleText, sizeof(vLogTitleText), "%T ", &stNowCalendarTime);
	assert(iLength < sizeof(vLogTitleText));
	vLogTitleText[iLength] = '\0'; //字符串结尾
	{
		::OI::Thread::MutexGuard stLock(m_stTitleLocker);
		m_sLogTitle.assign(vLogTitleText, iLength);
	}
	return true;
}

void LogManager::OnCrossDay()
{
	// 和写盘失败一样直接停。Release 下 assert 不在，不能接着往昨天的文件里写。
	if (!CheckAndCreateRootPath())
	{
		fprintf(stderr, "logd cross day mkdir failed: %s\n", m_sCurrentLogPath.c_str());
		fflush(stderr);
		std::abort();
	}
	if (!OpenAllLogFile())
	{
		fprintf(stderr, "logd cross day open failed: %s\n", m_sCurrentLogPath.c_str());
		fflush(stderr);
		std::abort();
	}
}

static std::string GetLogLvName(LOG_LEVEL iLevelParam)
{
	switch (iLevelParam)
	{
	case LOG_LEVEL::LOG_TRACE:
		return "trace";
	case LOG_LEVEL::LOG_DEBUG:
		return "debug";
	case LOG_LEVEL::LOG_INFO:
		return "info";
	case LOG_LEVEL::LOG_WARNING:
		return "warning";
	case LOG_LEVEL::LOG_ERROR:
		return "error";
	default:
		assert(false);
		return "unknow_lv";
	};
	return "unknow_lv";
}

bool LogManager::OpenLogFile(LOG_LEVEL iLevelParam)
{
	std::string sLogLevelName = GetLogLvName(iLevelParam);
	std::string sLogLevelPath = m_sCurrentLogPath + "/" + sLogLevelName;
	return m_mLogFileMap[iLevelParam]->CreateNewFile(sLogLevelPath.c_str());
}

bool LogManager::StartLogInstanceWork()
{
	for (const auto& [sName, pLogFile] : m_mLogFileMap)
	{
		if (!pLogFile->StartWriteThread())
		{
			return false;
		}
	}

	// 启动阶段，还没进 StartWork。等各级写线程跑起来就走。
	for (;;)
	{
		bool bAllWorking = true;
		for (const auto& [sName, pLogFile] : m_mLogFileMap)
		{
			if (!pLogFile->IsWorking())
			{
				bAllWorking = false;
				break;
			}
		}
		if (bAllWorking)
		{
			return true;
		}
		std::this_thread::yield();
	}
}

bool LogManager::OpenAllLogFile()
{
	for (int iLevelLv = 0; iLevelLv < LOG_LEVEL::LOG_LEVEL_COUNT; ++iLevelLv)
	{
		LOG_LEVEL iLevelLevel = (LOG_LEVEL)iLevelLv;
		if (m_mLogFileMap.find(iLevelLevel) == m_mLogFileMap.end())
		{
			m_mLogFileMap[iLevelLevel] = new LogFileInstance(this, iLevelLevel);
		}
		if (!OpenLogFile((LOG_LEVEL)iLevelLv))
		{
			return false;
		}
	}
	return true;
}

bool LogManager::CheckAndCreateRootPath()
{
	char vCurrentPath[64];
	size_t iLength = strftime(vCurrentPath, sizeof(vCurrentPath) - 1, "%y_%m_%d", &m_stLastCalendarTime);
	assert(iLength < sizeof(vCurrentPath) - 1);
	(void)iLength;
	m_sCurrentLogPath = m_sLogRootPath + "/" + vCurrentPath;
	return ::OI::File::MakeDirs(m_sCurrentLogPath);
}

bool LogManager::WriteLog(LOG_LEVEL iLevelParam, const char* pTextParam, size_t iLengthParam,
    LogServerSession* pSessionParam)
{
	const std::string sTitle = GetLogTitle();
	::OI::Thread::MutexGuard stLock(m_stBacklogLocker);
	if (iLevelParam < 0 || iLevelParam >= LOG_LEVEL_COUNT ||
	    iLengthParam > m_iMaxPendingBytes - sTitle.size())
	{
		return false;
	}
	const size_t iBytes = iLengthParam + sTitle.size();
	const bool bFull = iBytes > m_iMaxPendingBytes - m_iPendingBytes;
	// 新到的网络帧和本地日志不能越过已有等待者，避免大帧被小帧持续抢占。
	if (bFull || ((m_pResumingSession || !m_vWaitingSessions.empty()) &&
	                 (!pSessionParam || pSessionParam != m_pResumingSession)))
	{
		if (pSessionParam)
		{
			QueueWaiter(pSessionParam, iBytes);
		}
		return false;
	}
	::OI::Base::Bytes stBody;
	stBody.Reserve(iBytes);
	stBody.Append(sTitle.data(), sTitle.size());
	stBody.Append(pTextParam, iLengthParam);
	m_mLogFileMap.at(iLevelParam)->AddLog(stBody);
	m_iPendingBytes += iBytes;
	m_iPeakPendingBytes = std::max(m_iPeakPendingBytes, m_iPendingBytes);
	return true;
}

void LogManager::QueueWaiter(LogServerSession* pSessionParam, size_t iBytesParam)
{
	// 调用者持有积压锁；登记和容量检查原子完成，避免遗漏写线程的唤醒。
	if (iBytesParam > m_iMaxPendingBytes - m_iPendingBytes)
	{
		++m_iPauseCount;
	}
	m_vWaitingSessions.push_back({pSessionParam, iBytesParam});
	pSessionParam->AddRef();
	pSessionParam->WaitForWrite();
	ScheduleResume();
}

void LogManager::DeferWrite(LogServerSession* pSessionParam, size_t iLengthParam)
{
	const size_t iBytes = iLengthParam + GetLogTitle().size();
	::OI::Thread::MutexGuard stLock(m_stBacklogLocker);
	QueueWaiter(pSessionParam, iBytes);
}

void LogManager::CompleteWrite(size_t iBytesParam)
{
	::OI::Thread::MutexGuard stLock(m_stBacklogLocker);
	assert(iBytesParam <= m_iPendingBytes);
	m_iPendingBytes -= iBytesParam;
	m_stBacklogCondition.Signal();
	ScheduleResume();
}

void LogManager::ScheduleResume()
{
	// 只在队首整帧放得下时投递；满队列不会形成 PostOnLoop 空转。
	if (!m_bStopping && !m_bResumePosted && !m_pResumingSession && !m_vWaitingSessions.empty() &&
	    m_vWaitingSessions.front().iBytes <= m_iMaxPendingBytes - m_iPendingBytes)
	{
		m_bResumePosted = true;
		::OI::Net::Poll::GetInstance().PostOnLoop(ResumeOnLoop, this);
	}
}

void LogManager::ResumeOnLoop(void* pArgumentParam)
{
	static_cast<LogManager*>(pArgumentParam)->ResumeWaiting(false);
}

void LogManager::ResumeWaiting(bool bShutdownParam)
{
	LogServerSession* pSession = nullptr;
	{
		::OI::Thread::MutexGuard stLock(m_stBacklogLocker);
		m_bResumePosted = false;
		if (m_vWaitingSessions.empty() ||
		    m_vWaitingSessions.front().iBytes > m_iMaxPendingBytes - m_iPendingBytes)
		{
			return;
		}
		pSession = m_vWaitingSessions.front().pSession;
		m_vWaitingSessions.pop_front();
		m_pResumingSession = pSession;
	}
	pSession->ResumeWrite(bShutdownParam);
	{
		::OI::Thread::MutexGuard stLock(m_stBacklogLocker);
		m_pResumingSession = nullptr;
		ScheduleResume();
	}
	pSession->Release();
}

std::string LogManager::GetLogTitle()
{
	::OI::Thread::MutexGuard stLock(m_stTitleLocker);
	return m_sLogTitle;
}
