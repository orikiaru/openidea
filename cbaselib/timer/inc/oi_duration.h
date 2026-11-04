#pragma once
#include <cstdint>

namespace OI
{
namespace Timer
{

// 时长参数只有 MS。其他单位在构造时换成毫秒。
struct Milliseconds
{
	int64_t iMilliseconds;

	constexpr explicit Milliseconds(int64_t iMillisecondsParam)
	    : iMilliseconds(iMillisecondsParam)
	{
	}
};

struct Seconds
{
	int64_t iSeconds;

	constexpr explicit Seconds(int64_t iSecondsParam)
	    : iSeconds(iSecondsParam)
	{
	}
};

struct Minutes
{
	int64_t iMinutes;

	constexpr explicit Minutes(int64_t iMinutesParam)
	    : iMinutes(iMinutesParam)
	{
	}
};

struct Hours
{
	int64_t iHours;

	constexpr explicit Hours(int64_t iHoursParam)
	    : iHours(iHoursParam)
	{
	}
};

struct Days
{
	int64_t iDays;

	constexpr explicit Days(int64_t iDaysParam)
	    : iDays(iDaysParam)
	{
	}
};

struct MS
{
	int64_t iMilliseconds;

	constexpr MS()
	    : iMilliseconds(0)
	{
	}

	constexpr explicit MS(int64_t iMillisecondsParam)
	    : iMilliseconds(iMillisecondsParam)
	{
	}

	constexpr MS(Milliseconds stParam)
	    : iMilliseconds(stParam.iMilliseconds)
	{
	}

	constexpr MS(Seconds stParam)
	    : iMilliseconds(stParam.iSeconds * 1000)
	{
	}

	constexpr MS(Minutes stParam)
	    : iMilliseconds(stParam.iMinutes * 60 * 1000)
	{
	}

	constexpr MS(Hours stParam)
	    : iMilliseconds(stParam.iHours * 60 * 60 * 1000)
	{
	}

	constexpr MS(Days stParam)
	    : iMilliseconds(stParam.iDays * 24 * 60 * 60 * 1000)
	{
	}
};

// 单调毫秒，不受系统校时影响。
int64_t SteadyMilliseconds();

}
}
