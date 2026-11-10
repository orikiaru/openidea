#pragma once
#include "oi_lock.h"
#include "oi_socket.h"
namespace OI
{
namespace Net
{

class Session;
class Channel
{
  public:
	// 工作线程发布期望掩码，poll 线程读取；复合修改仍由通道锁串行化。
	// 期望的读写掩码。工作线程写，poll 线程读。
	std::atomic<int32_t> iEventMask;
	// poll 侧的监视对象。还没挂上、已摘掉，或正在用的 IoWatch，由 poll 解释。
	void* pPollToken;

  protected:
	// 这条通道的套接字和地址。
	NetSocket stSocket;
	// 数据进出交给它。监听通道可以为空。
	Session* pSession;
	// 串行化掩码修改。
	::OI::Thread::Mutex stLocker;

  public:
	friend class Poll;
	Channel(NetSocket stText, Session* pSession)
	    : iEventMask(0), pPollToken(NULL), stSocket(stText), pSession(pSession)
	{
	}
	virtual ~Channel();

  public:
	// poll 线程上可读。
	virtual void DataIn() = 0;
	// poll 线程上可写。
	virtual void DataOut() = 0;
	// 子类决定现在能不能关。返回 true 表示可以回收。
	virtual bool TryClose() = 0;

  public:
	// 套接字交给调用方关闭。析构看到的是无效号，不会再关一次。
	void DetachSocket()
	{
		stSocket.Detach();
	}
	// 打开写事件。关闭中的会话也打开一次，让空闲连接能被收回。
	// 会话在 Send 追加完、仍持会话锁时调用。UDP 直接发送一包并返回本地发送结果。
	// 返回非 OI_OK 时不要调用 OnBytesSend。缓冲里被取走的未写出字节由 SendResult 退回额度。
	virtual int32_t AllowSend();
	// 关掉写事件。会话已经关闭时不动。
	virtual void ForbidSend();
	// 打开读事件。会话已经关闭时不动。
	virtual void AllowRead();
	// 关掉读事件。会话已经关闭时不动。
	virtual void ForbidRead();
};
}
}
