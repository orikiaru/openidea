#pragma once

#include "net_channel.h"
class SSL_Channel : public NetChannel
{
public:
	SSL_Channel(NetSocket s, Session* pSession) : NetChannel(s, pSession) {}
	virtual ~SSL_Channel() {}
	virtual void DataIn();
	virtual void DataOut();
	//virtual bool TryClose();	//使用基类实现
};