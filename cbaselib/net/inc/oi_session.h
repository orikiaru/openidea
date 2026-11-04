#pragma once
#include <atomic>
#include "oi_lock.h"
#include "oi_bytes.h"
#include <errno.h>
#include <string>
#include "oi_poll.h"
namespace OI
{
namespace Net
{
enum
{
	IO_ST_CONNECTED,
	IO_ST_WILL_CLOSE,
	IO_ST_CLOSED, //无意义目前关闭时,对象已销毁
};

class SessionManager;

class Session
{
	friend class Poll;
	friend class NetChannel;

  protected:
	std::atomic<int32_t> iStatus;
	std::atomic<int32_t> iErrorCode;

	size_t iQueuedBytes = 0;
	size_t iMaxSendQueueBytes = 64 * 1024 * 1024;
	size_t iMaxReceiveBufferBytes = 64 * 1024 * 1024 + 8;
	::OI::Base::Bytes stOutputBuffer;
	::OI::Base::Bytes stInputBuffer;
	::OI::Thread::Mutex stLocker;

	sid_t iSessionId;
	Channel* pChannel;
	SessionManager* pSessionManager;
	std::string sEndpoint;
	// 通道持有一份，SendBytes 在锁外发送时临时再持一份。最后一份释放时 delete。
	std::atomic<int32_t> iReferences;

  public:
	Session(SessionManager* pMgrParam);
	virtual ~Session();
	void AddRef()
	{
		iReferences.fetch_add(1, std::memory_order_relaxed);
	}
	void Release();

  public: //提供给Channel的调用
	void SetChannel(Channel* pChannelParam)
	{
		pChannel = pChannelParam;
	}
	// 通道销毁前调用。之后 Send 只追加缓冲，不再碰通道。
	void DetachChannel();
	//从Channel的inBuff提取数据到_ibuff
	virtual void SwapInBuff(::OI::Base::Bytes& stInputBufferParam);
	//将会话输出数据处理后追加到outBuff
	virtual void SwapOutBuff(::OI::Base::Bytes& stOutputBufferParam);
	//Channel写空了。会话这边也没有待发数据才关写事件
	void SendDrained();
	//会话是否已经失效关闭
	virtual bool IsClosed();
	//IO线程被动关闭调用,会回调Mgr->CloseSession
	virtual void PassiveClose(int iErrorParam) = 0;

  public:
	//Session自实现
	virtual void OnDataIn() = 0;
	virtual void OnDataOut() = 0;

  public:
	//向外SessionManager暴露的接口
	inline int32_t GetError() const
	{
		return iErrorCode.load(std::memory_order_acquire);
	}
	inline void SetSessionID(int iSessionIdParam)
	{
		iSessionId = iSessionIdParam;
	}
	inline int GetSessionID() const
	{
		return iSessionId;
	}
	void SetEndpoint(const std::string& sEndpointParam)
	{
		sEndpoint = sEndpointParam;
	}
	const std::string& GetEndpoint() const
	{
		return sEndpoint;
	}

	bool Send(const ::OI::Base::Bytes& stBytesParam);
	void SetSendQueueLimit(size_t iBytesParam);
	// 接收缓冲仅 poll 线程访问；会话发布前设置限制。
	void SetReceiveQueueLimit(size_t iBytesParam)
	{
		iMaxReceiveBufferBytes = iBytesParam;
	}
	size_t ReceiveAllowance() const
	{
		return stInputBuffer.Size() < iMaxReceiveBufferBytes ? iMaxReceiveBufferBytes - stInputBuffer.Size() : 0;
	}
	size_t PendingSendBytes();
	void OnBytesSent(size_t iBytesParam);
	virtual void ActiveClose() = 0; //外部Mgr主动关闭调用,有锁,不会回调Mgr->CloseSession

  protected:
	void AllowSend();
	void ForbidSend();
	void AllowRead();
	void ForbidRead();
	virtual void Close(int iErrorParam) = 0;
	virtual void OnClose()
	{
	}
};

}
}
