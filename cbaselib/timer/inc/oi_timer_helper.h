#pragma once
#include <time.h>
#include <stdint.h>
#include <sys/time.h>

namespace OI
{
namespace Timer
{
// 存盘用。ts 是当时的时间，offset_sec 是切点偏移。
// offset_sec 改成 86400 就是整段再偏一天，已存的 ts 不用改。
// 日切想改到 05:00，把 offset_sec 设成 -5*3600。
// CrossWeek 从本地周一零点切。CrossWeekday 把这道切线挪到指定的星期几，比如每周三刷新。
// 和 struct tm 的 tm_wday 同一个数：0 周日，1 周一，一直到 6 周六。
enum Weekday
{
	WeekdaySunday = 0,
	WeekdayMonday = 1,
	WeekdayTuesday = 2,
	WeekdayWednesday = 3,
	WeekdayThursday = 4,
	WeekdayFriday = 5,
	WeekdaySaturday = 6
};

struct TimeMark
{
	time_t iTimestamp;
	int64_t iOffsetSeconds;
	TimeMark()
	    : iTimestamp(0), iOffsetSeconds(0)
	{
	}
};

time_t GetTime();
tm GetLocalTime();
timeval GetCurTickTime();

time_t Shift(time_t iTimestampParam, int64_t iOffsetSecondsParam);
TimeMark Capture(int64_t iOffsetSecondsParam = 0);

int64_t DayPeriod(time_t iTimestampParam, int64_t iOffsetSecondsParam = 0);
int64_t WeekPeriod(time_t iTimestampParam, int64_t iOffsetSecondsParam = 0);
int64_t WeekdayPeriod(time_t iTimestampParam, int iWeekdayParam, int64_t iOffsetSecondsParam = 0);
int64_t MonthPeriod(time_t iTimestampParam, int64_t iOffsetSecondsParam = 0);
int64_t YearPeriod(time_t iTimestampParam, int64_t iOffsetSecondsParam = 0);

bool SameDay(time_t iFirstParam, time_t iSecondParam, int64_t iOffsetSecondsParam = 0);
bool SameWeek(time_t iFirstParam, time_t iSecondParam, int64_t iOffsetSecondsParam = 0);
bool SameWeekday(time_t iFirstParam, time_t iSecondParam, int iWeekdayParam, int64_t iOffsetSecondsParam = 0);
bool SameMonth(time_t iFirstParam, time_t iSecondParam, int64_t iOffsetSecondsParam = 0);
bool SameYear(time_t iFirstParam, time_t iSecondParam, int64_t iOffsetSecondsParam = 0);

// now 落到了比 old 更晚的一格才算跨过。时间往回走不算。
bool CrossDay(time_t iOldTimestampParam, time_t iNowTimestampParam, int64_t iOffsetSecondsParam = 0);
bool CrossWeek(time_t iOldTimestampParam, time_t iNowTimestampParam, int64_t iOffsetSecondsParam = 0);
bool CrossWeekday(time_t iOldTimestampParam, time_t iNowTimestampParam, int iWeekdayParam, int64_t iOffsetSecondsParam = 0);
bool CrossMonth(time_t iOldTimestampParam, time_t iNowTimestampParam, int64_t iOffsetSecondsParam = 0);
bool CrossYear(time_t iOldTimestampParam, time_t iNowTimestampParam, int64_t iOffsetSecondsParam = 0);

bool CrossDay(const TimeMark& stSavedParam, time_t iNowTimestampParam);
bool CrossWeek(const TimeMark& stSavedParam, time_t iNowTimestampParam);
bool CrossWeekday(const TimeMark& stSavedParam, time_t iNowTimestampParam, int iWeekdayParam);
bool CrossMonth(const TimeMark& stSavedParam, time_t iNowTimestampParam);
bool CrossYear(const TimeMark& stSavedParam, time_t iNowTimestampParam);
}
}
