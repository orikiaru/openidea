#include "oi_udp_channel.h"
#include "oi_errno.h"
#include "oi_log.h"
#include "oi_net_os.h"
#include "oi_poll.h"
#include "oi_session.h"
#include "oi_session_manager.h"
#include <chrono>
#include <uv.h>
namespace OI
{
namespace Net
{

static uint64_t UdpNowMilliseconds()
{
	return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

UDP_PeerChannel::UDP_PeerChannel(UDP_Channel* pOwner, const NetAddr& stAddr, Session* pSessionLocal)
    : Channel(NetSocket(), pSessionLocal), m_pOwner(pOwner), m_stAddr(stAddr), m_iLastActive(UdpNowMilliseconds())
{
}

int32_t UDP_PeerChannel::ForbidSend()
{
	return Poll::GetInstance().CheckThreadAccess("UDP_PeerChannel::ForbidSend");
}

int32_t UDP_PeerChannel::AllowRead()
{
	return Poll::GetInstance().CheckThreadAccess("UDP_PeerChannel::AllowRead");
}

int32_t UDP_PeerChannel::ForbidRead()
{
	return Poll::GetInstance().CheckThreadAccess("UDP_PeerChannel::ForbidRead");
}

int32_t UDP_PeerChannel::AllowSend()
{
	const int32_t iThreadStatus = Poll::GetInstance().CheckThreadAccess("UDP_PeerChannel::AllowSend");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}

	if (pSession->IsClosed())
	{
		m_pOwner->MarkClosing(this);
		return OI_ERR_CONNECT;
	}
	return m_pOwner->Flush(this);
}

UDP_Channel::UDP_Channel(NetSocket stSocketLocal, SessionManager* pMgr, uint16_t iLocalPort)
    : Channel(stSocketLocal, NULL), m_pSessionManager(pMgr), m_iLocalPort(iLocalPort)
{
	m_stInput.Reserve(64 * 1024);
}

int32_t UDP_Channel::StartBind(SessionManager* pMgr)
{
	const int32_t iThreadStatus = Poll::GetInstance().CheckThreadAccess("UDP_Channel::StartBind");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}
	if (!pMgr)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}

	if (pMgr->pUdpChannel)
	{
		LOG_ERROR("UDP_Channel::StartBind already bound id=%s", pMgr->GetIDString().c_str());
		return OI_ERR_EXIST;
	}
	NetAddr stAddr;
	if (!stAddr.Init(pMgr))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	net_socket_t iDescriptor = stAddr.CreateUdp(true);
	if (iDescriptor == INVALID_SOCKET_ID)
	{
		LOG_ERROR("UDP_Channel::StartBind id=%s, err=%s", pMgr->GetIDString().c_str(), NetOs_StrError(errno));
		return OI_ERR_SOCKET;
	}
	if (NetOs_BindDatagram(iDescriptor, stAddr.NetAttrPtr(), stAddr.NetAttrLen()) != 0)
	{
		LOG_ERROR("UDP_Channel::StartBind bind id=%s, err=%s", pMgr->GetIDString().c_str(), NetOs_StrError(errno));
		NetOs_Close(iDescriptor);
		return OI_ERR_BIND;
	}
	sockaddr_storage stLocal;
	memset(&stLocal, 0, sizeof(stLocal));
	socklen_t iLength = sizeof(stLocal);
	NetAddr stBound;
	if (NetOs_GetSockName(iDescriptor, (sockaddr*)&stLocal, &iLength) != 0 || !stBound.Init((const sockaddr*)&stLocal, iLength))
	{
		LOG_ERROR("UDP_Channel::StartBind getsockname id=%s, err=%s", pMgr->GetIDString().c_str(), NetOs_StrError(errno));
		NetOs_Close(iDescriptor);
		return OI_ERR_BIND;
	}
	NetSocket stSocketLocal;
	stSocketLocal.Init(iDescriptor, pMgr);
	UDP_Channel* pChannelLocal = new UDP_Channel(stSocketLocal, pMgr, stBound.Port());
	pMgr->pUdpChannel = pChannelLocal;
	Poll::GetInstance().IOEventRegister(pChannelLocal, true, false);
	Poll::GetInstance().CallOnLoop(&UDP_Channel::OnStartIdle, pChannelLocal);
	return OI_OK;
}

