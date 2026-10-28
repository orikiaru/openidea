#include <string.h>
#include "log_file_instance.h"
#include "threadpool.h"
#include "logger.h"
#include "log_manager.h"

#ifdef _SINGLE_THREAD_
	#error LOG_SERVER MUST BE MULTI THREAD
#endif

LogFileInstance::LogFileInstance(LogManager* p_mgr, LOG_LEVEL lv):_p_mgr(p_mgr), _lv(lv), _working(false)
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
	// 交给写线程按块刷。这里只把 stdio 缓冲放大，避免每条日志一次系统调用。
	setvbuf(fp, NULL, _IOFBF, 1024 * 1024);
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


void LogFileInstance::AddLog(Bytes& log)
{
	bool notify = false;

	{
		MutexGuard l(_cache_locker);
		if (_logs.empty())
		{
			notify = true;
		}
		_logs.push_back(std::move(log));
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

static bool WriteAll(FILE* fp, const char* data, size_t size)
{
	while (size > 0)
	{
		size_t wrote = fwrite(data, 1, size, fp);
		if (wrote == 0)
		{
			return false;
		}
		data += wrote;
		size -= wrote;
	}
	return true;
}

void LogFileInstance::WriteLog()
{
	assert(std::this_thread::get_id() == _pid);
	_working = true;

	// 攒到一块再写。压力上来时大约 8MB 刷一次，队列空了把尾巴刷掉。
	const size_t kChunk = 8 * 1024 * 1024;
	Bytes tmp;
	tmp.Reserve(kChunk);
	int64_t unflushed = 0;
	for(;;)
	{
		FILE* fp = GetActiveFile();

		LogCacheList tmp_logs;
		_cache_locker.Lock();
		tmp_logs.swap(_logs);
		if (tmp_logs.empty() && tmp.Size() == 0)
		{
			Passive();
			_cache_locker.UnLock();
			continue;
		}
		_cache_locker.UnLock();

		if (!tmp_logs.empty())
		{
			const char* time_title = GetLogTiTle();
			size_t title_len = time_title ? strlen(time_title) : 0;
			for (LogCacheList::iterator it = tmp_logs.begin(); it != tmp_logs.end(); ++it)
			{
				if (title_len > 0)
				{
					tmp.Append(time_title, title_len);
				}
				tmp.Append(*it);
				if (tmp.Size() >= kChunk)
				{
					if (fp && WriteAll(fp, tmp.Begin(), tmp.Size()))
					{
						_write_size += (int64_t)tmp.Size();
						unflushed += (int64_t)tmp.Size();
					}
					tmp.Reserve(0);
					if (fp && unflushed >= (int64_t)kChunk)
					{
						fflush(fp);
						unflushed = 0;
					}
				}
			}
		}

		if (tmp_logs.empty() && tmp.Size() > 0)
		{
			if (fp && WriteAll(fp, tmp.Begin(), tmp.Size()))
			{
				_write_size += (int64_t)tmp.Size();
			}
			tmp.Reserve(0);
			if (fp)
			{
				fflush(fp);
			}
			unflushed = 0;
		}
	}
}

