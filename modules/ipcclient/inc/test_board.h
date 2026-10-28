#pragma once
#include "oi_singleton.h"
#include <atomic>
#include <cstdint>

// 每个测试向这里登记。全部做完才停进程。
class TestBoard
{
	DECLARE_SINGLETON(TestBoard)
	TestBoard();
	std::atomic<int32_t> _left;
	std::atomic<int32_t> _code;
public:
	void Arm();
	void Done(int code);
};
