#include <stdio.h>
#include <sys/epoll.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <pthread.h>
#include "poll.h"
#include "channel.h"
#include "task.h"
#include "threadpool.h"
#include "header.h"

class PollInnerTask
{
	Task* _p_task;
	bool  _once;
public:
	PollInnerTask(Task* pTask, bool Once) :_p_task(pTask), _once(Once) {}
	bool Exec()
	{
		assert(_p_task);
		_p_task->Exec();
		return _once;
	}
	~PollInnerTask()
	{
		SAFE_DELETE(_p_task);
	}
};

#ifndef _SINGLE_THREAD_
int multi_thread_create(void){
	int epoll_fd = epoll_create1(0);
	if(epoll_fd==-1){
		perror("epoll");
		exit(-1);
	}
	pthread_t tid = 0;
	int* pfd = (int*)malloc(sizeof(int));
	*pfd = epoll_fd;
	pthread_create(&tid,0,do_multi,pfd);
	pthread_detach(tid);
	return epoll_fd;
}
#endif

void *do_multi(void *arg){
	signal(SIGPIPE, SIG_IGN);
	int epoll_fd = *(int*)arg;
	struct epoll_event e_event[MAX_MULTI_FD];
	int get_num 	= 0;
	int READ_MASK 	= EPOLLIN | EPOLLERR | EPOLLHUP;
	int WRITE_MASK 	= EPOLLOUT;
	for(;;){
		get_num = epoll_wait(epoll_fd,e_event,MAX_MULTI_FD,-1);
		if(get_num==-1){
			if(errno == EINTR ||errno == EAGAIN){
				continue;
			}
			else{
				perror("epoll");
				exit(0);
			}
		}
		for(int i=0;i<get_num;i++){
			if(e_event[i].events & READ_MASK){
				recv_response(e_event[i].data.ptr);
			}
			if(e_event[i].events & WRITE_MASK){
				send_request(e_event[i].data.ptr);	
			}
			try_close(e_event[i].data.ptr);
		}
	}
}

int multi_add(int epoll_fd, int fd, int new_event, void* privdata){
	struct epoll_event event;
	event.events = new_event ? new_event : IO_EV_REG;
	event.data.ptr = privdata;
	if(epoll_ctl(epoll_fd,EPOLL_CTL_ADD,fd,&event)==-1){
		perror("epoll_ctl");
		return -1;
	}
	return 0;
}

int multi_mod(int epoll_fd, int fd, int new_event, void* privdata){
	struct epoll_event event;
	event.events = new_event;
	event.data.ptr = privdata;
	if(epoll_ctl(epoll_fd,EPOLL_CTL_MOD,fd,&event)==-1){
		perror("epoll_mod");
		return -1;
	}
	return 0;
}

int multi_del(int epoll_fd, int fd){
	if(epoll_ctl(epoll_fd,EPOLL_CTL_DEL,fd,0)==-1){
		perror("epoll_ctl");
		return -1;
	}
	return 0;
} 

void recv_response(void* privdata)
{
	Channel* p = (Channel*)privdata;
	p->DataIn();
}

void send_request(void* privdata)
{
	Channel* p = (Channel*)privdata;
	p->DataOut();
}

void try_close(void* privdata)
{
	Channel* p = (Channel*)privdata;
	p->TryClose();
}

enum POLL_IO_EVENT_MASK
{
	IO_EVENT_READ_MASK  = IO_EV_READ | IO_EV_ERR,
	IO_EVENT_WRITE_MASK = IO_EV_WRITE,
};

Poll Poll::inst;

Poll::~Poll() 
{

}

void  Poll::PollWait(int wait_ms)
{
#ifdef _SINGLE_THREAD_
	static struct epoll_event e_event[MAX_MULTI_FD];
#else
	struct epoll_event e_event[MAX_MULTI_FD];
#endif
	BeforePoll();
	do
	{
		int get_num = epoll_wait(_epoll_fd, e_event, MAX_MULTI_FD, wait_ms);
		if (get_num == -1) {
			if (errno == EINTR || errno == EAGAIN) {
				break;
			}
			else {
				perror("epoll");
				exit(0);
			}
		}
		for (int i = 0; i < get_num; i++) {
			if (e_event[i].events & IO_EVENT_READ_MASK) {
				recv_response(e_event[i].data.ptr);
			}
			if (e_event[i].events & IO_EVENT_WRITE_MASK) {
				send_request(e_event[i].data.ptr);
			}
			try_close(e_event[i].data.ptr);
		}
	} while (0);
	AfterPoll();
}

void* Poll::StartThreadPollWait(void* arg)
{
	//ÆÁ±ÎÏß³ÌÐÅºÅ
	ThreadPool::InitIgnoreSig();
	Poll* pInstance =  Poll::GetInstance();
	for(;;)
	{
		pInstance->PollWait();
	}
	return NULL;
}

bool Poll::Init()
{
	_epoll_fd = epoll_create1(0);
#ifndef _SINGLE_THREAD_
	ThreadPool::CreateSpecThread(_epoll_thr_id, StartThreadPollWait, NULL);
#endif
	return true;
}

void Poll::IOEventMod(Channel* p, int new_mask)
{
	multi_mod(_epoll_fd, p->_socket.GetSocket(), new_mask, p);
}

void Poll::IOEventRegister(Channel* p, bool allow_read, bool allow_write)
{
	int new_mask = 0;
	if(allow_read) new_mask |= IO_EV_READ;
	if(allow_write) new_mask |= IO_EV_WRITE;
	multi_add(_epoll_fd, p->_socket.GetSocket(), new_mask, p);
	p->_event_mask = new_mask;
}

void Poll::IOEventUnRegister(Channel* p)
{
	multi_del(_epoll_fd, p->_socket.GetSocket());
}

void Poll::AddPollTask(Task* pTask, PollTaskPosEnum pos, bool once)
{
	if (pos == PollTaskPosEnum::POLL_AFTER_TASK)
	{
		_before_tasks.emplace_back(new PollInnerTask(pTask, once));
	}
	else
	{
		_after_tasks.emplace_back(new PollInnerTask(pTask, once));
	}
}

void Poll::BeforePoll()
{
	for (PollTaskList::iterator it = _before_tasks.begin(); it != _before_tasks.end();)
	{
		bool once = (*it)->Exec();
		if (once)
		{
			it = _before_tasks.erase(it);
		}
		else 
		{
			++it;
		}
	}
}

void Poll::AfterPoll()
{
	for (PollTaskList::iterator it = _after_tasks.begin(); it != _after_tasks.end();)
	{
		bool once = (*it)->Exec();
		if (once)
		{
			it = _after_tasks.erase(it);
		}
		else
		{
			++it;
		}
	}
}