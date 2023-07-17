#pragma once
#include <map>
#include "base_header.h"

class LogFileInstance;

class LogManager : public BaseTimer
{
	//root_path + year_month_day
	//12:09:08 service_type-service_id:
	enum
	{
		LOG_TITLE_LEVEL_SIZE = 60,
		LOG_TITLE_BUFF_SIZE = 128,
	};
	//上一次创建log文件的时间戳,跨天重新建立
	struct tm	_last_tm;
	time_t		_last_ts;

	//日志title无锁队列[时间的缓存]
	int			_log_title_index;											//当前log_title索引
	char		_log_title_data[LOG_TITLE_LEVEL_SIZE][LOG_TITLE_BUFF_SIZE];	//备选数组,方便切换title,每次OnTimer修改index数据，切换index

	//日志lv对应的文件和缓存包装
	typedef std::map<LOG_LEVEL, LogFileInstance*>	LogFileMap;
	LogFileMap	_log_file_map;

	//日志文件根目录
	std::string _log_root_path;		//日志根目录
	std::string _cur_log_path;		//当天的日志目录 _log_root_path/year_month_day

	//单例对象
	static LogManager inst;
public:
	LogManager() {}
	~LogManager() {}
	bool Init();
	static LogManager& GetInstance() { return inst;  }

	void WriteLog(LOG_LEVEL lv, Bytes& b);

private:
	friend class LogFileInstance;
	void StartLogInstanceWork();
	bool OpenAllLogFile();
	bool CheckAndCreateRootPath();
	bool OpenLogFile(LOG_LEVEL lv);
	void OnCrossDay();
	virtual bool OnTimer();
	char* GetLogTitle(int index = -1);
};