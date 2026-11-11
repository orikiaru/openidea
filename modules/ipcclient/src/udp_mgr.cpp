#include "udp_mgr.h"
#include "oi_config.h"
#include "oi_log.h"
#include "oi_rpcdata_udpechoreq.h"
#include "oi_rpcdata_udpechores.h"
#include "oi_rpcdata_udpnoteres.h"
#include "oi_socket.h"
#include "rpc_ids.h"
#include "test_board.h"
#include <chrono>
#include <cstdio>

const std::string IpcUdp::GetIDString()
{
	return "IpcUdp";
}

void IpcUdp::OnAddSession(::OI::Net::Session* pSession)
{
	(void)pSession;
}

void IpcUdp::OnDelSession(const ::OI::Net::Session* pSession)
{
	UdpMgr::GetInstance().OnClose(pSession->GetSessionID());
}

UdpMgr::UdpMgr()
    : m_bEnabled(false), m_sServerIp("127.0.0.1"), m_iTotal(1000), m_iNotes(1000), m_iNext(0), m_iRpcBad(0),
      m_iClosedSessionId(0), m_iNoteReceived(0), m_iNoteDuplicate(0)
{
}

bool UdpMgr::Load()
{
	TryGetConfig("Test.udp", m_bEnabled);
	if (!m_bEnabled)
	{
		return true;
	}
	TryGetConfig("TestUdp.ip", m_sServerIp);
	TryGetConfig("TestUdp.port", m_sServerPort);
	TryGetConfig("TestUdp.total", m_iTotal);
	TryGetConfig("TestUdp.notes", m_iNotes);
	if (m_sServerPort.empty() || m_iTotal <= 0 || m_iNotes <= 0)
	{
		LOG_ERROR("udp test config invalid port=%s total=%d notes=%d", m_sServerPort.c_str(), m_iTotal, m_iNotes);
		return false;
	}
	m_pNoteSeen.reset(new std::atomic<uint8_t>[(size_t)m_iNotes]);
	for (int32_t iIndex = 0; iIndex < m_iNotes; ++iIndex)
	{
		m_pNoteSeen[iIndex].store(0);
	}
	OI_NOTIFY_BIND(UdpNote, [](const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::UdpNoteReq& stRequest)
	{
		return UdpMgr::GetInstance().HandleNote(stContext, stRequest);
	});
	return IpcUdp::GetInstance().Init() && IpcUdp::GetInstance().StartServer();
}

void UdpMgr::Start()
{
	if (!m_bEnabled)
	{
		return;
	}
	TestBoard::GetInstance().Arm();
	OI::Coro::StartTask([](OI::Coro::Token)
	{
		return UdpMgr::GetInstance().Run();
	});
}

void UdpMgr::OnClose(sid_t iSessionId)
{
	m_iClosedSessionId.store(iSessionId);
}

::OI::Rpc::RpcTask UdpMgr::HandleNote(const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::UdpNoteReq& stRequest)
{
	(void)stContext;
	if (stRequest.iSeq < 0 || stRequest.iSeq >= m_iNotes || m_pNoteSeen[stRequest.iSeq].exchange(1) != 0)
	{
		m_iNoteDuplicate.fetch_add(1);
		co_return OI_OK;
	}
	m_iNoteReceived.fetch_add(1);
	co_return OI_OK;
}

static OI::Coro::TaskResult<bool> EchoOnce(sid_t iSessionId, int32_t iSeq, std::string sData)
{
	::OI::RpcData::UdpEchoReq stRequest;
	stRequest.iSeq = iSeq;
	stRequest.sData = sData;
	::OI::RpcData::UdpEchoRes stResponse;
	const int32_t iRet = co_await ::OI::Rpc::RpcCall(IpcUdp::GetInstance(), iSessionId, stRequest, stResponse, ::OI::Timer::Seconds(3));
	co_return iRet == OI_OK && stResponse.iSeq == iSeq && stResponse.sData == sData;
}