UDP_PeerChannel* UDP_Channel::OpenLocked(const NetAddr& stAddr)
{
	const std::string sEndpoint = stAddr.Endpoint();
	auto it = m_mPeers.find(sEndpoint);
	if (it != m_mPeers.end() && !it->second->pSession->IsClosed())
	{
		return it->second;
	}
	// 旧会话关了还没回收时，新会话顶掉表项；回收只摘自己那一项。
	Session* pSessionLocal = m_pSessionManager->CloneSession();
	UDP_PeerChannel* pPeer = new UDP_PeerChannel(this, stAddr, pSessionLocal);
	pSessionLocal->SetChannel(pPeer);
	m_mPeers[sEndpoint] = pPeer;
	m_pSessionManager->AddSession(pSessionLocal, sEndpoint, m_pSessionManager->MaxSendQueueBytes(), m_pSessionManager->MaxReceiveBufferBytes());
	m_mSessions[pSessionLocal->GetSessionID()] = pPeer;
	return pPeer;
}

sid_t UDP_Channel::OpenPeer(const NetAddr& stAddr)
{
	::OI::Thread::MutexGuard stLock(m_stPeerLock);
	return OpenLocked(stAddr)->pSession->GetSessionID();
}

int32_t UDP_Channel::GetPeerAddr(sid_t iSessionId, NetAddr& stAddr)
{
	::OI::Thread::MutexGuard stLock(m_stPeerLock);
	auto it = m_mSessions.find(iSessionId);
	if (it == m_mSessions.end())
	{
		return OI_ERR_NOT_FOUND;
	}
	stAddr = it->second->m_stAddr;
	return OI_OK;
}

int32_t UDP_Channel::Flush(UDP_PeerChannel* pPeer)
{
	::OI::Base::Bytes stOutput;
	pPeer->pSession->SwapOutBuff(stOutput);
	if (stOutput.Size() == 0)
	{
		return OI_OK;
	}
	// 这一包已经离开会话缓冲。超限或 sendto 没收下整包就丢掉，不并进下一包，也不调用 OnBytesSend。
	// 发送额度由 SendResult 按取走但未写出的字节退回。
	const size_t iMaxDatagramBytes = pPeer->m_stAddr.Family() == AF_INET ? 65507 : 65527;
	if (stOutput.Size() > iMaxDatagramBytes)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	int iSent = -1;
	for (;;)
	{
		iSent = NetOs_SendTo(stSocket.GetSocket(), stOutput.Begin(), stOutput.Size(), pPeer->m_stAddr.NetAttrPtr(), pPeer->m_stAddr.NetAttrLen());
		if (iSent < 0 && errno == EINTR)
		{
			continue;
		}
		break;
	}
	if (iSent < 0)
	{
		const int iError = errno;
		LOG_TRACE("UDP_Channel::Flush drop sid=%d bytes=%zu errno=%d err=%s", pPeer->pSession->GetSessionID(), stOutput.Size(), iError, NetOs_StrError(iError));
		return OI_ERR_FAIL;
	}
	if ((size_t)iSent != stOutput.Size())
	{
		LOG_TRACE("UDP_Channel::Flush drop sid=%d bytes=%zu sent=%d", pPeer->pSession->GetSessionID(), stOutput.Size(), iSent);
		return OI_ERR_FAIL;
	}
	pPeer->pSession->OnBytesSend(stOutput.Size());
	pPeer->m_iLastActive.store(UdpNowMilliseconds(), std::memory_order_relaxed);
	return OI_OK;
}

void UDP_Channel::MarkClosing(UDP_PeerChannel* pPeer)
{
	bool bPost = false;
	{
		::OI::Thread::MutexGuard stLock(m_stClosingLock);
		if (pPeer->m_bClosing)
		{
			return;
		}
		pPeer->m_bClosing = true;
		m_vClosing.push_back(pPeer);
		bPost = !m_bRecyclePosted;
		m_bRecyclePosted = true;
	}
	if (bPost)
	{
		// 关闭可能发生在管理器锁里，回收要回调管理器，所以一律排到下一轮。
		Poll::GetInstance().PostOnLoop(&UDP_Channel::OnRecycle, this);
	}
}

void UDP_Channel::OnRecycle(void* pArgument)
{
	static_cast<UDP_Channel*>(pArgument)->Recycle();
}

