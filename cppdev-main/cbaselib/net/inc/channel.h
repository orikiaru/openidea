#pragma once
//Channel对应一种套接字的读取方式,如udp,tcp,ssl,管道,unix域等等
//与之相对的Session则是应用层解码器,将读取的数据序列化或反序列化给应用层使用
//Session维护会话所有数据,Channel只维护部分临时数据和套接字等
#include "spin_lock.h"
#include "net_socket.h"

class Session;
class Channel
{
protected:
	NetSocket	_socket;
	Session*	_p_session;		//绑定的会话
	int			_event_mask;	//IO事件掩码
	Mutex		_locker;		//掩码锁
public:
	friend class Poll;
	Channel(NetSocket s, Session* pSession) :_socket(s), _p_session(pSession),_event_mask(0) {}
	virtual ~Channel();

public:
	//epoll事件回调接口
	virtual void DataIn() = 0;		//real_data_handle	
	virtual void DataOut() = 0;		//real_data_send
	virtual bool TryClose() = 0;	//check err ? close : nothing done;

public:
	//epoll关注事件修改
	void AllowSend();
	void ForbidSend();
	void AllowRead();
	void ForbidRead();
};