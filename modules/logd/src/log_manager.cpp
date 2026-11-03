#include "log_manager.h"
#include <stdio.h>
#include <string.h>
#include <cstdlib>
#include "oi_timer_helper.h"
#include "oi_file.h"
#include "log_file_instance.h"
#include "oi_config.h"

bool LogManager::Init()
{
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
	for (const auto& [sName, pLogFile] : m_mLogFileMap)
	{
		pLogFile->StopWriteThread();
	}
}

bool LogManager::OnTimer()
{
	tm	stNowCalendarTime = ::OI::Timer::GetLocalTime();
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
	vLogTitleText[iLength] = '\0';	//字符串结尾
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

void LogManager::WriteLog(LOG_LEVEL iLevelParam, ::OI::Base::Bytes& stSecondParam)
{
	m_mLogFileMap[iLevelParam]->AddLog(stSecondParam);
}

std::string LogManager::GetLogTitle()
{
	::OI::Thread::MutexGuard stLock(m_stTitleLocker);
	return m_sLogTitle;
}


