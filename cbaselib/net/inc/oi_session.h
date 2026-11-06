#pragma once
#include <atomic>
#include <memory>
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
	// 可以收发。
	IO_ST_CONNECTED,
	// 正在关。发送只会被拒绝，通道还可能再触发一次 IO 来回收。
	IO_ST_WILL_CLOSE,
	// 已关闭。当前关闭路径上对象已经销毁，这个值很少用到。
	IO_ST_CLOSED,
};

class SessionManager;

class Session
{
	friend class Poll;
	friend class NetChannel;
	friend class UDP_Channel;

  protected:
	// 连接状态，取 IO_ST_*。
	std::atomic<int32_t> iStatus;
	// 关闭原因。还没记错时是 0。
	std::atomic<int32_t> iErrorCode;

	// 输出缓冲里还没写到套接字的字节。
	size_t iQueuedBytes = 0;
	// 发送队列上限。再写就会返回忙。
	size_t iMaxSendQueueBytes = 64 * 1024 * 1024;
	// 接收缓冲上限。会话发布前设置，之后只由 poll 线程看。
	size_t iMaxReceiveBufferBytes = 64 * 1024 * 1024 + 8;
	// 待写到套接字的字节。
	::OI::Base::Bytes stOutputBuffer;
	// 已读入、还没交给协议解析的字节。
	::OI::Base::Bytes stInputBuffer;
	// 保护缓冲、状态和通道指针。
	::OI::Thread::Mutex stLocker;

	// 管理器分配的会话号。
	sid_t iSessionId;
	// 底下的套接字通道。DetachChannel 之后为空，Send 只追加缓冲。
	Channel* pChannel;
	// 所属管理器。关闭时用它把会话从表里摘掉。
	SessionManager* pSessionManager;
	// 对端地址文本，按地址关闭时用。
	std::string sEndpoint;
	// 通道持有一份，SendBytes 在锁外发送时临时再持一份。最后一份释放时 delete。
	std::atomic<int32_t> iReferences;

  public:
	Session(SessionManager* pMgr);
	virtual ~Session();
	// 入站任务持有的业务租约。默认没有，子类需要时返回一份共享状态。
	virtual std::shared_ptr<void> HoldTask()
	{
		return {};
	}
	// 增加一份引用。和 Release 配对。
	void AddRef()
	{
		iReferences.fetch_add(1, std::memory_order_relaxed);
	}
	// 引用减到 0 时删除自己。
	void Release();

  public: //提供给Channel的调用
	// 绑上通道。
	void SetChannel(Channel* pChannelLocal)
	{
		pChannel = pChannelLocal;
	}
	// 通道销毁前调用。之后 Send 只追加缓冲，不再碰通道。
	void DetachChannel();
	//从Channel的inBuff提取数据到_ibuff
	virtual void SwapInBuff(::OI::Base::Bytes& stInputBuffer);
	//将会话输出数据处理后追加到outBuff
	virtual void SwapOutBuff(::OI::Base::Bytes& stOutputBuffer);
	//Channel写空了。会话这边也没有待发数据才关写事件
	void SendDrained();
	//会话是否已经失效关闭
	virtual bool IsClosed();
	//IO线程被动关闭调用,会回调Mgr->CloseSession
	virtual void PassiveClose(int iError) = 0;

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
	inline void SetSessionID(int iSessionIdLocal)
	{
		iSessionId = iSessionIdLocal;
	}
	inline int GetSessionID() const
	{
		return iSessionId;
	}
	void SetEndpoint(const std::string& sEndpointLocal)
	{
		sEndpoint = sEndpointLocal;
	}
	const std::string& GetEndpoint() const
	{
		return sEndpoint;
	}

	// 追加到发送队列并打开写事件。已关闭或队列放不下返回 false。
	bool Send(const ::OI::Base::Bytes& stBytes);
	// 和 Send 一样。已关闭返回连接错误，队列放不下返回忙。
	int32_t SendResult(const ::OI::Base::Bytes& stBytes);
	// 改发送队列上限。
	void SetSendQueueLimit(size_t iBytes);
	// 接收缓冲仅 poll 线程访问；会话发布前设置限制。
	void SetReceiveQueueLimit(size_t iBytes)
	{
		iMaxReceiveBufferBytes = iBytes;
	}
	size_t ReceiveAllowance() const
	{
		return stInputBuffer.Size() < iMaxReceiveBufferBytes ? iMaxReceiveBufferBytes - stInputBuffer.Size() : 0;
	}
	// 还没写出去的字节数。
	size_t PendingSendBytes();
	// 通道写出这么多字节后扣掉排队计数。
	void OnBytesSent(size_t iBytes);
	virtual void ActiveClose() = 0; //外部Mgr主动关闭调用,有锁,不会回调Mgr->CloseSession

  protected:
	// 转给通道打开或关掉对应事件。
	void AllowSend();
	void ForbidSend();
	void AllowRead();
	void ForbidRead();
	// 子类完成关闭，并记下错误码。
	virtual void Close(int iError) = 0;
	virtual void OnClose()
	{
	}
};

}
}
