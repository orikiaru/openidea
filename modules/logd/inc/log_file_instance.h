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
	LogManager* _p_mgr;
	LOG_LEVEL	_lv;

	//文件锁
	::OI::Thread::Mutex		_file_locker;
	//日志文件handle
	FILE*		_fp[2];

	typedef std::list<::OI::Base::Bytes> LogCacheList;
	//缓存锁
	::OI::Thread::Mutex			_cache_locker;
	::OI::Thread::MutexCond		_cache_cond;

	//当前lv的日志缓存
	LogCacheList	_logs;
	std::thread		_thread;
	std::atomic<bool>	_working;
	// 由 _cache_locker 保护。写线程看到后把 _logs 写完再返回。
	bool			_quit;


public:
	LogFileInstance(LogManager* p_mgr, LOG_LEVEL lv);
	~LogFileInstance();
	//跨天需要更换文件指针
	bool CreateNewFile(const char* path);
	void OnCreateNewFile(FILE* new_file);
	void StartWriteThread();
	// 打退出标记并 join。返回时队列里的日志都已写盘并 fflush。
	void StopWriteThread();
	// 会把 log 的内容移走
	void AddLog(::OI::Base::Bytes& log);
	void WriteLog();
	bool IsWorking() { return _working; }
private:
	//实际log_thread处理积压日志时,再做切换
	FILE* GetActiveFile();
	//激活当前日志文件对应线程
	void Active();
	void Passive();
	static void* WriteLogThreadHandle(void* arg);
	std::string GetLogTiTle();
};
