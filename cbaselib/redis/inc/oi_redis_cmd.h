#pragma once
#include "oi_redis_value.h"
#include "oi_duration.h"
#include <initializer_list>
namespace OI
{
namespace Redis
{
// 标准命令均只接收一个 key；参数在返回 Task 前拷入命令，输出对象须活到完成。
// 原生命令显式指定全部参数；类型化输出与标准命令共用解码和错误码。
template <typename T>
    requires(!std::is_same_v<T, RedisResult>)
[[nodiscard]] inline coro::TaskResult<int32_t> CallRedis(std::vector<std::string> vArgumentsParam, T& stOutputParam)
{
	return Detail::Read(Detail::Arguments(std::move(vArgumentsParam)), stOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t CallRedisSync(std::vector<std::string> vArgumentsParam, T& stOutputParam)
{
	return Detail::ReadSync(Detail::Arguments(std::move(vArgumentsParam)), stOutputParam);
}

// 字面量命令先以轻量视图接收，返回前全部复制；避免协程表达式创建临时 string 数组。
template <typename T>
    requires(!std::is_same_v<T, RedisResult>)
[[nodiscard]] inline coro::TaskResult<int32_t> CallRedis(std::initializer_list<std::string_view> vArgumentsParam, T& stOutputParam)
{
	std::vector<std::string> vArguments;
	vArguments.reserve(vArgumentsParam.size());
	for (const auto& sArgument : vArgumentsParam)
	{
		vArguments.emplace_back(sArgument);
	}
	return CallRedis(std::move(vArguments), stOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t CallRedisSync(std::initializer_list<std::string_view> vArgumentsParam, T& stOutputParam)
{
	std::vector<std::string> vArguments;
	vArguments.reserve(vArgumentsParam.size());
	for (const auto& sArgument : vArgumentsParam)
	{
		vArguments.emplace_back(sArgument);
	}
	return CallRedisSync(std::move(vArguments), stOutputParam);
}

}
}

#include "oi_redis_string.h"
#include "oi_redis_hash.h"
#include "oi_redis_list.h"
#include "oi_redis_set.h"
#include "oi_redis_zset.h"
