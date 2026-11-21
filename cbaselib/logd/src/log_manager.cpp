#include "oi_framework_timer.h"
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

int32_t LogManager::Init()
{
	int64_t iLimit = 64 * 1024 * 1024;
	TryGetConfig("Log.max_pending_bytes", iLimit);
	if (iLimit < ::OI::Log::LOG_FRAME_MAX + 8 || static_cast<uint64_t>(iLimit) > SIZE_MAX)
	{
		fprintf(stderr, "logd Log.max_pending_bytes must be at least %d and fit size_t\n", ::OI::Log::LOG_FRAME_MAX + 8);
		return OI_ERR_INVALID_ARGUMENT;
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

	UpdateLogPath();
	int32_t iStatus = StartLogInstanceWork();
	if (iStatus != OI_OK)
	{
		Stop();
		return iStatus;
	}

	//每秒触发一定定时器
	iStatus = ::OI::Timer::FrameworkTimer::AddTimer(this, 1000);
	if (iStatus != OI_OK)
	{
		Stop();
	}
	return iStatus;
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

	char vLogTitleText[LOG_TITLE_BUFF_SIZE];
	size_t iLength = strftime(vLogTitleText, sizeof(vLogTitleText), "%T ", &stNowCalendarTime);
	assert(iLength < sizeof(vLogTitleText));
	{
		::OI::Thread::MutexGuard stLock(m_stBacklogLocker);
		if (bIsCrossDay)
		{
			OnCrossDay();
		}
		::OI::Thread::MutexGuard stTitleLock(m_stTitleLocker);
		m_sLogTitle.assign(vLogTitleText, iLength);
	}

	return true;
}

static std::string GetLogLvName(LOG_LEVEL iLevel);

void LogManager::OnCrossDay()
{
	// 与 WriteLog 的入队共用积压锁，轮转标记不会越过已接纳的日志。
	try
	{
		UpdateLogPath();
		for (const auto& [iLevel, pLogFile] : m_mLogFileMap)
		{
			pLogFile->Rotate(m_sCurrentLogPath + "/" + GetLogLvName(iLevel));
		}
	}
	catch (...)
	{
		fprintf(stderr, "logd failed to enqueue rotation\n");
		std::abort();
	}
}

static std::string GetLogLvName(LOG_LEVEL iLevel)
{
	switch (iLevel)
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

int32_t LogManager::StartLogInstanceWork()
{
	for (int iLevel = 0; iLevel < LOG_LEVEL::LOG_LEVEL_COUNT; ++iLevel)
	{
		const LOG_LEVEL iLogLevel = static_cast<LOG_LEVEL>(iLevel);
		LogFileInstance* pLogFile = new LogFileInstance(this);
		m_mLogFileMap[iLogLevel] = pLogFile;
		const int32_t iStatus = pLogFile->StartWriteThread(m_sCurrentLogPath + "/" + GetLogLvName(iLogLevel));
		if (iStatus != OI_OK)
		{
			return iStatus;
		}
	}
	return OI_OK;
}

void LogManager::UpdateLogPath()
{
	char vCurrentPath[64];
	const size_t iLength = strftime(vCurrentPath, sizeof(vCurrentPath), "%y_%m_%d", &m_stLastCalendarTime);
	assert(iLength > 0 && iLength < sizeof(vCurrentPath));
	(void)iLength;
	m_sCurrentLogPath = m_sLogRootPath + "/" + vCurrentPath;
}

int32_t LogManager::WriteLog(LOG_LEVEL iLevel, const char* pText, size_t iLength,
    LogServerSession* pSession)
{
	::OI::Thread::MutexGuard stLock(m_stBacklogLocker);
	const std::string sTitle = GetLogTitle();
	if (iLevel < 0 || iLevel >= LOG_LEVEL_COUNT || (!pText && iLength > 0) ||
	    iLength > m_iMaxPendingBytes - sTitle.size())
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	const size_t iBytes = iLength + sTitle.size();
	const bool bFull = iBytes > m_iMaxPendingBytes - m_iPendingBytes;
	// 新到的网络帧和本地日志不能越过已有等待者，避免大帧被小帧持续抢占。
	if (bFull || ((m_pResumingSession || !m_vWaitingSessions.empty()) &&
	                 (!pSession || pSession != m_pResumingSession)))
	{
		if (pSession)
		{
			QueueWaiter(pSession, iBytes);
		}
		return OI_ERR_BUSY;
	}
	::OI::Base::Bytes stBody;
	stBody.Reserve(iBytes);
	stBody.Append(sTitle.data(), sTitle.size());
	stBody.Append(pText, iLength);
	m_mLogFileMap.at(iLevel)->AddLog(stBody);
	m_iPendingBytes += iBytes;
	m_iPeakPendingBytes = std::max(m_iPeakPendingBytes, m_iPendingBytes);
	return OI_OK;
}

void LogManager::QueueWaiter(LogServerSession* pSession, size_t iBytes)
{
	// 调用者持有积压锁；登记和容量检查原子完成，避免遗漏写线程的唤醒。
	if (iBytes > m_iMaxPendingBytes - m_iPendingBytes)
	{
		++m_iPauseCount;
	}
	m_vWaitingSessions.push_back({pSession, iBytes});
	pSession->AddRef();
	pSession->WaitForWrite();
	ScheduleResume();
}

int32_t LogManager::DeferWrite(LogServerSession* pSession, size_t iLength)
{
	const size_t iTitleBytes = GetLogTitle().size();
	if (iLength > m_iMaxPendingBytes - iTitleBytes)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	const size_t iBytes = iLength + iTitleBytes;
	::OI::Thread::MutexGuard stLock(m_stBacklogLocker);
	QueueWaiter(pSession, iBytes);
	return OI_OK;
}

void LogManager::CompleteWrite(size_t iBytes)
{
	::OI::Thread::MutexGuard stLock(m_stBacklogLocker);
	assert(iBytes <= m_iPendingBytes);
	m_iPendingBytes -= iBytes;
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

void LogManager::ResumeOnLoop(void* pArgument)
{
	static_cast<LogManager*>(pArgument)->ResumeWaiting(false);
}

void LogManager::ResumeWaiting(bool bShutdown)
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
	pSession->ResumeWrite(bShutdown);
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
