#include <string.h>
#include "log_file_instance.h"
#include "threadpool.h"
#include "logger.h"
#include "log_manager.h"

#ifdef _SINGLE_THREAD_
	#error LOG_SERVER MUST BE MULTI THREAD
#endif

LogFileInstance::LogFileInstance(LogManager* p_mgr, LOG_LEVEL lv):_p_mgr(p_mgr), _lv(lv), _pid(0), _working(false)
{
	memset(_fp, 0, sizeof(_fp));
	_write_size = 0;
}

LogFileInstance::~LogFileInstance()
{
	fflush(GetActiveFile());	//临死把文件写完
	fclose(GetActiveFile());
}


bool LogFileInstance::CreateNewFile(const char* path)
{
	FILE* fp = fopen(path, "a+");
	if (!fp)
	{
		return false;
	}
	OnCreateNewFile(fp);
	return true;
}

void LogFileInstance::OnCreateNewFile(FILE* new_file)
{
	{
		MutexGuard _l(_file_locker);
		assert(_fp[1] == NULL);
		_fp[1] = new_file;
	}
	Active();
}

void LogFileInstance::StartWriteThread()
{
	ThreadPool::CreateSpecThread(_pid, LogFileInstance::WriteLogThreadHandle, this);
}

FILE* LogFileInstance::GetActiveFile()
{
	MutexGuard _l(_file_locker);
	if (_fp[1])
	{
		if (_fp[0])
		{
			fclose(_fp[0]);
		}
		_fp[0] = _fp[1];
		_fp[1] = NULL;
	}
	return _fp[0];
}

void LogFileInstance::Active()
{
	_cache_cond.Signal();
}

void LogFileInstance::Passive()
{
	_cache_cond.Wait(_cache_locker);
}


void LogFileInstance::AddLog(const Bytes& log)
{
	bool notify = false;

	{
		MutexGuard l(_cache_locker);
		if (_logs.empty())
		{
			notify = true;
		}
		_logs.push_back(log);
		if (notify)
		{
			Active();
		}
	}
}

void* LogFileInstance::WriteLogThreadHandle(void* arg)
{
	LogFileInstance* p_inst = (LogFileInstance*)arg;
	p_inst->WriteLog();
	return NULL;
}

const char* LogFileInstance::GetLogTiTle()
{
	return _p_mgr->GetLogTitle();
}

void LogFileInstance::Dump()
{
	LOG_TRACE("LogFileInstance::Dump lv=%d, write_size=%ld", _lv, _write_size);
}

void LogFileInstance::WriteLog()
{
	assert(pthread_self() == _pid);
	_working = true;


	//留10MB的buff,
	Bytes tmp;
	tmp.Reserve(10485760);
	for(;;)
	{
		FILE* fp = GetActiveFile();

		LogCacheList tmp_logs;
		_cache_locker.Lock();

		//TODO 这里先拿全量,回头再优化
		tmp_logs.swap(_logs);

		if (tmp_logs.empty())
		{
			//没有日志等唤醒
			Passive();
		}
		_cache_locker.UnLock();


		const char* time_title = GetLogTiTle();
		for (LogCacheList::iterator it = tmp_logs.begin(); it != tmp_logs.end(); ++it)
		{
			tmp.Append(time_title, strlen(time_title));
			tmp.Append(*it);
		}

		size_t w_size = fwrite(tmp.Begin(), tmp.Size(), 1, fp);
		if (w_size < 1)
		{
			//TODO 
		}
		fflush(fp);
		_write_size += tmp.Size();
		//这里不会缩容.主要是修改data_len
		tmp.Reserve(0);
		assert(tmp.Size() == 0 && tmp.Captity() > 0);
	}
}

