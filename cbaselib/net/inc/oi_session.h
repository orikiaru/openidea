#ifndef __io_session_h__
#define __io_session_h__
#include "oi_lock.h"
#include "oi_bytes.h"
#include "oi_parser.h"
#include <errno.h>
#include <string>
#include "oi_poll.h"
enum 
{
	IO_ST_CONNECTED,
	IO_ST_WILL_CLOSE,
	IO_ST_CLOSED,	//无意义目前关闭时,对象已销毁
};

class Protocol;
class SessionManager;
	
class Session
{
	friend class Poll;
protected:
	int32_t 	_status;
	int32_t		_errcode;

	OI::Base::Bytes		_obuff;
	OI::Base::Bytes		_ibuff;
	OI::Thread::Mutex		_locker;

	sid_t		_session_id;
	Channel*	_p_channel;
	SessionManager* _p_session_mgr;
public:
	Session(SessionManager* pMgr);
	virtual ~Session();

public: //提供给Channel的调用
	void SetChannel(Channel* pChannel) { _p_channel = pChannel; }
	//从Channel的inBuff提取数据到_ibuff
	virtual void SwapInBuff(OI::Base::Bytes& inBuff);
	//将会话输出数据处理后追加到outBuff
	virtual void SwapOutBuff(OI::Base::Bytes& outBuff);
	//Channel写空了。会话这边也没有待发数据才关写事件
	void SendDrained();
	//会话是否已经失效关闭
	virtual bool IsClosed();
	//IO线程被动关闭调用,会回调Mgr->CloseSession
	virtual void PassiveClose(int err) = 0;	

public:
	//Session自实现
	virtual void OnDataIn()	= 0;
	virtual void OnDataOut() = 0;

public:
	//向外SessionManager暴露的接口
	inline int32_t GetError()const		{ return _errcode;	}
	inline void SetSessionID(int sid) 	{ _session_id = sid;	}
	inline int  GetSessionID() const	{ return _session_id;	}

	virtual void Send(const OI::Base::Bytes& bytes)	{ assert(false); };
	virtual void SendProtocol(const Protocol& proto){ SendProtocol(&proto); }
	virtual void SendProtocol(const Protocol* proto) = 0;
	virtual void ActiveClose() = 0;			//外部Mgr主动关闭调用,有锁,不会回调Mgr->CloseSession

protected:
	void AllowSend();
	void ForbidSend();
	void AllowRead();
	void ForbidRead();
	virtual void Close(int err) = 0;
	virtual void OnClose() {}
};













#endif
