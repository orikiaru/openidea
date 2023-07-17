#include <unistd.h>
#include <time.h>
#include <map>
#include <assert.h>
#include "spin_lock.h"
#include "errno.h"
#include <sys/time.h>
#include <set>
#include <stdio.h>





#ifndef _SINGLE_THREAD_

//SpinLock 

#ifndef USE_PTHREAD_SPINLOCK
void SpinLock::Lock()
{
	int _test_count = 0;
	time_t o_time = time(NULL);
	while(1)
	{
		if(__sync_fetch_and_add(&_l, 1) == 0)
		{
			return;
		}
		_test_count ++;
		if((_test_count % 100000 == 0) && time(NULL) - o_time > 5)
		{
			printf("spin_lock time_out maybe deadlock\n");
			assert(false);
		}
		__sync_fetch_and_sub(&_l, 1);	
		pthread_yield();
	}
}

void SpinLock::UnLock()
{
	__sync_fetch_and_sub(&_l, 1);
}

#else
void SpinLock::Lock()
{
	//TryLock();
	pthread_spin_lock(&_l);
}

void SpinLock::TryLock(int time_out)
{
	int _test_count = 0;
	time_t o_time = time(NULL);
	while(1)
	{
		if(EBUSY != pthread_spin_trylock(&_l))
		{
			return;
		}
		++_test_count;
		if((_test_count % 100000 == 0) && time(NULL) - o_time > time_out)
		{
			printf("spin_lock time_out maybe deadlock\n");
			assert(false);
		}
		pthread_yield();
	}
}

void SpinLock::UnLock()
{
	pthread_spin_unlock(&_l);
}
#endif

//Mutex

//#define __MUTEX_THREAD_ONCE__
Mutex::Mutex()
{
	pthread_mutexattr_t 	_mutex_attr;
	pthread_mutexattr_init(&_mutex_attr);
#ifdef __MUTEX_THREAD_ONCE__
	pthread_mutexattr_settype(&_mutex_attr, PTHREAD_MUTEX_RECURSIVE);	//allow on one thread lock many times, I think can use in weakly realtime system if you more need the Robustness of the system
#else
	pthread_mutexattr_settype(&_mutex_attr, PTHREAD_MUTEX_NORMAL);		//not allow on one thread lock many times	//default
#endif
	pthread_mutex_init(&_l, &_mutex_attr);
	pthread_mutexattr_destroy(&_mutex_attr);
}

void Mutex::TryLock()
{
	int _test_count = 0;
	time_t o_time = time(NULL);
	while(1)
	{
		if(EBUSY != pthread_mutex_trylock(&_l))
		{
			return;
		}
		++_test_count;
		if((_test_count % 100000 == 0) && time(NULL) - o_time > 10)
		{
			printf("mutex time_out maybe deadlock\n");
			assert(false);
		}
		pthread_yield();
	}
}

Mutex::~Mutex()
{
	//to destory the mutex		
	pthread_mutex_destroy(&_l);	//no errno in manpage, no care
}



#endif














//FOR TEST
#ifdef __JXY_TEST__
int g_count = 0;
int g_count1 = 0;
SpinLock l;
void *doit(void * arg)
{
	int *p_count = (int*)arg;
	pthread_detach(pthread_self());
	for(int m = 0; m < *p_count; ++m)
	{
		//SpinLockGuard lock(l);
		l.Lock();
		++g_count;
		l.UnLock();
	}
}

Mutex  l_m;
void *doit2(void *arg)
{
	int *p_count = (int*)arg;
	pthread_detach(pthread_self());
	for(int m = 0; m < *p_count; ++m)
	{
		//l_m.Lock();
		MutexGuard lock(l_m);
		//MutexGuard lock1(l_m);
		++g_count1;
		//l_m.UnLock();
	}
}

int main()
{
	int test_thread_count = 6;
	int test_task_count = 1000000000;
	pthread_t pid[test_thread_count];
	for(int j = 0 ; j < test_thread_count; ++j)
	{
		pthread_create(&pid[j], NULL, doit2, &test_task_count);
		//pthread_create(&pid[j], NULL, doit, &test_task_count);
	}
	//while(1)
	{
	}

	//l.Lock();	//TEST DEADLOCK
	//sleep(999);

	for(int i = 0; i < 10; ++i)
	{
		sleep(20);
		l.Lock();
		l_m.Lock();
		printf("final=%d,final1=%d,in 20s\n", g_count, g_count1);
		g_count = 0;
		g_count1 = 0;
		l_m.UnLock();
		l.UnLock();
	}


	return 0;
}
#endif
