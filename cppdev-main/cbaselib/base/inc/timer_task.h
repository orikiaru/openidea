#pragma once
#include "task.h"
#include "base_timer.h"

//注TimerTask必须是堆内存,自销毁,重写Handle即可
class TimerTask : public BaseTimer, public Task
{
public:
	TimerTask() :Task(PRIO_NORMAL) {}
	virtual void Exec()
	{
		Handle();
		delete this;
	}
	virtual bool OnTimer();

	virtual void Handle() = 0;
};