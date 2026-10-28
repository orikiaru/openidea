#pragma once
#include "singleton.h"
#include <atomic>

// 每个测试向这里登记。全部做完才停进程。
class TestBoard
{
	DECLARE_SINGLETON(TestBoard)
	TestBoard();
	std::atomic<int> _left;
	std::atomic<int> _code;
public:
	void Arm();
	void Done(int code);
};