static OI::Coro::TaskResult<std::string> Diagnose(sid_t iSessionId, int32_t iSeq)
{
	::OI::RpcData::UdpEchoReq stRequest;
	stRequest.iSeq = iSeq;
	::OI::RpcData::UdpEchoRes stResponse;
	const int32_t iRet = co_await ::OI::Rpc::RpcCall(IpcUdp::GetInstance(), iSessionId, stRequest, stResponse, ::OI::Timer::Seconds(3));
	co_return iRet == OI_OK ? stResponse.sData : std::string("ret=") + std::to_string(iRet);
}

OI::Coro::Task UdpMgr::Pump(sid_t iSessionId)
{
	for (;;)
	{
		const int32_t iSeq = m_iNext.fetch_add(1);
		if (iSeq >= m_iTotal)
		{
			co_return;
		}
		if (!co_await EchoOnce(iSessionId, iSeq, "udp-" + std::to_string(iSeq)))
		{
			m_iRpcBad.fetch_add(1);
		}
	}
}

OI::Coro::Task UdpMgr::Run()
{
	IpcUdp& stManager = IpcUdp::GetInstance();
	int32_t iBadTotal = 0;
	auto fnReport = [&iBadTotal](const char* sFormat, int32_t iBad, auto... vArgs)
	{
		iBadTotal += iBad;
		std::printf(sFormat, vArgs..., iBad);
		std::fflush(stdout);
	};

	const uint16_t iLocalPort = stManager.LocalPort();
	fnReport("udp local port=%u bad=%d\n", iLocalPort == 0 ? 1 : 0, (unsigned)iLocalPort);

	const sid_t iSessionId = stManager.OpenPeer(m_sServerIp, m_sServerPort);
	const sid_t iAgainId = stManager.OpenPeer(m_sServerIp, m_sServerPort);
	fnReport("udp open dedup bad=%d\n", (iSessionId == INVALID_SID || iSessionId != iAgainId) ? 1 : 0);

	::OI::Net::NetAddr stAddr;
	const bool bAddr = stManager.GetPeerAddr(iSessionId, stAddr);
	fnReport("udp peer addr bad=%d\n", (!bAddr || std::to_string(stAddr.Port()) != m_sServerPort) ? 1 : 0);

	const std::string sSeen = co_await Diagnose(iSessionId, ::OI::RpcData::UDP_PROBE_PEER_ADDR);
	const std::string sExpect = m_sServerIp + ":" + std::to_string(iLocalPort);
	if (sSeen != sExpect)
	{
		std::printf("udp server sees %s expect %s\n", sSeen.c_str(), sExpect.c_str());
	}
	fnReport("udp server sees client port bad=%d\n", sSeen == sExpect ? 0 : 1);

	// 在途数压低，回环口的收包缓冲不会被一次性冲满。
	std::vector<OI::Coro::Task> vWorkers;
	for (int32_t iWorker = 0; iWorker < 16; ++iWorker)
	{
		vWorkers.push_back(Pump(iSessionId));
	}
	co_await OI::Coro::WhenAll(std::move(vWorkers));
	fnReport("udp rpc calls=%d bad=%d\n", m_iRpcBad.load(), m_iTotal);

	for (int32_t iSeq = 0; iSeq < m_iNotes; ++iSeq)
	{
		::OI::RpcData::UdpNoteReq stNote;
		stNote.iSeq = iSeq;
		if (stManager.Notify(iSessionId, stNote) != OI_OK)
		{
			++iBadTotal;
		}
		if (iSeq % 32 == 31)
		{
			co_await OI::Coro::yield(1);
		}
	}
	const auto stNoteDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (m_iNoteReceived.load() < m_iNotes && std::chrono::steady_clock::now() < stNoteDeadline)
	{
		co_await OI::Coro::yield(10);
	}
	const int32_t iReceived = m_iNoteReceived.load();
	const int32_t iDuplicate = m_iNoteDuplicate.load();
	fnReport("udp notify sent=%d received=%d duplicate=%d bad=%d\n", (iReceived != m_iNotes || iDuplicate != 0) ? 1 : 0,
	    m_iNotes, iReceived, iDuplicate);

	// 向仅注册通知处理器的协议发送带调用号的请求，验证处理后的回执。
	::OI::RpcData::UdpNoteReq stWrappedNote;
	stWrappedNote.iSeq = 0;
	::OI::RpcData::UdpNoteRes stWrappedResponse;
	const int32_t iWrappedRet = co_await ::OI::Rpc::RpcCall(stManager, iSessionId, stWrappedNote, stWrappedResponse, ::OI::Timer::Seconds(3));
	fnReport("udp notify rpc receipt=%d bad=%d\n", iWrappedRet == OI_OK ? 0 : 1, iWrappedRet);

	const std::string sRejected = co_await Diagnose(iSessionId, ::OI::RpcData::UDP_PROBE_REJECT);
	const std::string sRejectedExpect = "ret=" + std::to_string((int32_t)::OI::RpcData::UDP_ERR_REJECTED);
	fnReport("udp protocol error code %s bad=%d\n", sRejected == sRejectedExpect ? 0 : 1, sRejected.c_str());

	const bool bLarge = co_await EchoOnce(iSessionId, 1000000, std::string(60000, 'U'));
	fnReport("udp large datagram bytes=%d bad=%d\n", bLarge ? 0 : 1, 60000);

	::OI::RpcData::UdpEchoReq stOversized;
	stOversized.iSeq = 1000003;
	stOversized.sData.assign(65528, 'U');
	const int64_t iOversizedStartMilliseconds = ::OI::Timer::SteadyMilliseconds();
	const int32_t iNotifyRet = stManager.Notify(iSessionId, stOversized);
	::OI::RpcData::UdpEchoRes stOversizedResponse;
	const int32_t iCallRet = co_await ::OI::Rpc::RpcCall(stManager, iSessionId, stOversized, stOversizedResponse, ::OI::Timer::Seconds(3));
	const bool bImmediate = ::OI::Timer::SteadyMilliseconds() - iOversizedStartMilliseconds < 1000;
	const bool bRecovered = co_await EchoOnce(iSessionId, 1000004, "after oversized");
	fnReport("udp oversized notify=%d call=%d immediate=%d recovered=%d bad=%d\n",
	    (iNotifyRet == OI_ERR_INVALID_ARGUMENT && iCallRet == OI_ERR_INVALID_ARGUMENT && bImmediate && bRecovered) ? 0 : 1,
	    iNotifyRet, iCallRet, bImmediate ? 1 : 0, bRecovered ? 1 : 0);

	::OI::Base::Bytes stGarbage;
	stGarbage.Append("garbage-garbage", 15);
	const bool bGarbageSent = stManager.SendBytes(iSessionId, stGarbage);
	const bool bAfter = co_await EchoOnce(iSessionId, 1000001, "after");
	const std::string sSessions = co_await Diagnose(iSessionId, ::OI::RpcData::UDP_PROBE_SESSIONS);
	fnReport("udp garbage keeps session sessions=%s bad=%d\n", (bGarbageSent && bAfter && sSessions == "1") ? 0 : 1, sSessions.c_str());

	const auto stIdleDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(stManager.IdleTimeoutMilliseconds() * 4LL);
	while (m_iClosedSessionId.load() != iSessionId && std::chrono::steady_clock::now() < stIdleDeadline)
	{
		co_await OI::Coro::yield(50);
	}
	::OI::Net::NetAddr stGone;
	const bool bClosed = m_iClosedSessionId.load() == iSessionId && !stManager.GetPeerAddr(iSessionId, stGone);
	const sid_t iReopenId = stManager.OpenPeer(m_sServerIp, m_sServerPort);
	const bool bReopen = iReopenId != INVALID_SID && iReopenId != iSessionId && co_await EchoOnce(iReopenId, 1000002, "reopen");
	if (!bClosed || !bReopen)
	{
		std::printf("udp idle closed=%d reopen=%d sid=%d new=%d\n", bClosed ? 1 : 0, bReopen ? 1 : 0, (int)iSessionId, (int)iReopenId);
	}
	fnReport("udp idle close reopen bad=%d\n", (bClosed && bReopen) ? 0 : 1);

	std::printf("udp done bad=%d\n", iBadTotal);
	std::fflush(stdout);
	TestBoard::GetInstance().Done(iBadTotal == 0 ? 0 : 1);
}
