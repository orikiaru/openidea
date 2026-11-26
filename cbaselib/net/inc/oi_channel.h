#pragma once
#include <cstdint>
#include "oi_socket.h"
namespace OI
{
namespace Net
{

class Session;
class Channel
{
  public:
	// 当前读写掩码，只在 poll 线程更新。
	int32_t iEventMask;

  protected:
	// 这条通道的套接字和地址。
	NetSocket stSocket;
	// 数据进出交给它。监听通道可以为空。
	Session* pSession;

  public:
	friend class Poll;
	Channel(NetSocket stText, Session* pSession)
	    : iEventMask(0), stSocket(stText), pSession(pSession)
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
	// 会话在 Send 追加完、仍持会话锁时调用。UDP 将本次数据作为独立数据报入队。
	// 返回非 OI_OK 时不要调用 OnBytesSend。缓冲里被取走的未写出字节由 SendResult 退回额度。
	virtual int32_t AllowSend();
	// 关掉写事件。会话已经关闭时不动。
	virtual int32_t ForbidSend();
	// 打开读事件。会话已经关闭时不动。
	virtual int32_t AllowRead();
	// 关掉读事件。会话已经关闭时不动。
	virtual int32_t ForbidRead();
	// 挂着会话且会话已关闭。poll 据此给等不到 IO 的关闭连接挂兜底回收。
	bool SessionClosed();
};
}
}
