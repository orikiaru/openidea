#pragma once
#include <stdio.h>
#include <list>
#include <map>
#include "bytes.h"
#include "spin_lock.h"

class LogManager;
class LogFileInstance
{
	LogManager* _p_mgr;
	LOG_LEVEL	_lv;

	//文件锁
	Mutex		_file_locker;
	//日志文件handle
	FILE*		_fp[2];

	typedef std::list<Bytes> LogCacheList;
	//缓存锁
	Mutex			_cache_locker;
	MutexCond		_cache_cond;

	//当前lv的日志缓存
	LogCacheList	_logs;
	pthread_t		_pid;
	bool			_working;

	//当前写入日志总数
	int64_t			_write_size;


public:
	LogFileInstance(LogManager* p_mgr, LOG_LEVEL lv);
	~LogFileInstance();
	//跨天需要更换文件指针
	bool CreateNewFile(const char* path);
	void OnCreateNewFile(FILE* new_file);
	void StartWriteThread();
	void AddLog(const Bytes& log);
	void WriteLog();
	bool IsWorking() { return _working; }
	void Dump();
private:
	//实际log_thread处理积压日志时,再做切换
	FILE* GetActiveFile();
	//激活当前日志文件对应线程
	void Active();
	void Passive();
	static void* WriteLogThreadHandle(void* arg);
	const char* GetLogTiTle();
};