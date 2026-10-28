#include "oi_timer_helper.h"
#include "oi_timer_manager.h"

namespace OI { namespace Timer
{
namespace
{
int64_t FloorDiv(int64_t a, int64_t b)
{
	int64_t q = a / b;
	int64_t r = a % b;
	if (r != 0 && ((a < 0) != (b < 0)))
	{
		--q;
	}
	return q;
}

int64_t CivilDays(int year, unsigned month, unsigned day)
{
	year -= month <= 2;
	const int era = (year >= 0 ? year : year - 399) / 400;
	const unsigned yoe = (unsigned)(year - era * 400);
	const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

bool ToLocal(time_t ts, int64_t offset_sec, tm* out)
{
	int64_t shifted = (int64_t)ts + offset_sec;
	time_t clock = (time_t)shifted;
	return localtime_r(&clock, out) != NULL;
}

int64_t DayOf(const tm& local)
{
	return CivilDays(local.tm_year + 1900, (unsigned)local.tm_mon + 1, (unsigned)local.tm_mday);
}

int64_t WeekStarting(const tm& local, int weekday)
{
	// 1970-01-01 是周四。weekday 用 tm_wday 的数，周一就是现在的 CrossWeek。
	return FloorDiv(DayOf(local) + (4 - weekday), 7);
}

int64_t WeekOf(const tm& local)
{
	return WeekStarting(local, WeekdayMonday);
}

bool KnownWeekday(int weekday)
{
	return weekday >= WeekdaySunday && weekday <= WeekdaySaturday;
}

bool WeekdaySlot(time_t ts, int weekday, int64_t offset_sec, int64_t* out)
{
	tm local;
	if (!KnownWeekday(weekday) || !ToLocal(ts, offset_sec, &local))
	{
		return false;
	}
	*out = WeekStarting(local, weekday);
	return true;
}

int64_t MonthOf(const tm& local)
{
	return (int64_t)local.tm_year * 12 + local.tm_mon;
}

int64_t YearOf(const tm& local)
{
	return local.tm_year;
}

typedef int64_t (*SlotOf)(const tm&);

bool Slot(time_t ts, int64_t offset_sec, SlotOf slot_of, int64_t* out)
{
	tm local;
	if (!ToLocal(ts, offset_sec, &local))
	{
		return false;
	}
	*out = slot_of(local);
	return true;
}

int64_t PeriodOf(time_t ts, int64_t offset_sec, SlotOf slot_of)
{
	int64_t slot = 0;
	if (!Slot(ts, offset_sec, slot_of, &slot))
	{
		return 0;
	}
	return slot;
}

bool SameOf(time_t a, time_t b, int64_t offset_sec, SlotOf slot_of)
{
	int64_t left = 0;
	int64_t right = 0;
	if (!Slot(a, offset_sec, slot_of, &left) || !Slot(b, offset_sec, slot_of, &right))
	{
		return false;
	}
	return left == right;
}

bool CrossOf(time_t old_ts, time_t now_ts, int64_t offset_sec, SlotOf slot_of)
{
	int64_t old_slot = 0;
	int64_t now_slot = 0;
	if (!Slot(old_ts, offset_sec, slot_of, &old_slot) || !Slot(now_ts, offset_sec, slot_of, &now_slot))
	{
		return false;
	}
	return now_slot > old_slot;
}
}

time_t GetTime()
{
	return BaseTimerManager::GetInstance().GetTime();
}

tm GetLocalTime()
{
	return BaseTimerManager::GetInstance().GetTm();
}

timeval GetCurTickTime()
{
	return BaseTimerManager::GetInstance().GetCurTickTime();
}

time_t Shift(time_t ts, int64_t offset_sec)
{
	return (time_t)((int64_t)ts + offset_sec);
}

TimeMark Capture(int64_t offset_sec)
{
	TimeMark mark;
	mark.ts = GetTime();
	mark.offset_sec = offset_sec;
	return mark;
}

int64_t DayPeriod(time_t ts, int64_t offset_sec)
{
	return PeriodOf(ts, offset_sec, DayOf);
}

int64_t WeekPeriod(time_t ts, int64_t offset_sec)
{
	return PeriodOf(ts, offset_sec, WeekOf);
}

int64_t WeekdayPeriod(time_t ts, int weekday, int64_t offset_sec)
{
	int64_t slot = 0;
	if (!WeekdaySlot(ts, weekday, offset_sec, &slot))
	{
		return 0;
	}
	return slot;
}

int64_t MonthPeriod(time_t ts, int64_t offset_sec)
{
	return PeriodOf(ts, offset_sec, MonthOf);
}

int64_t YearPeriod(time_t ts, int64_t offset_sec)
{
	return PeriodOf(ts, offset_sec, YearOf);
}

bool SameDay(time_t a, time_t b, int64_t offset_sec)
{
	return SameOf(a, b, offset_sec, DayOf);
}

bool SameWeek(time_t a, time_t b, int64_t offset_sec)
{
	return SameOf(a, b, offset_sec, WeekOf);
}

bool SameWeekday(time_t a, time_t b, int weekday, int64_t offset_sec)
{
	int64_t left = 0;
	int64_t right = 0;
	if (!WeekdaySlot(a, weekday, offset_sec, &left) || !WeekdaySlot(b, weekday, offset_sec, &right))
	{
		return false;
	}
	return left == right;
}

bool SameMonth(time_t a, time_t b, int64_t offset_sec)
{
	return SameOf(a, b, offset_sec, MonthOf);
}

bool SameYear(time_t a, time_t b, int64_t offset_sec)
{
	return SameOf(a, b, offset_sec, YearOf);
}

bool CrossDay(time_t old_ts, time_t now_ts, int64_t offset_sec)
{
	return CrossOf(old_ts, now_ts, offset_sec, DayOf);
}

bool CrossWeek(time_t old_ts, time_t now_ts, int64_t offset_sec)
{
	return CrossOf(old_ts, now_ts, offset_sec, WeekOf);
}

bool CrossWeekday(time_t old_ts, time_t now_ts, int weekday, int64_t offset_sec)
{
	int64_t old_slot = 0;
	int64_t now_slot = 0;
	if (!WeekdaySlot(old_ts, weekday, offset_sec, &old_slot) || !WeekdaySlot(now_ts, weekday, offset_sec, &now_slot))
	{
		return false;
	}
	return now_slot > old_slot;
}

bool CrossMonth(time_t old_ts, time_t now_ts, int64_t offset_sec)
{
	return CrossOf(old_ts, now_ts, offset_sec, MonthOf);
}

bool CrossYear(time_t old_ts, time_t now_ts, int64_t offset_sec)
{
	return CrossOf(old_ts, now_ts, offset_sec, YearOf);
}

bool CrossDay(const TimeMark& saved, time_t now_ts)
{
	return CrossDay(saved.ts, now_ts, saved.offset_sec);
}

bool CrossWeek(const TimeMark& saved, time_t now_ts)
{
	return CrossWeek(saved.ts, now_ts, saved.offset_sec);
}

bool CrossWeekday(const TimeMark& saved, time_t now_ts, int weekday)
{
	return CrossWeekday(saved.ts, now_ts, weekday, saved.offset_sec);
}

bool CrossMonth(const TimeMark& saved, time_t now_ts)
{
	return CrossMonth(saved.ts, now_ts, saved.offset_sec);
}

bool CrossYear(const TimeMark& saved, time_t now_ts)
{
	return CrossYear(saved.ts, now_ts, saved.offset_sec);
}
}}
