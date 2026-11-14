#pragma once
#include "oi_net_channel.h"
namespace OI
{
namespace Net
{

class TCP_Channel : public NetChannel
{
  public:
	TCP_Channel(NetSocket stText, Session* pSession)
	    : NetChannel(stText, pSession)
	{
	}
	virtual ~TCP_Channel()
	{
	}
	void DataIn() override;
	void DataOut() override;
	//virtual bool TryClose();	//使用基类实现
};
}
}