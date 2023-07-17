#include "log_manager.h"
#include "base_timer_helper.h"
#include "file_helper.h"
#include "log_file_instance.h"
#include "config.h"


LogManager LogManager::inst;
bool LogManager::Init()
{
	_last_ts = TimerHelper::GetTime();
	_last_tm = TimerHelper::GetLocalTime();

	std::string log_root_path = "./";
	TryGetConfig("Log.root_path", log_root_path);
	_log_root_path = log_root_path;

	if (!CheckAndCreateRootPath())
	{
		LOG_TRACE("LogManager::Init error CheckAndCreateRootPath=%s", _log_root_path.c_str());
		return false;
	}

	if (!OpenAllLogFile())
	{
		LOG_TRACE("LogManager::Init error OpenAllLogFile=%s", _log_root_path.c_str());
		return false;
	}

	StartLogInstanceWork();

	//每秒触发一定定时器
	BaseTimerManager::GetInstance()->AddTimer(this, 1000);



	return true;
}

bool LogManager::OnTimer()
{
	tm	_now_tm = TimerHelper::GetLocalTime();
	time_t _now_ts = TimerHelper::GetTime();

	bool is_cross_day = TimerHelper::IsCross<DT_DAY>(_last_tm, _now_tm);
	_last_tm = _now_tm;
	_last_ts = _now_ts;

	if (is_cross_day)
	{
		OnCrossDay();
	}
	
	int next_index = (this->_log_title_index + 1) / LOG_TITLE_LEVEL_SIZE;
	char* log_title_str = GetLogTitle(next_index);
	//只需要时分秒.
	int len = strftime(log_title_str, LOG_TITLE_LEVEL_SIZE - 1, "%T ", &_now_tm);
	assert(len < LOG_TITLE_LEVEL_SIZE - 1);
	log_title_str[len] = '\0';	//字符串结尾
	_log_title_index = next_index;

	for (LogFileMap::iterator it = _log_file_map.begin(); it != _log_file_map.end(); ++it)
	{
		it->second->Dump();
	}
	return true;
}

void LogManager::OnCrossDay()
{
	if (!CheckAndCreateRootPath())
	{
		assert(false && "日志服务创建不了根目录直接嘎掉");
	}
	if (!OpenAllLogFile())
	{
		assert(false && "日志服务打不了文件,直接嘎掉");
	}
}

static std::string GetLogLvName(LOG_LEVEL lv)
{
	switch (lv)
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

bool LogManager::OpenLogFile(LOG_LEVEL lv)
{
	std::string log_lv_name = GetLogLvName(lv);
	std::string log_lv_path = _cur_log_path + "/" + log_lv_name;
	return _log_file_map[lv]->CreateNewFile(log_lv_path.c_str());
}

void LogManager::StartLogInstanceWork()
{
	for (LogFileMap::iterator it = _log_file_map.begin(); it != _log_file_map.end(); ++it)
	{
		it->second->StartWriteThread();
	}

	for (;;)
	{
		bool all_working = true;
		for (LogFileMap::iterator it = _log_file_map.begin(); it != _log_file_map.end(); ++it)
		{
			if (!it->second->IsWorking())
			{
				all_working = false;
				break;
			}
		}
		if (all_working)
		{
			break;
		}
	}
}


bool LogManager::OpenAllLogFile()
{
	for (int lv = 0; lv < LOG_LEVEL::LOG_LEVEL_COUNT; ++lv)
	{
		LOG_LEVEL level = (LOG_LEVEL)lv;
		if (_log_file_map.find(level) == _log_file_map.end())
		{
			_log_file_map[level] = new LogFileInstance(this, level);
		}
		if (!OpenLogFile((LOG_LEVEL)lv))
		{
			return false;
		}
	}
	return true;
}

bool LogManager::CheckAndCreateRootPath()
{
	char cur_path[64];
	size_t len = strftime(cur_path, sizeof(cur_path) - 1, "%y_%m_%d", &_last_tm);
	assert(len < sizeof(cur_path) - 1);
	_cur_log_path = _log_root_path + "/" + cur_path;
	return FileHelper::MakeDir(_cur_log_path);
}

void LogManager::WriteLog(LOG_LEVEL lv, Bytes& b)
{
	_log_file_map[lv]->AddLog(b);
}

char* LogManager::GetLogTitle(int index)
{
	if (index == -1)
	{
		return _log_title_data[_log_title_index];
	}
	else if(index >= 0 &&  index <= LOG_LEVEL_COUNT - 1)
	{
		return _log_title_data[index];
	}
	assert(false);
	return _log_title_data[_log_title_index];
}


