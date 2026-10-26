#pragma once
#include <time.h>
#include "base_header.h"

enum class DT_SECOND {};
enum class DT_MINUTE {};
enum class DT_HOUR {};
enum class DT_DAY {};
enum class DT_WEEK {};
enum class DT_MONTH {};
enum class DT_YEAR {};

namespace TimerHelper
{
	static time_t	GetTime()		{	return BaseTimerManager::GetInstance()->GetTime();	}
	//拷贝一份,防止多线程翻车,可能只赋值一部分
	static tm		GetLocalTime()	{	return BaseTimerManager::GetInstance()->GetTm();	}
	static timeval	GetCurTickTime(){	return BaseTimerManager::GetInstance()->GetCurTickTime(); }
	
	//跨天计算

	template<typename DATE_TYPE>
	static bool IsCross(const struct tm& old_tm, const struct tm& new_tm);
	template<typename DATE_TYPE>
	static bool IsCross(const struct tm& old_tm){	return IsCross<DATE_TYPE>(old_tm, GetLocalTime());	}
	template<typename DATE_TYPE>
	static bool IsCross(const time_t& old_ts)	{	return IsCross<DATE_TYPE>(old_ts, GetTime());	}
	template<typename DATE_TYPE>
	static bool IsCross(const time_t& old_ts, const time_t& now_ts)
	{
		struct tm old, now;
		localtime_r(&old_ts, &old);
		localtime_r(&now_ts, &now);
		return IsCross<DATE_TYPE>(old, now);
	}

	template<typename DATE_TYPE>
	static bool IsSame(const struct tm& old_tm, const struct tm& now_tm);

	template<typename DATE_TYPE>
	static bool IsSame(const struct tm& old_tm)		{	return IsSame<DATE_TYPE>(old_tm, GetLocalTime());  }

	template<typename DATE_TYPE>
	static bool IsSame(time_t old_ts)				{	return IsSame<DATE_TYPE>(old_ts, GetTime());  }

	template<typename DATE_TYPE>
	static bool IsSame(time_t old_ts, time_t now_ts)
	{
		struct tm old, now;
		localtime_r(&old_ts, &old);
		localtime_r(&now_ts, &now);
		return IsSame<DATE_TYPE>(old, now);
	}
};





template<>
inline bool TimerHelper::IsSame<DT_YEAR>(const struct tm& old_tm, const struct tm& now_tm)
{
	return old_tm.tm_year == now_tm.tm_year;
}

//TODO 未实现
//TODO 同周计算,不带时间有点难算
template<>
inline bool TimerHelper::IsSame<DT_WEEK>(const tm& old_tm, const tm& now_tm) __attribute__((deprecated));
template<>
bool TimerHelper::IsSame<DT_WEEK>(const tm& old_tm, const tm& now_tm) 
{
	assert(false);
	return false;
}

#define REGISTER_IS_SAME_FUNCTION(DT, NEXT_DT, VAL_NAME) \
template<>\
inline bool TimerHelper::IsSame<DT>(const struct tm& old_tm, const struct tm& now_tm)\
{\
	return IsSame<NEXT_DT>(old_tm, now_tm) && old_tm.VAL_NAME == now_tm.VAL_NAME;\
}

REGISTER_IS_SAME_FUNCTION(DT_MONTH, DT_YEAR, tm_mon)			//月相同需要比较年相同
REGISTER_IS_SAME_FUNCTION(DT_DAY, DT_YEAR, tm_yday)				//天相同需要比较年相同
REGISTER_IS_SAME_FUNCTION(DT_HOUR, DT_DAY, tm_hour)				//小时相同需要比较天相同
REGISTER_IS_SAME_FUNCTION(DT_MINUTE, DT_HOUR, tm_min)			//分钟相同需要比较小时相同
REGISTER_IS_SAME_FUNCTION(DT_SECOND, DT_MINUTE, tm_sec)			//秒相同需要先比较分钟相同


//IsCross的实现

#define REGISTER_IS_CROSS_FUNCTION(DT, NEXT_DT, VAL_NAME) \
template<>\
inline bool TimerHelper::IsCross<DT>(const tm& old_tm, const tm& now_tm)\
{\
	return IsCross<NEXT_DT>(old_tm, now_tm) || (IsSame<NEXT_DT>(old_tm, now_tm) && old_tm.VAL_NAME < now_tm.VAL_NAME);\
}


template<>
inline bool TimerHelper::IsCross<DT_YEAR>(const tm& old_tm, const tm& now_tm)
{
	return old_tm.tm_year < now_tm.tm_year;
}

//TODO 未实现
//TODO 跨周计算,不带时间还有点难算,带时间就直接计算86400*7 和week值即可
template<>
bool TimerHelper::IsCross<DT_WEEK>(const tm& old_tm, const tm& now_tm) __attribute__((deprecated));
template<> 
bool TimerHelper::IsCross<DT_WEEK>(const tm& old_tm, const tm& now_tm)
{
	assert(false);
	return false;
}

REGISTER_IS_CROSS_FUNCTION(DT_MONTH, DT_YEAR, tm_mon)		//--
REGISTER_IS_CROSS_FUNCTION(DT_DAY, DT_YEAR, tm_yday)	//--
REGISTER_IS_CROSS_FUNCTION(DT_HOUR, DT_DAY, tm_hour)	//--
REGISTER_IS_CROSS_FUNCTION(DT_MINUTE, DT_HOUR, tm_min)		//跨小时 || 同小时值大
REGISTER_IS_CROSS_FUNCTION(DT_SECOND,	DT_MINUTE,	tm_sec)		//跨分钟 || 同分钟值大
