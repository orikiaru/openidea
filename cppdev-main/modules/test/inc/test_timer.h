#pragma once
#include "base_timer.h"
#include <string>
class TestTimer : public BaseTimer
{
	int _offset;
	timer_tick_t _before_tick;
public:
	TestTimer(int offset);
	virtual bool OnTimer();
	std::string GenLog();
	static void DoPerformanceTest();
};