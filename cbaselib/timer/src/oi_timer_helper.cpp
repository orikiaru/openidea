#include "oi_timer_helper.h"
#include "oi_timer_manager.h"
#include "oi_duration.h"
#include <chrono>

namespace OI
{
namespace Timer
{
namespace
{
int64_t FloorDiv(int64_t iFirst, int64_t iSecond)
{
	int64_t iQ = iFirst / iSecond;
	int64_t iR = iFirst % iSecond;
	if (iR != 0 && ((iFirst < 0) != (iSecond < 0)))
	{
		--iQ;
	}
	return iQ;
}

int64_t CivilDays(int iYear, unsigned iMonth, unsigned iDay)
{
	iYear -= iMonth <= 2;
	const int iEra = (iYear >= 0 ? iYear : iYear - 399) / 400;
	const unsigned iYoe = (unsigned)(iYear - iEra * 400);
	const unsigned iDoy = (153 * (iMonth + (iMonth > 2 ? -3 : 9)) + 2) / 5 + iDay - 1;
	const unsigned iDoe = iYoe * 365 + iYoe / 4 - iYoe / 100 + iDoy;
	return (int64_t)iEra * 146097 + (int64_t)iDoe - 719468;
}

bool ToLocal(time_t iTimestamp, int64_t iOffsetSecondsLocal, tm* pOutput)
{
	int64_t iShifted = (int64_t)iTimestamp + iOffsetSecondsLocal;
	time_t iClock = (time_t)iShifted;
	return localtime_r(&iClock, pOutput) != NULL;
}

int64_t DayOf(const tm& stLocal)
{
	return CivilDays(stLocal.tm_year + 1900, (unsigned)stLocal.tm_mon + 1, (unsigned)stLocal.tm_mday);
}

int64_t WeekStarting(const tm& stLocal, int iWeekday)
{
	// 1970-01-01 是周四。weekday 用 tm_wday 的数，周一就是现在的 CrossWeek。
	return FloorDiv(DayOf(stLocal) + (4 - iWeekday), 7);
}

int64_t WeekOf(const tm& stLocal)
{
	return WeekStarting(stLocal, WeekdayMonday);
}

bool KnownWeekday(int iWeekday)
{
	return iWeekday >= WeekdaySunday && iWeekday <= WeekdaySaturday;
}

bool WeekdaySlot(time_t iTimestamp, int iWeekday, int64_t iOffsetSecondsLocal, int64_t* pOutput)
{
	tm stLocal;
	if (!KnownWeekday(iWeekday) || !ToLocal(iTimestamp, iOffsetSecondsLocal, &stLocal))
	{
		return false;
	}
	*pOutput = WeekStarting(stLocal, iWeekday);
	return true;
}

int64_t MonthOf(const tm& stLocal)
{
	return (int64_t)stLocal.tm_year * 12 + stLocal.tm_mon;
}

int64_t YearOf(const tm& stLocal)
{
	return stLocal.tm_year;
}

typedef int64_t (*SlotOf)(const tm&);

bool Slot(time_t iTimestamp, int64_t iOffsetSecondsLocal, SlotOf fnSlotOf, int64_t* pOutput)
{
	tm stLocal;
	if (!ToLocal(iTimestamp, iOffsetSecondsLocal, &stLocal))
	{
		return false;
	}
	*pOutput = fnSlotOf(stLocal);
	return true;
}

int64_t PeriodOf(time_t iTimestamp, int64_t iOffsetSecondsLocal, SlotOf fnSlotOf)
{
	int64_t iSlot = 0;
	if (!Slot(iTimestamp, iOffsetSecondsLocal, fnSlotOf, &iSlot))
	{
		return 0;
	}
	return iSlot;
}

bool SameOf(time_t iFirst, time_t iSecond, int64_t iOffsetSecondsLocal, SlotOf fnSlotOf)
{
	int64_t iLeft = 0;
	int64_t iRight = 0;
	if (!Slot(iFirst, iOffsetSecondsLocal, fnSlotOf, &iLeft) || !Slot(iSecond, iOffsetSecondsLocal, fnSlotOf, &iRight))
	{
		return false;
	}
	return iLeft == iRight;
}

bool CrossOf(time_t iOldTimestamp, time_t iNowTimestamp, int64_t iOffsetSecondsLocal, SlotOf fnSlotOf)
{
	int64_t iOldSlot = 0;
	int64_t iNowSlot = 0;
	if (!Slot(iOldTimestamp, iOffsetSecondsLocal, fnSlotOf, &iOldSlot) || !Slot(iNowTimestamp, iOffsetSecondsLocal, fnSlotOf, &iNowSlot))
	{
		return false;
	}
	return iNowSlot > iOldSlot;
}
}

int64_t SteadyMilliseconds()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now().time_since_epoch())
	    .count();
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

time_t Shift(time_t iTimestamp, int64_t iOffsetSecondsLocal)
{
	return (time_t)((int64_t)iTimestamp + iOffsetSecondsLocal);
}

TimeMark Capture(int64_t iOffsetSecondsLocal)
{
	TimeMark stMark;
	stMark.iTimestamp = GetTime();
	stMark.iOffsetSeconds = iOffsetSecondsLocal;
	return stMark;
}

int64_t DayPeriod(time_t iTimestamp, int64_t iOffsetSecondsLocal)
{
	return PeriodOf(iTimestamp, iOffsetSecondsLocal, DayOf);
}

int64_t WeekPeriod(time_t iTimestamp, int64_t iOffsetSecondsLocal)
{
	return PeriodOf(iTimestamp, iOffsetSecondsLocal, WeekOf);
}

int64_t WeekdayPeriod(time_t iTimestamp, int iWeekday, int64_t iOffsetSecondsLocal)
{
	int64_t iSlot = 0;
	if (!WeekdaySlot(iTimestamp, iWeekday, iOffsetSecondsLocal, &iSlot))
	{
		return 0;
	}
	return iSlot;
}

int64_t MonthPeriod(time_t iTimestamp, int64_t iOffsetSecondsLocal)
{
	return PeriodOf(iTimestamp, iOffsetSecondsLocal, MonthOf);
}

int64_t YearPeriod(time_t iTimestamp, int64_t iOffsetSecondsLocal)
{
	return PeriodOf(iTimestamp, iOffsetSecondsLocal, YearOf);
}

bool SameDay(time_t iFirst, time_t iSecond, int64_t iOffsetSecondsLocal)
{
	return SameOf(iFirst, iSecond, iOffsetSecondsLocal, DayOf);
}

bool SameWeek(time_t iFirst, time_t iSecond, int64_t iOffsetSecondsLocal)
{
	return SameOf(iFirst, iSecond, iOffsetSecondsLocal, WeekOf);
}

bool SameWeekday(time_t iFirst, time_t iSecond, int iWeekday, int64_t iOffsetSecondsLocal)
{
	int64_t iLeft = 0;
	int64_t iRight = 0;
	if (!WeekdaySlot(iFirst, iWeekday, iOffsetSecondsLocal, &iLeft) || !WeekdaySlot(iSecond, iWeekday, iOffsetSecondsLocal, &iRight))
	{
		return false;
	}
	return iLeft == iRight;
}

bool SameMonth(time_t iFirst, time_t iSecond, int64_t iOffsetSecondsLocal)
{
	return SameOf(iFirst, iSecond, iOffsetSecondsLocal, MonthOf);
}

bool SameYear(time_t iFirst, time_t iSecond, int64_t iOffsetSecondsLocal)
{
	return SameOf(iFirst, iSecond, iOffsetSecondsLocal, YearOf);
}

bool CrossDay(time_t iOldTimestamp, time_t iNowTimestamp, int64_t iOffsetSecondsLocal)
{
	return CrossOf(iOldTimestamp, iNowTimestamp, iOffsetSecondsLocal, DayOf);
}

bool CrossWeek(time_t iOldTimestamp, time_t iNowTimestamp, int64_t iOffsetSecondsLocal)
{
	return CrossOf(iOldTimestamp, iNowTimestamp, iOffsetSecondsLocal, WeekOf);
}

bool CrossWeekday(time_t iOldTimestamp, time_t iNowTimestamp, int iWeekday, int64_t iOffsetSecondsLocal)
{
	int64_t iOldSlot = 0;
	int64_t iNowSlot = 0;
	if (!WeekdaySlot(iOldTimestamp, iWeekday, iOffsetSecondsLocal, &iOldSlot) || !WeekdaySlot(iNowTimestamp, iWeekday, iOffsetSecondsLocal, &iNowSlot))
	{
		return false;
	}
	return iNowSlot > iOldSlot;
}

bool CrossMonth(time_t iOldTimestamp, time_t iNowTimestamp, int64_t iOffsetSecondsLocal)
{
	return CrossOf(iOldTimestamp, iNowTimestamp, iOffsetSecondsLocal, MonthOf);
}

bool CrossYear(time_t iOldTimestamp, time_t iNowTimestamp, int64_t iOffsetSecondsLocal)
{
	return CrossOf(iOldTimestamp, iNowTimestamp, iOffsetSecondsLocal, YearOf);
}

bool CrossDay(const TimeMark& stSaved, time_t iNowTimestamp)
{
	return CrossDay(stSaved.iTimestamp, iNowTimestamp, stSaved.iOffsetSeconds);
}

bool CrossWeek(const TimeMark& stSaved, time_t iNowTimestamp)
{
	return CrossWeek(stSaved.iTimestamp, iNowTimestamp, stSaved.iOffsetSeconds);
}

bool CrossWeekday(const TimeMark& stSaved, time_t iNowTimestamp, int iWeekday)
{
	return CrossWeekday(stSaved.iTimestamp, iNowTimestamp, iWeekday, stSaved.iOffsetSeconds);
}

bool CrossMonth(const TimeMark& stSaved, time_t iNowTimestamp)
{
	return CrossMonth(stSaved.iTimestamp, iNowTimestamp, stSaved.iOffsetSeconds);
}

bool CrossYear(const TimeMark& stSaved, time_t iNowTimestamp)
{
	return CrossYear(stSaved.iTimestamp, iNowTimestamp, stSaved.iOffsetSeconds);
}
}
}