void UDP_Channel::Recycle()
{
	std::vector<UDP_PeerChannel*> vClosing;
	{
		::OI::Thread::MutexGuard stLock(m_stClosingLock);
		vClosing.swap(m_vClosing);
		m_bRecyclePosted = false;
	}
	for (UDP_PeerChannel* pPeer : vClosing)
	{
		Session* pSessionLocal = pPeer->pSession;
		{
			::OI::Thread::MutexGuard stLock(m_stPeerLock);
			auto itPeer = m_mPeers.find(pSessionLocal->GetEndpoint());
			if (itPeer != m_mPeers.end() && itPeer->second == pPeer)
			{
				m_mPeers.erase(itPeer);
			}
			auto itSession = m_mSessions.find(pSessionLocal->GetSessionID());
			if (itSession != m_mSessions.end() && itSession->second == pPeer)
			{
				m_mSessions.erase(itSession);
			}
		}
		// 和 NetChannel::Destroy 一样：先断开会话到通道的指针，再通知管理器并释放。
		pSessionLocal->DetachChannel();
		pSessionLocal->OnClose();
		pSessionLocal->Release();
		delete pPeer;
	}
}

void UDP_Channel::OnStartIdle(void* pArgument)
{
	UDP_Channel* pChannelLocal = static_cast<UDP_Channel*>(pArgument);
	const uint64_t iIdle = pChannelLocal->m_pSessionManager->IdleTimeoutMilliseconds();
	uv_timer_t* pTimer = new uv_timer_t();
	pTimer->data = pChannelLocal;
	uv_timer_init(Poll::GetInstance().Loop(), pTimer);
	const uint64_t iRepeat = iIdle < 4000 ? (iIdle / 4 > 0 ? iIdle / 4 : 1) : 1000;
	uv_timer_start(pTimer, &UDP_Channel::OnIdle, iRepeat, iRepeat);
	pChannelLocal->m_pIdleTimer = pTimer;
}

void UDP_Channel::OnIdle(uv_timer_s* pTimer)
{
	static_cast<UDP_Channel*>(pTimer->data)->CloseIdle();
}

void UDP_Channel::CloseIdle()
{
	const uint64_t iNow = UdpNowMilliseconds();
	const uint64_t iIdle = m_pSessionManager->IdleTimeoutMilliseconds();
	std::vector<Session*> vIdle;
	{
		::OI::Thread::MutexGuard stLock(m_stPeerLock);
		for (const auto& [sEndpoint, pPeer] : m_mPeers)
		{
			const uint64_t iLast = pPeer->m_iLastActive.load(std::memory_order_relaxed);
			if (iNow > iLast && iNow - iLast >= iIdle && !pPeer->pSession->IsClosed())
			{
				pPeer->pSession->AddRef();
				vIdle.push_back(pPeer->pSession);
			}
		}
	}
	// 关闭会回调管理器，出锁再关。
	for (Session* pSessionLocal : vIdle)
	{
		pSessionLocal->PassiveClose(ETIMEDOUT);
		pSessionLocal->Release();
	}
}

void UDP_Channel::DataIn()
{
	// 水平触发。一轮收够包数就先让出，剩下的下一轮再来。
	for (int iRound = 0; iRound < 64; ++iRound)
	{
		sockaddr_storage stFrom;
		memset(&stFrom, 0, sizeof(stFrom));
		socklen_t iLength = sizeof(stFrom);
		int iCount = NetOs_RecvFrom(stSocket.GetSocket(), m_stInput.Begin(), m_stInput.Capacity(), (sockaddr*)&stFrom, &iLength);
		if (iCount < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			if (errno != EAGAIN)
			{
				LOG_TRACE("UDP_Channel::DataIn id=%s, err=%s", m_pSessionManager->GetIDString().c_str(), NetOs_StrError(errno));
			}
			return;
		}
		NetAddr stAddr;
		if (iCount == 0 || !stAddr.Init((const sockaddr*)&stFrom, iLength))
		{
			continue;
		}
		// 按包长拷一份交出去。收包缓冲留着下次用，不让每条会话各占一块 64K。
		::OI::Base::Bytes stPacket;
		stPacket.Append(m_stInput.Begin(), (size_t)iCount);
		Session* pSessionLocal = NULL;
		{
			::OI::Thread::MutexGuard stLock(m_stPeerLock);
			UDP_PeerChannel* pPeer = OpenLocked(stAddr);
			pPeer->m_iLastActive.store(UdpNowMilliseconds(), std::memory_order_relaxed);
			pSessionLocal = pPeer->pSession;
		}
		// 删除只在本线程，出锁后会话还在。
		pSessionLocal->SwapInBuff(stPacket);
	}
}
}
}
