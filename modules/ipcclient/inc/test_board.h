#pragma once
#include <atomic>

// 每个测试向这里登记。全部做完才停进程。
class TestBoard
{
	std::atomic<int> _left;
	std::atomic<int> _code;
public:
	TestBoard();
	void Arm();
	void Done(int code);
};

TestBoard& GetTestBoard();
