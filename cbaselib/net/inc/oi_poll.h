#pragma once
#include <thread>
#include "oi_singleton.h"
struct uv_loop_s;
namespace OI
{
namespace Net
{

class Channel;

// 套接字在 poll 上关心的事件。REG 表示读或写至少打开了一个。
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
	// 跑 libuv 的线程。单线程就是调用 Init 的那条。
	std::thread::id m_stLoopThread;
	// 多线程下的 poll 线程。StopAndJoin 等它结束。
	std::thread m_stThread;

  public:
	~Poll();

	// 准备 loop。多线程拉起 poll 线程并等到就绪；线程创建失败返回 false。
	bool Init();
	// 正在 poll 里。单线程看 PollWait 的嵌套深度；多线程看是不是 loop 线程。同步等待不能从这里发起。
	bool InPollCallback() const;

	// 当前线程是不是跑 libuv 的那条。
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

	// 按读写开关把通道挂上 loop。真正改监视在 poll 线程完成。
	void IOEventRegister(Channel* pChannel, bool bAllowRead = true, bool bAllowWrite = false);
	// 改已挂通道的事件掩码。监视还没挂上时只更新期望值。
	void IOEventMod(Channel* pChannel, int iNewMask);
	// 只在 poll 线程调用。先摘掉事件，ms 毫秒后恢复成暂停前的掩码。通道在这段时间里不能删。
	void PauseEvents(Channel* pChannel, unsigned iMilliseconds);
	// close_fd 为假时只摘掉监听，套接字留给调用方关。
	void IOEventUnRegister(Channel* pChannel, bool bCloseDescriptor = true);
	// poll 线程入口，跑到停止为止。
	static void* StartThreadPollWait(void* pArgument);
	// 通知 loop 退出，并等 poll 线程结束。
	void StopAndJoin();
	// wait_ms 为 0 时只试一次已经就绪的事件，不睡。否则堵住等网络或定时器。
	void PollWait(int iWaitMilliseconds = 1000);
	// libuv 重复定时器。循环起来之前调用，到点在 poll 线程执行 fn。
	void ArmTick(unsigned iRepeatMilliseconds, void (*fnFunction)());
};

}
}
