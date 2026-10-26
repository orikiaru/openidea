#include "timer_task.h"
#include "threadpool.h"

bool TimerTask::OnTimer()
{
	ThreadPool::GetInstance()->AddTask(this);
	return END_TIMER;
}