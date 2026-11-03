#include "oi_timer_helper.h"
#include "oi_timer_manager.h"

namespace OI
{
namespace Timer
{
namespace
{
int64_t FloorDiv(int64_t iFirstParam, int64_t iSecondParam)
{
	int64_t iQ = iFirstParam / iSecondParam;
	int64_t iR = iFirstParam % iSecondParam;
	if (iR != 0 && ((iFirstParam < 0) != (iSecondParam < 0)))
	{
		--iQ;
	}
	return iQ;
}

int64_t CivilDays(int iYearParam, unsigned iMonthParam, unsigned iDayParam)
{
	iYearParam -= iMonthParam <= 2;
	const int iEra = (iYearParam >= 0 ? iYearParam : iYearParam - 399) / 400;
	const unsigned iYoe = (unsigned)(iYearParam - iEra * 400);
	const unsigned iDoy = (153 * (iMonthParam + (iMonthParam > 2 ? -3 : 9)) + 2) / 5 + iDayParam - 1;
	const unsigned iDoe = iYoe * 365 + iYoe / 4 - iYoe / 100 + iDoy;
	return (int64_t)iEra * 146097 + (int64_t)iDoe - 719468;
}

bool ToLocal(time_t iTimestampParam, int64_t iOffsetSecondsParam, tm* pOutputParam)
{
	int64_t iShifted = (int64_t)iTimestampParam + iOffsetSecondsParam;
	time_t iClock = (time_t)iShifted;
	return localtime_r(&iClock, pOutputParam) != NULL;
}

int64_t DayOf(const tm& stLocalParam)
{
	return CivilDays(stLocalParam.tm_year + 1900, (unsigned)stLocalParam.tm_mon + 1, (unsigned)stLocalParam.tm_mday);
}

int64_t WeekStarting(const tm& stLocalParam, int iWeekdayParam)
{
	// 1970-01-01 是周四。weekday 用 tm_wday 的数，周一就是现在的 CrossWeek。
	return FloorDiv(DayOf(stLocalParam) + (4 - iWeekdayParam), 7);
}

int64_t WeekOf(const tm& stLocalParam)
{
	return WeekStarting(stLocalParam, WeekdayMonday);
}

bool KnownWeekday(int iWeekdayParam)
{
	return iWeekdayParam >= WeekdaySunday && iWeekdayParam <= WeekdaySaturday;
}

bool WeekdaySlot(time_t iTimestampParam, int iWeekdayParam, int64_t iOffsetSecondsParam, int64_t* pOutputParam)
{
	tm stLocal;
	if (!KnownWeekday(iWeekdayParam) || !ToLocal(iTimestampParam, iOffsetSecondsParam, &stLocal))
	{
		return false;
	}
	*pOutputParam = WeekStarting(stLocal, iWeekdayParam);
	return true;
}

int64_t MonthOf(const tm& stLocalParam)
{
	return (int64_t)stLocalParam.tm_year * 12 + stLocalParam.tm_mon;
}

int64_t YearOf(const tm& stLocalParam)
{
	return stLocalParam.tm_year;
}

typedef int64_t (*SlotOf)(const tm&);

bool Slot(time_t iTimestampParam, int64_t iOffsetSecondsParam, SlotOf fnSlotOfParam, int64_t* pOutputParam)
{
	tm stLocal;
	if (!ToLocal(iTimestampParam, iOffsetSecondsParam, &stLocal))
	{
		return false;
	}
	*pOutputParam = fnSlotOfParam(stLocal);
	return true;
}

int64_t PeriodOf(time_t iTimestampParam, int64_t iOffsetSecondsParam, SlotOf fnSlotOfParam)
{
	int64_t iSlot = 0;
	if (!Slot(iTimestampParam, iOffsetSecondsParam, fnSlotOfParam, &iSlot))
	{
		return 0;
	}
	return iSlot;
}

bool SameOf(time_t iFirstParam, time_t iSecondParam, int64_t iOffsetSecondsParam, SlotOf fnSlotOfParam)
{
	int64_t iLeft = 0;
	int64_t iRight = 0;
	if (!Slot(iFirstParam, iOffsetSecondsParam, fnSlotOfParam, &iLeft) || !Slot(iSecondParam, iOffsetSecondsParam, fnSlotOfParam, &iRight))
	{
		return false;
	}
	return iLeft == iRight;
}

bool CrossOf(time_t iOldTimestampParam, time_t iNowTimestampParam, int64_t iOffsetSecondsParam, SlotOf fnSlotOfParam)
{
	int64_t iOldSlot = 0;
	int64_t iNowSlot = 0;
	if (!Slot(iOldTimestampParam, iOffsetSecondsParam, fnSlotOfParam, &iOldSlot) || !Slot(iNowTimestampParam, iOffsetSecondsParam, fnSlotOfParam, &iNowSlot))
	{
		return false;
	}
	return iNowSlot > iOldSlot;
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

time_t Shift(time_t iTimestampParam, int64_t iOffsetSecondsParam)
{
	return (time_t)((int64_t)iTimestampParam + iOffsetSecondsParam);
}

TimeMark Capture(int64_t iOffsetSecondsParam)
{
	TimeMark stMark;
	stMark.iTimestamp = GetTime();
	stMark.iOffsetSeconds = iOffsetSecondsParam;
	return stMark;
}

int64_t DayPeriod(time_t iTimestampParam, int64_t iOffsetSecondsParam)
{
	return PeriodOf(iTimestampParam, iOffsetSecondsParam, DayOf);
}

int64_t WeekPeriod(time_t iTimestampParam, int64_t iOffsetSecondsParam)
{
	return PeriodOf(iTimestampParam, iOffsetSecondsParam, WeekOf);
}

int64_t WeekdayPeriod(time_t iTimestampParam, int iWeekdayParam, int64_t iOffsetSecondsParam)
{
	int64_t iSlot = 0;
	if (!WeekdaySlot(iTimestampParam, iWeekdayParam, iOffsetSecondsParam, &iSlot))
	{
		return 0;
	}
	return iSlot;
}

int64_t MonthPeriod(time_t iTimestampParam, int64_t iOffsetSecondsParam)
{
	return PeriodOf(iTimestampParam, iOffsetSecondsParam, MonthOf);
}

int64_t YearPeriod(time_t iTimestampParam, int64_t iOffsetSecondsParam)
{
	return PeriodOf(iTimestampParam, iOffsetSecondsParam, YearOf);
}

bool SameDay(time_t iFirstParam, time_t iSecondParam, int64_t iOffsetSecondsParam)
{
	return SameOf(iFirstParam, iSecondParam, iOffsetSecondsParam, DayOf);
}

bool SameWeek(time_t iFirstParam, time_t iSecondParam, int64_t iOffsetSecondsParam)
{
	return SameOf(iFirstParam, iSecondParam, iOffsetSecondsParam, WeekOf);
}

bool SameWeekday(time_t iFirstParam, time_t iSecondParam, int iWeekdayParam, int64_t iOffsetSecondsParam)
{
	int64_t iLeft = 0;
	int64_t iRight = 0;
	if (!WeekdaySlot(iFirstParam, iWeekdayParam, iOffsetSecondsParam, &iLeft) || !WeekdaySlot(iSecondParam, iWeekdayParam, iOffsetSecondsParam, &iRight))
	{
		return false;
	}
	return iLeft == iRight;
}

bool SameMonth(time_t iFirstParam, time_t iSecondParam, int64_t iOffsetSecondsParam)
{
	return SameOf(iFirstParam, iSecondParam, iOffsetSecondsParam, MonthOf);
}

bool SameYear(time_t iFirstParam, time_t iSecondParam, int64_t iOffsetSecondsParam)
{
	return SameOf(iFirstParam, iSecondParam, iOffsetSecondsParam, YearOf);
}

bool CrossDay(time_t iOldTimestampParam, time_t iNowTimestampParam, int64_t iOffsetSecondsParam)
{
	return CrossOf(iOldTimestampParam, iNowTimestampParam, iOffsetSecondsParam, DayOf);
}

bool CrossWeek(time_t iOldTimestampParam, time_t iNowTimestampParam, int64_t iOffsetSecondsParam)
{
	return CrossOf(iOldTimestampParam, iNowTimestampParam, iOffsetSecondsParam, WeekOf);
}

bool CrossWeekday(time_t iOldTimestampParam, time_t iNowTimestampParam, int iWeekdayParam, int64_t iOffsetSecondsParam)
{
	int64_t iOldSlot = 0;
	int64_t iNowSlot = 0;
	if (!WeekdaySlot(iOldTimestampParam, iWeekdayParam, iOffsetSecondsParam, &iOldSlot) || !WeekdaySlot(iNowTimestampParam, iWeekdayParam, iOffsetSecondsParam, &iNowSlot))
	{
		return false;
	}
	return iNowSlot > iOldSlot;
}

bool CrossMonth(time_t iOldTimestampParam, time_t iNowTimestampParam, int64_t iOffsetSecondsParam)
{
	return CrossOf(iOldTimestampParam, iNowTimestampParam, iOffsetSecondsParam, MonthOf);
}

bool CrossYear(time_t iOldTimestampParam, time_t iNowTimestampParam, int64_t iOffsetSecondsParam)
{
	return CrossOf(iOldTimestampParam, iNowTimestampParam, iOffsetSecondsParam, YearOf);
}

bool CrossDay(const TimeMark& stSavedParam, time_t iNowTimestampParam)
{
	return CrossDay(stSavedParam.iTimestamp, iNowTimestampParam, stSavedParam.iOffsetSeconds);
}

bool CrossWeek(const TimeMark& stSavedParam, time_t iNowTimestampParam)
{
	return CrossWeek(stSavedParam.iTimestamp, iNowTimestampParam, stSavedParam.iOffsetSeconds);
}

bool CrossWeekday(const TimeMark& stSavedParam, time_t iNowTimestampParam, int iWeekdayParam)
{
	return CrossWeekday(stSavedParam.iTimestamp, iNowTimestampParam, iWeekdayParam, stSavedParam.iOffsetSeconds);
}

bool CrossMonth(const TimeMark& stSavedParam, time_t iNowTimestampParam)
{
	return CrossMonth(stSavedParam.iTimestamp, iNowTimestampParam, stSavedParam.iOffsetSeconds);
}

bool CrossYear(const TimeMark& stSavedParam, time_t iNowTimestampParam)
{
	return CrossYear(stSavedParam.iTimestamp, iNowTimestampParam, stSavedParam.iOffsetSeconds);
}
}
}
