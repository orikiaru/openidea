#pragma once
#include "oi_net_channel.h"
namespace OI { namespace Net {

class TCP_Channel : public NetChannel
{
public:
	TCP_Channel(NetSocket stTextParam, Session* pSessionParam) : NetChannel(stTextParam, pSessionParam){}
	virtual ~TCP_Channel() {}
	virtual void DataIn();
	virtual void DataOut();
	//virtual bool TryClose();	//使用基类实现
};
}}