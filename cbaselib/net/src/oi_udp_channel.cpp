#include "oi_udp_channel.h"
#include "oi_errno.h"
#include "oi_log.h"
#include "oi_net_os.h"
#include "oi_poll.h"
#include "oi_session.h"
#include "oi_session_manager.h"
#include "oi_duration.h"
#include <uv.h>
namespace OI
{
namespace Net
{

UDP_Channel::UDP_Channel(NetSocket stSocketLocal, SessionManager* pMgr, uint16_t iLocalPort, Session* pPeer)
    : Channel(stSocketLocal, pPeer), m_pSessionManager(pMgr), m_iLocalPort(iLocalPort)
{
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

	if (!Poll::GetInstance().InLoopThread())
	{
		return Poll::GetInstance().StartListener([](void* pArgument)
		{
			return StartBind(static_cast<SessionManager*>(pArgument));
		}, pMgr);
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
	std::unique_ptr<UDP_Channel> pChannelLocal(new UDP_Channel(stSocketLocal, pMgr, stBound.Port(), nullptr));
	auto& stPoll = Poll::GetInstance();
	const int32_t iRegisterStatus = stPoll.IOEventRegister(pChannelLocal.get(), true, false);
	if (iRegisterStatus != OI_OK)
	{
		return iRegisterStatus;
	}
	const uint64_t iIdle = pMgr->IdleTimeoutMilliseconds();
	auto pTimer = std::make_unique<uv_timer_t>();
	int iStatus = uv_timer_init(stPoll.Loop(), pTimer.get());
	if (iStatus == 0)
	{
		uv_timer_t* pIdle = pTimer.release();
		stPoll.AdoptWatchTimer(pChannelLocal.get(), pIdle);
		pIdle->data = pMgr;
		const uint64_t iRepeat = iIdle < 4000 ? (iIdle / 4 > 0 ? iIdle / 4 : 1) : 1000;
		iStatus = uv_timer_start(pIdle, [](uv_timer_t* pHandle)
		{
			static_cast<SessionManager*>(pHandle->data)->CloseIdleUdpPeers();
		}, iRepeat, iRepeat);
	}
	if (iStatus != 0)
	{
		LOG_ERROR("UDP_Channel::StartBind idle timer id=%s err=%s (%d)", pMgr->GetIDString().c_str(), uv_strerror(iStatus), iStatus);
		return OI_ERR_FAIL;
	}
	pMgr->pUdpChannel = pChannelLocal.release();
	return OI_OK;
}

int32_t UDP_Channel::AllowSend()
{
	if (!pSession)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	if (pSession->IsClosed())
	{
		if (!pSession->bPeerClosing)
		{
			pSession->bPeerClosing = true;
			for (const auto& stPacket : m_vPackets)
			{
				pSession->iQueuedBytes -= stPacket.Size();
			}
			m_vPackets.clear();
			Poll::GetInstance().PostOnLoop([](void* pArgument)
			{
				auto* pChannel = static_cast<UDP_Channel*>(pArgument);
				pChannel->m_pSessionManager->RecycleUdpPeer(pChannel->pSession);
				delete pChannel;
			}, this);
		}
		return OI_ERR_CONNECT;
	}
	return QueuePacket(false);
}

int32_t UDP_Channel::QueuePacket(bool bFramed)
{
	::OI::Base::Bytes stOutput;
	pSession->SwapOutBuff(stOutput);
	if (stOutput.Size() == 0)
	{
		return OI_OK;
	}
	if (pSession->bPeerIdleExpired)
	{
		return OI_ERR_CONNECT;
	}
	// 入队前拒绝的整包由 SendResult 退还额度。
	const size_t iMaxDatagramBytes = pSession->stPeerAddr.Family() == AF_INET ? 65507 : 65527;
	if (stOutput.Size() > iMaxDatagramBytes)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	// 1200 字节仅限制合包。单条大帧仍可按数据报上限原样发送。
	constexpr size_t iBatchBytes = 1200;
	if (bFramed && m_bTailFramed && !m_vPackets.empty() && m_vPackets.back().Size() + stOutput.Size() <= iBatchBytes)
	{
		m_vPackets.back().Append(stOutput);
		return OI_OK;
	}
	m_vPackets.emplace_back(std::move(stOutput));
	m_bTailFramed = bFramed;
	if (m_vPackets.size() == 1)
	{
		Poll::GetInstance().ChangeEvents(m_pSessionManager->pUdpChannel, IO_EV_WRITE, 0);
	}
	return OI_OK;
}

// 队列非空即本通道有待发包。持会话锁，本轮只取这个对端的包。
bool UDP_Channel::Flush()
{
	::OI::Thread::MutexGuard stLock(pSession->stLocker);
	if (pSession->IsClosed() || pSession->bPeerIdleExpired)
	{
		return false;
	}
	for (int iRound = 0; iRound < 8 && !m_vPackets.empty(); ++iRound)
	{
		auto& stPacket = m_vPackets.front();
		int iSent;
		do
		{
			iSent = NetOs_SendTo(stSocket.GetSocket(), stPacket.Begin(), stPacket.Size(), pSession->stPeerAddr.NetAttrPtr(), pSession->stPeerAddr.NetAttrLen());
		} while (iSent < 0 && errno == EINTR);
		if (iSent < 0 && errno == EAGAIN)
		{
			return true;
		}
		if (iSent >= 0 && static_cast<size_t>(iSent) == stPacket.Size())
		{
			pSession->OnBytesSend(stPacket.Size());
			pSession->iPeerActive.store(::OI::Timer::SteadyMilliseconds(), std::memory_order_relaxed);
		}
		else
		{
			const int iError = iSent < 0 ? errno : 0;
			LOG_ERROR("UDP sendto sid=%d bytes=%zu sent=%d errno=%d", pSession->GetSessionID(), stPacket.Size(), iSent, iError);
			pSession->iQueuedBytes -= stPacket.Size();
		}
		m_vPackets.pop_front();
	}
	return !m_vPackets.empty();
}

void UDP_Channel::DataOut()
{
	// 先撤本次写关注。扫描中有新包入空队时，发送方会重新开启，避免覆盖唤醒。
	Poll::GetInstance().ChangeEvents(this, 0, IO_EV_WRITE);
	std::vector<UDP_Channel*> vPeers;
	{
		::OI::Thread::MutexGuard stLock(m_pSessionManager->m_stUdpPeerLock);
		vPeers.reserve(m_pSessionManager->m_mUdpPeers.size());
		for (const auto& [sEndpoint, pChannel] : m_pSessionManager->m_mUdpPeers)
		{
			vPeers.push_back(pChannel);
		}
	}
	// 只有 poll 回收通道，本轮内指针有效；发送时不持 peer 表锁。
	bool bPending = false;
	for (auto* pChannel : vPeers)
	{
		if (pChannel->Flush())
		{
			bPending = true;
		}
	}
	if (bPending)
	{
		Poll::GetInstance().ChangeEvents(this, IO_EV_WRITE, 0);
	}
}

void UDP_Channel::DataIn()
{
	m_stInput.Reserve(64 * 1024);
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
		Session* pSessionLocal;
		{
			::OI::Thread::MutexGuard stLock(m_pSessionManager->m_stUdpPeerLock);
			pSessionLocal = m_pSessionManager->OpenUdpPeerLocked(stAddr);
			if (!pSessionLocal)
			{
				continue;
			}
			pSessionLocal->iPeerActive.store(::OI::Timer::SteadyMilliseconds(), std::memory_order_relaxed);
		}
		// 删除只在本线程，出锁后会话还在。
		pSessionLocal->SwapInBuff(stPacket);
	}
}
}
}
