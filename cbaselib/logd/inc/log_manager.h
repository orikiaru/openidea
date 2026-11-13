#pragma once
#include <map>
#include <deque>
#include "oi_base.h"
#include "oi_singleton.h"

class LogFileInstance;
class LogServerSession;

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
	::OI::Thread::Mutex m_stBacklogLocker;
	::OI::Thread::MutexCond m_stBacklogCondition;
	size_t m_iMaxPendingBytes = 64 * 1024 * 1024;
	size_t m_iPendingBytes = 0;
	size_t m_iPeakPendingBytes = 0;
	size_t m_iPauseCount = 0;
	struct WaitingLog
	{
		LogServerSession* pSession;
		size_t iBytes;
	};
	std::deque<WaitingLog> m_vWaitingSessions;
	LogServerSession* m_pResumingSession = nullptr;
	bool m_bResumePosted = false;
	bool m_bStopping = false;

	//日志文件根目录
	std::string m_sLogRootPath;    //日志根目录
	std::string m_sCurrentLogPath; //当天的日志目录 _log_root_path/year_month_day

  public:
	~LogManager()
	{
	}
	int32_t Init();
	// StartWork 返回后调用。join 各级写线程，返回时日志都已落盘。
	void Stop();

	// 满时不消费正文；网络会话登记恢复，本地调用返回 OI_ERR_BUSY。
	int32_t WriteLog(LOG_LEVEL iLevel, const char* pText, size_t iLength,
	    LogServerSession* pSession = nullptr);
	// poll 单次解析达到预算，保留下一帧并排到等待队列尾部。
	void DeferWrite(LogServerSession* pSession, size_t iLength);

  private:
	friend class LogFileInstance;
	void CompleteWrite(size_t iBytes);
	void QueueWaiter(LogServerSession* pSession, size_t iBytes);
	void ScheduleResume();
	static void ResumeOnLoop(void* pArgument);
	void ResumeWaiting(bool bShutdown);
	int32_t StartLogInstanceWork();
	int32_t OpenAllLogFile();
	int32_t CheckAndCreateRootPath();
	int32_t OpenLogFile(LOG_LEVEL iLevel);
	void OnCrossDay();
	virtual bool OnTimer();
	std::string GetLogTitle();
};
