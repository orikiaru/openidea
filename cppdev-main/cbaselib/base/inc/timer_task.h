#pragma once
#include "task.h"
#include "base_timer.h"
#include "threadpool.h"
#include <utility>

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

// 到点后把 lambda 拷贝进线程池。lambda 是 ()，要用的数据自己捕获。
template<typename F>
class FnTimer : public BaseTimer
{
	F _fn;
public:
	explicit FnTimer(F fn) : _fn(std::move(fn)) {}
	bool OnTimer()
	{
		F copied = _fn;
		ThreadPool::GetInstance()->AddTask(std::move(copied));
		delete this;
		return END_TIMER;
	}
};