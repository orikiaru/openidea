#pragma once
#include <thread>
#include "oi_singleton.h"
struct uv_loop_s;
namespace OI
{
namespace Net
{

class Channel;

enum IO_EVENT_MASK
{
	IO_EV_READ = 0x001,
	IO_EV_WRITE = 0x004,
	IO_EV_ERR = 0x008,
	IO_EV_REG = IO_EV_READ | IO_EV_WRITE,
};

class Poll
{
	DECLARE_SINGLETON(Poll)
	Poll()
	{
	}
	std::thread::id m_stLoopThread;
	std::thread m_stThread;

  public:
	~Poll();

	bool Init();
	bool InPollCallback() const;

	bool InLoopThread() const
	{
		return std::this_thread::get_id() == m_stLoopThread;
	}
	// 套接字事件只从 Channel 的 DataIn / DataOut 进来。只在 poll 线程里用。
	::uv_loop_s* Loop();
	// 已经在 poll 线程就直接执行，否则丢进 libuv 再执行。定时器轮子只能在这条线程上改。
	void CallOnLoop(void (*fnFunction)(void*), void* pArgument);
	// 始终排队；本批回调中新投的调用留到下次唤醒，避免重入和饿死 IO。
	void PostOnLoop(void (*fnFunction)(void*), void* pArgument);

	void IOEventRegister(Channel* pChannel, bool bAllowRead = true, bool bAllowWrite = false);
	void IOEventMod(Channel* pChannel, int iNewMask);
	// 只在 poll 线程调用。先摘掉事件，ms 毫秒后恢复成暂停前的掩码。通道在这段时间里不能删。
	void PauseEvents(Channel* pChannel, unsigned iMilliseconds);
	// close_fd 为假时只摘掉监听，套接字留给调用方关。
	void IOEventUnRegister(Channel* pChannel, bool bCloseDescriptor = true);
	static void* StartThreadPollWait(void* pArgument);
	void StopAndJoin();
	// wait_ms 为 0 时只试一次已经就绪的事件，不睡。否则堵住等网络或定时器。
	void PollWait(int iWaitMilliseconds = 1000);
	// libuv 重复定时器。循环起来之前调用，到点在 poll 线程执行 fn。
	void ArmTick(unsigned iRepeatMilliseconds, void (*fnFunction)());
};

}
}
