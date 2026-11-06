#pragma once
#include <cstdint>

namespace OI
{
namespace Timer
{

// 时长参数只有 MS。其他单位在构造时换成毫秒。
struct Milliseconds
{
	// 毫秒数。构造 MS 时原样使用。
	int64_t iMilliseconds;

	constexpr explicit Milliseconds(int64_t iMilliseconds)
	    : iMilliseconds(iMilliseconds)
	{
	}
};

struct Seconds
{
	// 秒数。构造 MS 时乘 1000。
	int64_t iSeconds;

	constexpr explicit Seconds(int64_t iSeconds)
	    : iSeconds(iSeconds)
	{
	}
};

struct Minutes
{
	// 分钟数。构造 MS 时乘 60 秒。
	int64_t iMinutes;

	constexpr explicit Minutes(int64_t iMinutes)
	    : iMinutes(iMinutes)
	{
	}
};

struct Hours
{
	// 小时数。构造 MS 时乘 3600 秒。
	int64_t iHours;

	constexpr explicit Hours(int64_t iHours)
	    : iHours(iHours)
	{
	}
};

struct Days
{
	// 天数。构造 MS 时乘 24 小时。
	int64_t iDays;

	constexpr explicit Days(int64_t iDays)
	    : iDays(iDays)
	{
	}
};

struct MS
{
	// 统一后的毫秒数。定时器和 yield 都用这个。
	int64_t iMilliseconds;

	constexpr MS()
	    : iMilliseconds(0)
	{
	}

	constexpr explicit MS(int64_t iMilliseconds)
	    : iMilliseconds(iMilliseconds)
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
