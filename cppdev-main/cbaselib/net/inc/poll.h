#ifndef __multi_t_t___
#define __multi_t_t___
#define MAX_MULTI_FD  1024
#include <sys/epoll.h>
#include <list>
//#define MAX_WAIT_TIME 1000
int multi_add(int epoll_fd, int fd, int new_event, void* privdata);
int multi_mod(int epoll_fd, int fd, int new_event, void* privdata);
int multi_del(int epoll_fd, int fd);
void *do_multi(void *arg);
void recv_response(void* privdata);
void send_request(void* privdata);
void try_close(void* privdata);

class Task;
class Channel;
class PollInnerTask;



enum IO_EVENT_MASK
{
	IO_EV_READ 	= EPOLLIN,
	IO_EV_WRITE	= EPOLLOUT,
	IO_EV_ERR	= EPOLLERR | EPOLLHUP,
	IO_EV_REG	= IO_EV_READ | IO_EV_WRITE,
};

typedef std::list<PollInnerTask*> PollTaskList;

class Poll
{
	int 			_epoll_fd;
	pthread_t 		_epoll_thr_id;
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
	Poll():_epoll_fd(0),_epoll_thr_id(0){}
	~Poll();

	bool Init();

	static Poll inst;
	static Poll* GetInstance()
	{
		return &inst;
	}

	//use from session lockerguard
	void IOEventRegister(Channel* pChannel, bool allow_read = true, bool allow_write = false);
	void IOEventMod(Channel* pChannel, int new_mask);
	void IOEventUnRegister(Channel* pChannel);

	static void* StartThreadPollWait(void* arg);
	void PollWait(int wait_ms = 1000);

	//增加poll之前执行或poll之后执行的任务,目前只支持增加一次性或重复性的任务
	void AddPollTask(Task* pTask, PollTaskPosEnum pos, bool once = false);

};


#endif

