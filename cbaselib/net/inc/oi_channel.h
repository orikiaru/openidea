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
	int32_t iEventMask;
	void* pPollToken;

  protected:
	NetSocket stSocket;
	Session* pSession;
	::OI::Thread::Mutex stLocker;

  public:
	friend class Poll;
	Channel(NetSocket stTextParam, Session* pSessionParam)
	    : iEventMask(0), pPollToken(NULL), stSocket(stTextParam), pSession(pSessionParam)
	{
	}
	virtual ~Channel();

  public:
	virtual void DataIn() = 0;
	virtual void DataOut() = 0;
	virtual bool TryClose() = 0;

  public:
	// 套接字交给调用方关闭。析构看到的是无效号，不会再关一次。
	void DetachSocket()
	{
		stSocket.Detach();
	}
	void AllowSend();
	void ForbidSend();
	void AllowRead();
	void ForbidRead();
};
}
}
