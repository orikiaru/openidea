#pragma once
#include <atomic>
#include <map>
#include <string>
#include <vector>
#include "oi_bytes.h"
#include "oi_channel.h"
#include "oi_header.h"
#include "oi_lock.h"
struct uv_timer_s;
namespace OI
{
namespace Net
{

class SessionManager;
class UDP_Channel;

// 一个对端地址一条会话。自己不挂 poll：收包由 UDP_Channel 按来源分过来，
// 会话每次 Send 就在 AllowSend 里直接 sendto 成一包，包界和 Send 一一对应。
class UDP_PeerChannel : public Channel
{
	friend class UDP_Channel;
	UDP_Channel* m_pOwner;
	// 对端地址。发包目的地，也是 GetPeerAddr 的结果。
	NetAddr m_stAddr;
	// 上次收发的单调时钟毫秒。空闲回收看它。
	std::atomic<uint64_t> m_iLastActive;
	// 已经排进待回收表。只在 UDP_Channel 的回收锁里读写。
	bool m_bClosing = false;

  public:
	UDP_PeerChannel(UDP_Channel* pOwner, const NetAddr& stAddr, Session* pSessionLocal);
	void DataIn() override
	{
	}
	void DataOut() override
	{
	}
	bool TryClose() override
	{
		return false;
	}
	int32_t AllowSend() override;
	int32_t ForbidSend() override;
	int32_t AllowRead() override;
	int32_t ForbidRead() override;
};

// 一个绑好的 UDP 口，只在 poll 上挂一次读。管理器的会话按对端地址挂在这里。
// 收：一次 recvfrom 一包，按来源找会话，没见过的地址用 CloneSession 新建，再 SwapInBuff。
// 发：会话自己 sendto，不走写事件。只有 sendto 接受整包才 OnBytesSend。超限或本地发送失败直接返回错误，这一包丢掉，不自动重发。
// 关：会话关闭后排到 poll 线程回收；空闲超过 idle_timeout_ms 的会话由计时器关掉。
class UDP_Channel : public Channel
{
	SessionManager* m_pSessionManager;
	// 实际绑上的本端端口。配置写 0 时由系统分配。
	uint16_t m_iLocalPort;
	// 收包缓冲。只在 poll 线程用。
	::OI::Base::Bytes m_stInput;
	// 护两张表。新建可以在任意线程，删除只在 poll 线程，所以 poll 线程出锁后还能用查到的指针。
	// 持锁时会调 AddSession；不能在持管理器锁或会话锁时再来拿它。
	::OI::Thread::Mutex m_stPeerLock;
	// 端点文本到对端。
	std::map<std::string, UDP_PeerChannel*> m_mPeers;
	// 会话号到对端。
	std::map<sid_t, UDP_PeerChannel*> m_mSessions;
	// 只护待回收表，持锁时不调出去。
	::OI::Thread::Mutex m_stClosingLock;
	std::vector<UDP_PeerChannel*> m_vClosing;
	bool m_bRecyclePosted = false;
	uv_timer_s* m_pIdleTimer = nullptr;

	// 持 m_stPeerLock 调用。已有且没关的直接返回，否则新建一条会话。
	UDP_PeerChannel* OpenLocked(const NetAddr& stAddr);
	static void OnRecycle(void* pArgument);
	void Recycle();
	static void OnStartIdle(void* pArgument);
	static void OnIdle(uv_timer_s* pTimer);
	void CloseIdle();

  public:
	UDP_Channel(NetSocket stSocketLocal, SessionManager* pMgr, uint16_t iLocalPort);
	// 绑管理器配置的地址，挂上 poll，记到管理器上。
	static int32_t StartBind(SessionManager* pMgr);
	uint16_t LocalPort() const
	{
		return m_iLocalPort;
	}
	// 找到或新建到这个地址的会话，返回会话号。任意线程可调。
	sid_t OpenPeer(const NetAddr& stAddr);
	// 会话的对端地址。不是本口的会话返回 false。
	int32_t GetPeerAddr(sid_t iSessionId, NetAddr& stAddr);
	// 对端通道在会话锁里调。把会话待发的字节作为一包发出去。sendto 的结果决定回包：接受整包才扣额度并刷新活跃时间。
	int32_t Flush(UDP_PeerChannel* pPeer);
	// 会话已经关了，排到 poll 线程回收。
	void MarkClosing(UDP_PeerChannel* pPeer);

	void DataIn() override;
	void DataOut() override
	{
	}
	bool TryClose() override
	{
		return false;
	}
};
}
}
