#pragma once
#include "spin_lock.h"
#include "net_socket.h"

class Session;
class Channel
{
public:
	int			_event_mask;
	void*		_poll_token;
protected:
	NetSocket	_socket;
	Session*	_p_session;
	Mutex		_locker;
public:
	friend class Poll;
	Channel(NetSocket s, Session* pSession) :_socket(s), _p_session(pSession), _event_mask(0), _poll_token(NULL) {}
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
