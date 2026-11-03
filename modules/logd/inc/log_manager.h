#pragma once
#include <map>
#include "oi_base.h"
#include "oi_singleton.h"

class LogFileInstance;

class LogManager : public ::OI::Timer::BaseTimer
{
	DECLARE_SINGLETON(LogManager)
	LogManager() = default;
	//root_path + year_month_day
	//12:09:08 service_type-service_id:
	enum
	{
		LOG_TITLE_BUFF_SIZE = 128,
	};
	//上一次创建log文件的时间戳,跨天重新建立
	struct tm m_stLastCalendarTime;
	time_t m_iLastTimestamp;

	::OI::Thread::Mutex m_stTitleLocker;
	std::string m_sLogTitle;

	//日志lv对应的文件和缓存包装
	typedef std::map<LOG_LEVEL, LogFileInstance*> LogFileMap;
	LogFileMap m_mLogFileMap;

	//日志文件根目录
	std::string m_sLogRootPath;    //日志根目录
	std::string m_sCurrentLogPath; //当天的日志目录 _log_root_path/year_month_day

  public:
	~LogManager()
	{
	}
	bool Init();
	// StartWork 返回后调用。join 各级写线程，返回时日志都已落盘。
	void Stop();

	// b 的内容会被移进写盘队列
	void WriteLog(LOG_LEVEL iLevelParam, ::OI::Base::Bytes& stSecondParam);

  private:
	friend class LogFileInstance;
	bool StartLogInstanceWork();
	bool OpenAllLogFile();
	bool CheckAndCreateRootPath();
	bool OpenLogFile(LOG_LEVEL iLevelParam);
	void OnCrossDay();
	virtual bool OnTimer();
	std::string GetLogTitle();
};
