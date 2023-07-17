#pragma once
#include "channel.h"
#include "bytes.h"
//Session需要提供接口 
//virtual void SwapInBuff(Bytes& inBuff);			//全量交换读取来的inbuff
//virtual void SwapOutBuff(Bytes& outBuff);			//全量交换需要输出的outbuff
//virtual bool IsClosed();							//确定会话是否已经处于关闭状态,处于此状态将会将会话析构

class NetChannel : public Channel
{
protected:
	Bytes		_ibuff;
	Bytes		_obuff;
public:
	NetChannel(NetSocket , Session* pSession);
	virtual ~NetChannel();
	//epoll事件回调接口
	virtual void DataIn() = 0;		//real_data_handle	
	virtual void DataOut() = 0;		//real_data_send
	virtual bool TryClose();
	//自定义销毁接口,默认为销毁会话并销毁自身
	virtual void Destory();
};