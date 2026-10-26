#ifndef __multi_t_t___
#define __multi_t_t___
#define MAX_MULTI_FD  1024
#include <list>
#include <thread>

class Task;
class Channel;
class PollInnerTask;

enum IO_EVENT_MASK
{
	IO_EV_READ 	= 0x001,
	IO_EV_WRITE	= 0x004,
	IO_EV_ERR	= 0x008,
	IO_EV_REG	= IO_EV_READ | IO_EV_WRITE,
};

typedef std::list<PollInnerTask*> PollTaskList;

class Poll
{
	std::thread::id	_loop_thread;
	PollTaskList	_before_tasks;
	PollTaskList	_after_tasks;

	void BeforePoll();
	void AfterPoll();
public:
	enum PollTaskPosEnum
	{
		POLL_BEFORE_TASK = true,
		POLL_AFTER_TASK = false,
	};
	Poll(){}
	~Poll();

	bool Init();

	static Poll inst;
	static Poll* GetInstance()
	{
		return &inst;
	}

	void IOEventRegister(Channel* pChannel, bool allow_read = true, bool allow_write = false);
	void IOEventMod(Channel* pChannel, int new_mask);
	void IOEventUnRegister(Channel* pChannel);

	static void* StartThreadPollWait(void* arg);
	void PollWait(int wait_ms = 1000);
	// libuv 重复定时器。循环起来之前调用，到点在 poll 线程执行 fn。
	void ArmTick(unsigned repeat_ms, void (*fn)());

	void AddPollTask(Task* pTask, PollTaskPosEnum pos, bool once = false);
};

#endif
