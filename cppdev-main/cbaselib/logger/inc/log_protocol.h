#pragma once
#include "protocol.h"

//走向日志服的协议不是正常Protocol,是需要特殊会话和特殊解码器处理的
class LogProtocol :public Protocol
{
public:
	LogProtocol(proto_type_t id) :Protocol(id) {}
	virtual Protocol* Clone() = 0;
	virtual void Handle(SessionManager* mgr, sid_t sid) = 0;
};

enum LOG_PROTO_TYPE
{
	LPT_REMOTE_LOG,
	LPT_LOG_CLIENT_REGISTER,
};

class RemoteLog : public LogProtocol
{
public:
	int			_lv;
	Bytes		_log_content;
public:
	explicit RemoteLog() : LogProtocol(LPT_REMOTE_LOG) {}
	RemoteLog(LOG_LEVEL lv, const Bytes& log) :
		LogProtocol(LPT_REMOTE_LOG), _lv(lv), _log_content(log) {}
	virtual void Handle(SessionManager* mgr, sid_t sid);
	virtual Protocol* Clone()
	{
		return new RemoteLog(*this);
	}
	virtual BytesStream& serialize(BytesStream& os)const
	{
		return os << _lv << _log_content;
	}
	virtual BytesStream& deserialize(BytesStream& os)
	{
		return os >> _lv >> _log_content;
	}
};