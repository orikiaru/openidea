#pragma once
#include <chrono>
#include <cstdint>

namespace OI { namespace Timer {

// 应用层统一构造时长，不依赖字面量后缀，也不混用裸整数的单位。
constexpr std::chrono::milliseconds Milliseconds(int64_t value) { return std::chrono::milliseconds(value); }
constexpr std::chrono::seconds Seconds(int64_t value) { return std::chrono::seconds(value); }
constexpr std::chrono::minutes Minutes(int64_t value) { return std::chrono::minutes(value); }
constexpr std::chrono::hours Hours(int64_t value) { return std::chrono::hours(value); }
constexpr std::chrono::duration<int64_t, std::ratio<86400>> Days(int64_t value)
{
	return std::chrono::duration<int64_t, std::ratio<86400>>(value);
}

}}
