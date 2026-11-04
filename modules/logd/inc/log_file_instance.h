#pragma once
#include <stdio.h>
#include <list>
#include <map>
#include <atomic>
#include <thread>
#include "oi_bytes.h"
#include "oi_lock.h"

class LogManager;
class LogFileInstance
{
	friend class LogManager;
	LogManager* m_pManager;
	LOG_LEVEL m_iLevel;

	//文件锁
	::OI::Thread::Mutex m_stFileLocker;
	//日志文件handle
	FILE* m_vFiles[2];

	typedef std::list<::OI::Base::Bytes> LogCacheList;
	//缓存锁
	::OI::Thread::Mutex m_stCacheLocker;
	::OI::Thread::MutexCond m_stCacheCondition;

	//当前lv的日志缓存
	LogCacheList m_vLogs;
	std::thread m_stThread;
	std::atomic<bool> m_bWorking;
	// 由缓存锁保护。轮转也必须唤醒空队列上的写线程。
	bool m_bQuit;
	bool m_bRotationPending = false;

  public:
	LogFileInstance(LogManager* pManagerParam, LOG_LEVEL iLevelParam);
	~LogFileInstance();
	//跨天需要更换文件指针
	bool CreateNewFile(const char* pPathParam);
	void OnCreateNewFile(FILE* pNewFileParam);
	bool StartWriteThread();
	// 打退出标记并 join。返回时队列里的日志都已写盘并 fflush。
	void StopWriteThread();
	void WriteLog();
	bool IsWorking()
	{
		return m_bWorking;
	}

  private:
	// 只能由管理器在预留全局额度后调用。
	void AddLog(::OI::Base::Bytes& stLogParam);
	//实际log_thread处理积压日志时,再做切换
	FILE* GetActiveFile();
	//激活当前日志文件对应线程
	void Active();
	void Passive();
	static void* WriteLogThreadHandle(void* pArgumentParam);
};
