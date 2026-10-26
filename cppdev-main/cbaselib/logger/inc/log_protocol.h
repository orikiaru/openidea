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
	enum
	{
		PB_LV = 0,
		PB_CONTENT = 1,
	};
	virtual BytesStream& serialize(BytesStream& os)const
	{
		os.WritePb(PB_LV, _lv);
		os.WritePb(PB_CONTENT, _log_content);
		return os;
	}
	virtual BytesStream& deserialize(BytesStream& os)
	{
		while (os.RemainSize() > 0)
		{
			uint32_t seq = 0;
			uint32_t size = 0;
			os.ReadPbHeader(seq, size);
			switch (seq)
			{
			case PB_LV: os.ReadPb(size, _lv); break;
			case PB_CONTENT: os.ReadPb(size, _log_content); break;
			default: os.SkipPb(size); break;
			}
		}
		return os;
	}
};