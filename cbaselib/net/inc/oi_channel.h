#pragma once
#include "oi_lock.h"
#include "oi_socket.h"
namespace OI { namespace Net {

class Session;
class Channel
{
public:
	int32_t		_event_mask;
	void*		_poll_token;
protected:
	NetSocket	_socket;
	Session*	_p_session;
	::OI::Thread::Mutex		_locker;
public:
	friend class Poll;
	Channel(NetSocket s, Session* pSession) :_event_mask(0), _poll_token(NULL), _socket(s), _p_session(pSession) {}
	virtual ~Channel();

public:
	virtual void DataIn() = 0;
	virtual void DataOut() = 0;
	virtual bool TryClose() = 0;

public:
	void AllowSend();
	void ForbidSend();
	void AllowRead();
	void ForbidRead();
};
}}
