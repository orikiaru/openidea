#pragma once
#include <stdio.h>
#include <list>
#include <map>
#include <thread>
#include "oi_bytes.h"
#include "oi_lock.h"

class LogManager;
class LogFileInstance
{
	friend class LogManager;
	LogManager* m_pManager;

	struct LogEntry
	{
		::OI::Base::Bytes stData;
		std::string sRotationPath;
	};
	typedef std::list<LogEntry> LogCacheList;
	//缓存锁
	::OI::Thread::Mutex m_stCacheLocker;
	::OI::Thread::MutexCond m_stCacheCondition;

	//当前lv的日志缓存
	LogCacheList m_vLogs;
	std::thread m_stThread;
	// 由缓存锁保护。轮转也必须唤醒空队列上的写线程。
	bool m_bQuit;
	bool m_bStarted = false;
	int32_t m_iStartStatus = OI_OK;
	std::string m_sInitialPath;

  public:
	explicit LogFileInstance(LogManager* pManager);
	~LogFileInstance();
	//跨天需要更换文件指针
	int32_t StartWriteThread(const std::string& sPath);
	// 打退出标记并 join。返回时队列里的日志都已写盘并 fflush。
	void StopWriteThread();
	void WriteLog();

  private:
	// 只能由管理器在预留全局额度后调用。
	void AddLog(::OI::Base::Bytes& stLog);
	//实际log_thread处理积压日志时,再做切换
	void Rotate(const std::string& sPath);
	//激活当前日志文件对应线程
	void Active();
	void Passive();
	static void* WriteLogThreadHandle(void* pArgument);
};
