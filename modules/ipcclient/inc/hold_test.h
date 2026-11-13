#pragma once
#include "oi_config.h"
#include "oi_duration.h"
#include "oi_redis.h"
#include <cstring>
#include <ctime>
#include <string>

// 各回归 mgr 共用的计时和端口。端口跟着配置走，不写死。
struct HoldTest
{
	static long long CpuMs(std::clock_t iBegin)
	{
		double dMilliseconds = 1000.0 * (double)(std::clock() - iBegin) / (double)CLOCKS_PER_SEC;
		if (dMilliseconds < 0)
		{
			dMilliseconds = 0;
		}
		return (long long)dMilliseconds;
	}

	static long long WallMs(int64_t iBeginMilliseconds)
	{
		return ::OI::Timer::SteadyMilliseconds() - iBeginMilliseconds;
	}

	static bool Pong(const ::OI::Redis::RedisResult& stResult)
	{
		return !stResult.bTimeout && stResult.pReply && stResult.pReply->type == REDIS_REPLY_STATUS && stResult.pReply->str && std::strcmp(stResult.pReply->str, "PONG") == 0;
	}

	static std::string IpcServerPort()
	{
		std::string sPort = "17981";
		TryGetConfig("IpcClient.port", sPort);
		return sPort;
	}
};
