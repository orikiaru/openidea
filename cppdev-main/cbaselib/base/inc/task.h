#pragma once

#include <list>
enum PRIO_ENUM
{
	PRIO_NORMAL,	//正常排队,无序,复数线程池取到就处理,先取可能后处理,多线程处理时序无法保证
	PRIO_URGENT,	//加急,加急队列有自己的线程泳道,加急协议下行处理将进行插队处理
	PRIO_QUEUE,		//有序队列,此队列只有一个线程,多线程下也可能需要一些任务线程执行,可将复数任务线性push到此队列,或者封装成一个任务丢到其他队列
	PRIO_NUM,
};

class Task
{
public:
	PRIO_ENUM _prio;
	Task(PRIO_ENUM prio = PRIO_NORMAL) :_prio(prio) {}
	virtual ~Task() {}
	virtual void Exec() = 0;
};

typedef std::list<Task*> TASK_LIST;
