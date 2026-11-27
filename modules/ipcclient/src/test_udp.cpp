#include "test_udp.h"
#include "oi_config.h"
#include "oi_log.h"
#include "oi_rpcdata_udpechoreq.h"
#include "oi_rpcdata_udpechores.h"
#include "oi_rpcdata_udpnoteres.h"
#include "oi_socket.h"
#include "oi_poll.h"
#include "oi_net_os.h"
#include <cerrno>
#include "rpc_ids.h"
#include "test_board.h"
#include <chrono>
#include <thread>
#include <cstdio>

static ::OI::Rpc::RpcTask UdpSelfEcho(const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::UdpEchoReq& stRequest, ::OI::RpcData::UdpEchoRes& stResponse)
{
	stResponse.iSeq = stRequest.iSeq;
	if (stRequest.iSeq == ::OI::RpcData::UDP_PROBE_PEER_ADDR)
	{
		::OI::Net::NetAddr stAddr;
		if (IpcUdp::GetInstance().GetPeerAddr(stContext.iSessionId, stAddr) != OI_OK)
		{
			co_return OI_ERR_NOT_FOUND;
		}
		stResponse.sData = stAddr.Endpoint();
	}
	else
	{
		stResponse.sData = stRequest.sData;
	}
	co_return OI_OK;
}

class UdpSecondPeer : public ::OI::Rpc::ProtoSessionManager
{
  public:
	std::atomic<int32_t> iMask{0};
	std::atomic<int32_t> iDuplicate{0};
	::OI::Rpc::RpcTask HandleProbe(const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::UdpNoteReq& stRequest)
	{
		(void)stContext;
		if (stRequest.iSeq >= 1000204 && stRequest.iSeq <= 1000207)
		{
			const int32_t iBit = 1 << (stRequest.iSeq - 1000204);
			if ((iMask.fetch_or(iBit) & iBit) != 0)
			{
				++iDuplicate;
			}
		}
		else
		{
			++iDuplicate;
		}
		co_return OI_OK;
	}
};

const std::string IpcUdp::GetIDString()
{
	return "IpcUdp";
}

void IpcUdp::OnAddSession(::OI::Net::Session* pSession)
{
	m_iAddSessionCount.fetch_add(1);
	::OI::Net::NetAddr stAddr;
	m_bAddPeerAddressOk.store(GetPeerAddr(pSession->GetSessionID(), stAddr) == OI_OK && stAddr.Port() != 0);
}

void IpcUdp::OnDelSession(const ::OI::Net::Session* pSession)
{
	TestUdp::GetInstance().OnClose(pSession->GetSessionID());
}

TestUdp::TestUdp()
    : m_bEnabled(false), m_sServerIp("127.0.0.1"), m_iTotal(1000), m_iNotes(1000), m_iNext(0), m_iRpcBad(0),
      m_iClosedSessionId(0), m_iNoteReceived(0), m_iNoteDuplicate(0)
{
}

int32_t TestUdp::Load()
{
	TryGetConfig("Test.udp", m_bEnabled);
	if (!m_bEnabled)
	{
		return OI_OK;
	}
	TryGetConfig("TestUdp.ip", m_sServerIp);
	TryGetConfig("TestUdp.port", m_sServerPort);
	TryGetConfig("TestUdp.total", m_iTotal);
	TryGetConfig("TestUdp.notes", m_iNotes);
	if (m_sServerPort.empty() || m_iTotal <= 0 || m_iNotes <= 0)
	{
		LOG_ERROR("udp test config invalid port=%s total=%d notes=%d", m_sServerPort.c_str(), m_iTotal, m_iNotes);
		return OI_ERR_INVALID_ARGUMENT;
	}
	m_pNoteSeen.reset(new std::atomic<uint8_t>[(size_t)m_iNotes]);
	for (int32_t iIndex = 0; iIndex < m_iNotes; ++iIndex)
	{
		m_pNoteSeen[iIndex].store(0);
	}
	IpcUdp::GetInstance().RegisterRpcHandler(UdpSelfEcho);
	IpcUdp::GetInstance().RegisterNotifyHandler([](const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::UdpNoteReq& stRequest)
	{
		return TestUdp::GetInstance().HandleNote(stContext, stRequest);
	});
	int32_t iCode = IpcUdp::GetInstance().Init();
	if (iCode != OI_OK)
	{
		LOG_ERROR("udp load init failed code=%d", iCode);
		return iCode;
	}
	return OI_OK;
}

void TestUdp::Start()
{
	if (!m_bEnabled)
	{
		return;
	}
	TestBoard::GetInstance().Arm();
	OI::Coro::StartTask([]()
	{
		return TestUdp::GetInstance().Run();
	});
}

void TestUdp::OnClose(sid_t iSessionId)
{
	m_iClosedSessionId.store(iSessionId);
}

::OI::Rpc::RpcTask TestUdp::HandleNote(const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::UdpNoteReq& stRequest)
{
	(void)stContext;
	if (stRequest.iSeq >= 1000204 && stRequest.iSeq <= 1000207)
	{
		const int32_t iBit = 1 << (stRequest.iSeq - 1000204);
		if ((m_iPeerNoteMask.fetch_or(iBit) & iBit) != 0)
		{
			++m_iPeerNoteDuplicate;
		}
		co_return OI_OK;
	}
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
	co_return iRet == OI_OK&& stResponse.iSeq == iSeq&& stResponse.sData == sData;
}

static OI::Coro::TaskResult<std::string> Diagnose(sid_t iSessionId, int32_t iSeq)
{
	::OI::RpcData::UdpEchoReq stRequest;
	stRequest.iSeq = iSeq;
	::OI::RpcData::UdpEchoRes stResponse;
	const int32_t iRet = co_await ::OI::Rpc::RpcCall(IpcUdp::GetInstance(), iSessionId, stRequest, stResponse, ::OI::Timer::Seconds(3));
	co_return iRet == OI_OK ? stResponse.sData : std::string("ret=") + std::to_string(iRet);
}

OI::Coro::Task TestUdp::Pump(sid_t iSessionId)
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

class UdpBudgetProbe : public ::OI::Rpc::ProtoSessionManager
{
  public:
	void SetLimit(size_t iLimit)
	{
		iMaxSendQueueBytes = iLimit;
	}
};

struct UdpQueueProbe
{
	::OI::Rpc::ProtoSessionManager* pManager = nullptr;
	net_socket_t vSockets[2]{INVALID_SOCKET_ID, INVALID_SOCKET_ID};
	sid_t vSessionIds[2]{INVALID_SID, INVALID_SID};
	uint16_t vPorts[2]{};
	sid_t iReopened = INVALID_SID;
	int32_t iOldSend = OI_ERR_FAIL;
	UdpBudgetProbe* pBudget = nullptr;
	sid_t iBudgetSid = INVALID_SID;
	int32_t iBudgetFirst = OI_ERR_FAIL;
	int32_t iBudgetBusy = OI_ERR_FAIL;
	~UdpQueueProbe()
	{
		for (const auto iSocket : vSockets)
		{
			if (iSocket != INVALID_SOCKET_ID)
			{
				NetOs_Close(iSocket);
			}
		}
	}
};

static int32_t UdpQueueSend(::OI::Rpc::ProtoSessionManager& stManager, sid_t iSessionId, const std::string& sPacket)
{
	const ::OI::Base::Bytes stPacket(sPacket.data(), sPacket.size());
	return stManager.SendBytes(iSessionId, stPacket);
}

#ifndef _SINGLE_THREAD_
static void UdpQueueConcurrentSend(UdpQueueProbe* pProbe, int32_t iIndex, std::atomic<int32_t>* pReady, std::atomic<bool>* pStart, std::atomic<int32_t>* pBad)
{
	++*pReady;
	while (!pStart->load())
	{
		std::this_thread::yield();
	}
	const sid_t iSessionId = iIndex == 0 ? pProbe->iReopened : pProbe->vSessionIds[1];
	for (int32_t iPacket = 0; iPacket < 16; ++iPacket)
	{
		const std::string sPacket = "T" + std::to_string(iIndex) + "-" + std::to_string(iPacket);
		if (UdpQueueSend(*pProbe->pManager, iSessionId, sPacket) != OI_OK)
		{
			++*pBad;
		}
	}
}
#endif

struct UdpFrameProbe
{
	UdpQueueProbe* pQueue = nullptr;
	int32_t iMode = 0;
	std::string sRaw;
	int32_t iBad = 0;
};

static int32_t UdpFrameSend(void* pArgument)
{
	auto& stProbe = *static_cast<UdpFrameProbe*>(pArgument);
	auto& stManager = *stProbe.pQueue->pManager;
	const sid_t iFirst = stProbe.pQueue->iReopened;
	const sid_t iSecond = stProbe.pQueue->vSessionIds[1];
	::OI::RpcData::UdpNoteReq stFirst;
	stFirst.iSeq = 700001;
	::OI::RpcData::UdpNoteReq stSecond;
	stSecond.iSeq = 700002;
	::OI::RpcData::UdpEchoReq stLargeFirst;
	stLargeFirst.iSeq = 700003;
	stLargeFirst.sData.assign(700, 'L');
	::OI::RpcData::UdpEchoReq stLargeSecond;
	stLargeSecond.iSeq = 700004;
	stLargeSecond.sData.assign(700, 'M');
	auto fnCheck = [&stProbe](int32_t iCode)
	{
		if (iCode != OI_OK)
		{
			++stProbe.iBad;
		}
	};
	switch (stProbe.iMode)
	{
	case 0:
		fnCheck(stManager.Notify(iFirst, stFirst));
		break;
	case 1:
		fnCheck(stManager.Notify(iFirst, stSecond));
		break;
	case 2:
		fnCheck(stManager.Notify(iFirst, stLargeFirst));
		break;
	case 3:
		fnCheck(stManager.Notify(iFirst, stLargeSecond));
		break;
	case 4:
		fnCheck(stManager.Notify(iFirst, stFirst));
		fnCheck(stManager.Notify(iFirst, stSecond));
		break;
	case 5:
		fnCheck(stManager.Notify(iFirst, stFirst));
		fnCheck(UdpQueueSend(stManager, iFirst, stProbe.sRaw));
		fnCheck(stManager.Notify(iFirst, stSecond));
		break;
	case 6:
		fnCheck(stManager.Notify(iFirst, stFirst));
		fnCheck(stManager.Notify(iSecond, stSecond));
		fnCheck(stManager.Notify(iFirst, stSecond));
		fnCheck(stManager.Notify(iSecond, stFirst));
		break;
	case 7:
		fnCheck(stManager.Notify(iFirst, stLargeFirst));
		fnCheck(stManager.Notify(iFirst, stLargeSecond));
		break;
	default:
		return OI_ERR_INVALID_ARGUMENT;
	}
	return stProbe.iBad == 0 ? OI_OK : OI_ERR_FAIL;
}

static OI::Coro::TaskResult<std::vector<std::string>> UdpFrameReceive(net_socket_t iSocket, size_t iExpected)
{
	std::vector<std::string> vPackets;
	const auto stDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (vPackets.size() < iExpected && std::chrono::steady_clock::now() < stDeadline)
	{
		char vBuffer[65536];
		sockaddr_storage stFrom{};
		socklen_t iLength = sizeof(stFrom);
		const int32_t iRead = NetOs_RecvFrom(iSocket, vBuffer, sizeof(vBuffer), (sockaddr*)&stFrom, &iLength);
		if (iRead > 0)
		{
			vPackets.emplace_back(vBuffer, (size_t)iRead);
		}
		else
		{
			co_await OI::Coro::yield(10);
		}
	}
	co_return vPackets;
}

static OI::Coro::TaskResult<int> CheckUdpFrameMerge(UdpQueueProbe& stQueue)
{
	UdpFrameProbe stProbe;
	stProbe.pQueue = &stQueue;
	std::vector<std::string> vBaseline;
	for (int32_t iMode = 0; iMode < 4; ++iMode)
	{
		stProbe.iMode = iMode;
		const int32_t iSent = ::OI::Net::Poll::GetInstance().StartListener(UdpFrameSend, &stProbe);
		const auto vPackets = co_await UdpFrameReceive(stQueue.vSockets[0], 1);
		if (iSent != OI_OK || vPackets.size() != 1)
		{
			std::fprintf(stderr, "udp merge baseline mode=%d status=%d count=%zu\n", iMode, iSent, vPackets.size());
			co_return 1;
		}
		vBaseline.push_back(vPackets[0]);
	}
	stProbe.iMode = 4;
	const int32_t iMerge = ::OI::Net::Poll::GetInstance().StartListener(UdpFrameSend, &stProbe);
	const auto vMerged = co_await UdpFrameReceive(stQueue.vSockets[0], 1);
	const bool bMerged = iMerge == OI_OK && vMerged == std::vector<std::string>{vBaseline[0] + vBaseline[1]};
	stProbe.iMode = 5;
	stProbe.sRaw = vBaseline[0];
	const int32_t iRaw = ::OI::Net::Poll::GetInstance().StartListener(UdpFrameSend, &stProbe);
	const auto vRaw = co_await UdpFrameReceive(stQueue.vSockets[0], 3);
	const bool bRaw = iRaw == OI_OK && vRaw == std::vector<std::string>{vBaseline[0], vBaseline[0], vBaseline[1]};
	stProbe.iMode = 6;
	const int32_t iPeers = ::OI::Net::Poll::GetInstance().StartListener(UdpFrameSend, &stProbe);
	const auto vFirst = co_await UdpFrameReceive(stQueue.vSockets[0], 1);
	const auto vSecond = co_await UdpFrameReceive(stQueue.vSockets[1], 1);
	const bool bPeers = iPeers == OI_OK && vFirst == std::vector<std::string>{vBaseline[0] + vBaseline[1]} &&
	                    vSecond == std::vector<std::string>{vBaseline[1] + vBaseline[0]};
	stProbe.iMode = 7;
	const int32_t iLarge = ::OI::Net::Poll::GetInstance().StartListener(UdpFrameSend, &stProbe);
	const auto vLarge = co_await UdpFrameReceive(stQueue.vSockets[0], 2);
	const bool bLarge = iLarge == OI_OK && vLarge == std::vector<std::string>{vBaseline[2], vBaseline[3]};
	const int32_t iBad = bMerged && bRaw && bPeers && bLarge ? 0 : 1;
	std::printf("udp merge frames=%d raw=%d peers=%d limit=%d bad=%d\n", bMerged, bRaw, bPeers, bLarge, iBad);
	co_return iBad;
}

static OI::Coro::TaskResult<int> CheckUdpQueue()
{
	static UdpBudgetProbe stManager;
	stManager.DefaultInit("127.0.0.1", "0", ::OI::Net::SESSION_TYPE_UDP);
	const int32_t iStart = co_await stManager.StartClient();
	if (iStart != OI_OK)
	{
		std::fprintf(stderr, "udp queue manager start=%d\n", iStart);
		co_return 1;
	}
	UdpQueueProbe stProbe;
	stProbe.pManager = &stManager;
	::OI::Net::NetAddr stLoopback;
	if (!stLoopback.Init("127.0.0.1", "0"))
	{
		co_return 1;
	}
	for (int32_t iIndex = 0; iIndex < 2; ++iIndex)
	{
		stProbe.vSockets[iIndex] = NetOs_CreateUdp4(true);
		sockaddr_storage stBound{};
		socklen_t iLength = sizeof(stBound);
		::OI::Net::NetAddr stBoundAddress;
		if (stProbe.vSockets[iIndex] == INVALID_SOCKET_ID ||
		    NetOs_BindDatagram(stProbe.vSockets[iIndex], stLoopback.NetAttrPtr(), stLoopback.NetAttrLen()) != 0 ||
		    NetOs_GetSockName(stProbe.vSockets[iIndex], (sockaddr*)&stBound, &iLength) != 0 ||
		    !stBoundAddress.Init((sockaddr*)&stBound, iLength))
		{
			std::fprintf(stderr, "udp queue socket setup failed index=%d errno=%d\n", iIndex, errno);
			co_return 1;
		}
		stProbe.vPorts[iIndex] = stBoundAddress.Port();
		if (stManager.OpenPeer("127.0.0.1", std::to_string(stProbe.vPorts[iIndex]), stProbe.vSessionIds[iIndex]) != OI_OK)
		{
			co_return 1;
		}
	}
	const int32_t iQueued = ::OI::Net::Poll::GetInstance().StartListener([](void* pArgument) -> int32_t
	{
		auto& stLocal = *static_cast<UdpQueueProbe*>(pArgument);
		const int32_t iA1 = UdpQueueSend(*stLocal.pManager, stLocal.vSessionIds[0], "A1");
		const int32_t iB1 = UdpQueueSend(*stLocal.pManager, stLocal.vSessionIds[1], "B1");
		const int32_t iA2 = UdpQueueSend(*stLocal.pManager, stLocal.vSessionIds[0], "A222");
		const int32_t iB2 = UdpQueueSend(*stLocal.pManager, stLocal.vSessionIds[1], "B22222");
		return iA1 == OI_OK && iB1 == OI_OK && iA2 == OI_OK && iB2 == OI_OK ? OI_OK : OI_ERR_FAIL;
	}, &stProbe);
	std::vector<std::string> vReceived[2];
	for (int32_t iWait = 0; iWait < 300 && (vReceived[0].size() < 2 || vReceived[1].size() < 2); ++iWait)
	{
		for (int32_t iIndex = 0; iIndex < 2; ++iIndex)
		{
			char vBuffer[64];
			sockaddr_storage stFrom{};
			socklen_t iLength = sizeof(stFrom);
			const int32_t iRead = NetOs_RecvFrom(stProbe.vSockets[iIndex], vBuffer, sizeof(vBuffer), (sockaddr*)&stFrom, &iLength);
			if (iRead > 0)
			{
				vReceived[iIndex].emplace_back(vBuffer, (size_t)iRead);
			}
		}
		co_await OI::Coro::yield(10);
	}
	const bool bPackets = iQueued == OI_OK && vReceived[0] == std::vector<std::string>{"A1", "A222"} &&
	                      vReceived[1] == std::vector<std::string>{"B1", "B22222"};
	const int32_t iRequeue = ::OI::Net::Poll::GetInstance().StartListener([](void* pArgument) -> int32_t
	{
		auto& stLocal = *static_cast<UdpQueueProbe*>(pArgument);
		const int32_t iQueuedOld = UdpQueueSend(*stLocal.pManager, stLocal.vSessionIds[0], "OLD");
		const int32_t iClosed = stLocal.pManager->CloseSession(stLocal.vSessionIds[0]);
		const int32_t iOpen = stLocal.pManager->OpenPeer("127.0.0.1", std::to_string(stLocal.vPorts[0]), stLocal.iReopened);
		stLocal.iOldSend = UdpQueueSend(*stLocal.pManager, stLocal.vSessionIds[0], "LATE");
		const int32_t iNew = UdpQueueSend(*stLocal.pManager, stLocal.iReopened, "NEW");
		return iQueuedOld == OI_OK && iClosed == OI_OK && iOpen == OI_OK &&
		               stLocal.iReopened != stLocal.vSessionIds[0] && stLocal.iOldSend != OI_OK && iNew == OI_OK
		           ? OI_OK
		           : OI_ERR_FAIL;
	}, &stProbe);
	std::vector<std::string> vAfter;
	const auto stObserveStart = std::chrono::steady_clock::now();
	for (int32_t iWait = 0; iWait < 100; ++iWait)
	{
		char vBuffer[64];
		sockaddr_storage stFrom{};
		socklen_t iLength = sizeof(stFrom);
		const int32_t iRead = NetOs_RecvFrom(stProbe.vSockets[0], vBuffer, sizeof(vBuffer), (sockaddr*)&stFrom, &iLength);
		if (iRead > 0)
		{
			vAfter.emplace_back(vBuffer, (size_t)iRead);
		}
		co_await OI::Coro::yield(10);
	}
	const auto iObserveMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stObserveStart).count();
	const bool bReopen = iRequeue == OI_OK && vAfter == std::vector<std::string>{"NEW"};
	bool bConcurrent = true;
	int32_t iWorkers = 0;
#ifndef _SINGLE_THREAD_
	iWorkers = 2;
	std::atomic<int32_t> iReady{0};
	std::atomic<bool> bStart{false};
	std::atomic<int32_t> iSendBad{0};
	std::thread stFirst(UdpQueueConcurrentSend, &stProbe, 0, &iReady, &bStart, &iSendBad);
	std::thread stSecond(UdpQueueConcurrentSend, &stProbe, 1, &iReady, &bStart, &iSendBad);
	while (iReady.load() != 2)
	{
		std::this_thread::yield();
	}
	bStart.store(true);
	stFirst.join();
	stSecond.join();
	std::vector<std::string> vConcurrent[2];
	for (int32_t iWait = 0; iWait < 300 && (vConcurrent[0].size() < 16 || vConcurrent[1].size() < 16); ++iWait)
	{
		for (int32_t iIndex = 0; iIndex < 2; ++iIndex)
		{
			char vBuffer[64];
			sockaddr_storage stFrom{};
			socklen_t iLength = sizeof(stFrom);
			const int32_t iRead = NetOs_RecvFrom(stProbe.vSockets[iIndex], vBuffer, sizeof(vBuffer), (sockaddr*)&stFrom, &iLength);
			if (iRead > 0)
			{
				vConcurrent[iIndex].emplace_back(vBuffer, (size_t)iRead);
			}
		}
		co_await OI::Coro::yield(10);
	}
	// 继续观察，确保完成计数后没有迟到的重复包。
	for (int32_t iWait = 0; iWait < 10; ++iWait)
	{
		for (int32_t iIndex = 0; iIndex < 2; ++iIndex)
		{
			char vBuffer[64];
			sockaddr_storage stFrom{};
			socklen_t iLength = sizeof(stFrom);
			const int32_t iRead = NetOs_RecvFrom(stProbe.vSockets[iIndex], vBuffer, sizeof(vBuffer), (sockaddr*)&stFrom, &iLength);
			if (iRead > 0)
			{
				vConcurrent[iIndex].emplace_back(vBuffer, (size_t)iRead);
			}
		}
		co_await OI::Coro::yield(10);
	}
	bConcurrent = iSendBad.load() == 0;
	for (int32_t iIndex = 0; iIndex < 2; ++iIndex)
	{
		std::vector<std::string> vExpected;
		for (int32_t iPacket = 0; iPacket < 16; ++iPacket)
		{
			vExpected.push_back("T" + std::to_string(iIndex) + "-" + std::to_string(iPacket));
		}
		if (vConcurrent[iIndex] != vExpected)
		{
			std::fprintf(stderr, "udp queue concurrent index=%d count=%zu send_bad=%d received=", iIndex, vConcurrent[iIndex].size(), iSendBad.load());
			for (const auto& sPacket : vConcurrent[iIndex])
			{
				std::fprintf(stderr, "%s,", sPacket.c_str());
			}
			std::fprintf(stderr, "\n");
			bConcurrent = false;
		}
	}
#endif
	const int32_t iMergeBad = co_await CheckUdpFrameMerge(stProbe);
	static UdpBudgetProbe stBudget;
	stBudget.DefaultInit("127.0.0.1", "0", ::OI::Net::SESSION_TYPE_UDP);
	stBudget.SetLimit(4);
	const int32_t iBudgetStart = co_await stBudget.StartClient();
	stProbe.pBudget = &stBudget;
	const int32_t iBudgetOpen = iBudgetStart == OI_OK ? stBudget.OpenPeer("127.0.0.1", std::to_string(stProbe.vPorts[0]), stProbe.iBudgetSid) : iBudgetStart;
	int32_t iBudgetQueued = OI_ERR_FAIL;
	if (iBudgetOpen == OI_OK)
	{
		iBudgetQueued = ::OI::Net::Poll::GetInstance().StartListener([](void* pArgument) -> int32_t
		{
			auto& stLocal = *static_cast<UdpQueueProbe*>(pArgument);
			stLocal.iBudgetFirst = UdpQueueSend(*stLocal.pBudget, stLocal.iBudgetSid, "FULL");
			stLocal.iBudgetBusy = UdpQueueSend(*stLocal.pBudget, stLocal.iBudgetSid, "X");
			return OI_OK;
		}, &stProbe);
	}
	std::vector<std::string> vBudgetPackets;
	for (int32_t iWait = 0; iWait < 300 && vBudgetPackets.empty(); ++iWait)
	{
		char vBuffer[64];
		sockaddr_storage stFrom{};
		socklen_t iLength = sizeof(stFrom);
		const int32_t iRead = NetOs_RecvFrom(stProbe.vSockets[0], vBuffer, sizeof(vBuffer), (sockaddr*)&stFrom, &iLength);
		if (iRead > 0)
		{
			vBudgetPackets.emplace_back(vBuffer, (size_t)iRead);
		}
		co_await OI::Coro::yield(10);
	}
	const int32_t iBudgetAfter = iBudgetOpen == OI_OK ? UdpQueueSend(stBudget, stProbe.iBudgetSid, "OK") : OI_ERR_FAIL;
	for (int32_t iWait = 0; iWait < 100; ++iWait)
	{
		char vBuffer[64];
		sockaddr_storage stFrom{};
		socklen_t iLength = sizeof(stFrom);
		const int32_t iRead = NetOs_RecvFrom(stProbe.vSockets[0], vBuffer, sizeof(vBuffer), (sockaddr*)&stFrom, &iLength);
		if (iRead > 0)
		{
			vBudgetPackets.emplace_back(vBuffer, (size_t)iRead);
		}
		co_await OI::Coro::yield(10);
	}
	const bool bBudget = iBudgetQueued == OI_OK && stProbe.iBudgetFirst == OI_OK && stProbe.iBudgetBusy == OI_ERR_BUSY &&
	                     iBudgetAfter == OI_OK && vBudgetPackets == std::vector<std::string>{"FULL", "OK"};
	std::printf("udp queue budget first=%d busy=%d after=%d packets=%zu bad=%d\n", stProbe.iBudgetFirst,
	    stProbe.iBudgetBusy, iBudgetAfter, vBudgetPackets.size(), bBudget ? 0 : 1);
	// IPv4 数据报上限：65507 收下整包，65508 在入队前拒绝。
	const std::string sMaxDatagram(65507, 'M');
	const std::string sOverDatagram(65508, 'O');
	const int32_t iMaxSend = UdpQueueSend(stManager, stProbe.iReopened, sMaxDatagram);
	const int32_t iOverSend = UdpQueueSend(stManager, stProbe.iReopened, sOverDatagram);
	int32_t iMaxReceived = 0;
	std::vector<char> vMaxBuffer(65536);
	for (int32_t iWait = 0; iWait < 300 && iMaxReceived == 0; ++iWait)
	{
		sockaddr_storage stFrom{};
		socklen_t iLength = sizeof(stFrom);
		const int32_t iRead = NetOs_RecvFrom(stProbe.vSockets[0], vMaxBuffer.data(), vMaxBuffer.size(), (sockaddr*)&stFrom, &iLength);
		if (iRead > 0)
		{
			iMaxReceived = iRead;
		}
		co_await OI::Coro::yield(10);
	}
	const bool bMaxDatagram = iMaxSend == OI_OK && iOverSend == OI_ERR_INVALID_ARGUMENT && iMaxReceived == 65507;
	if (!bMaxDatagram)
	{
		std::fprintf(stderr, "udp datagram max=%d over=%d received=%d\n", iMaxSend, iOverSend, iMaxReceived);
	}
	std::printf("udp datagram max=%d over=%d received=%d bad=%d\n", iMaxSend, iOverSend, iMaxReceived, bMaxDatagram ? 0 : 1);
	const int32_t iBad = bPackets && bReopen && bConcurrent && bBudget && bMaxDatagram && iMergeBad == 0 ? 0 : 1;
	std::printf("udp queue packets=%d reopen=%d concurrent=%d workers=%d queued=%d requeue=%d old=%d count=%zu/%zu after=%zu observe_ms=%lld bad=%d\n",
	    bPackets ? 1 : 0, bReopen ? 1 : 0, bConcurrent ? 1 : 0, iWorkers, iQueued, iRequeue, stProbe.iOldSend,
	    vReceived[0].size(), vReceived[1].size(), vAfter.size(), (long long)iObserveMilliseconds, iBad);
	co_return iBad;
}

OI::Coro::Task TestUdp::Run()
{
	IpcUdp& stManager = IpcUdp::GetInstance();
	int32_t iBadTotal = 0;
	auto fnReport = [&iBadTotal](const char* sFormat, int32_t iBad, auto... vArgs)
	{
		iBadTotal += iBad;
		std::printf(sFormat, vArgs..., iBad);
		std::fflush(stdout);
	};

	sid_t iBeforeBind = 12345;
	const int32_t iBeforeCode = stManager.OpenPeer(m_sServerIp, m_sServerPort, iBeforeBind);
	const int32_t iStartCode = co_await stManager.StartClient();
	const int32_t iAddCount = stManager.AddSessionCount();
	const bool bBound = iBeforeCode == OI_ERR_CONNECT && iBeforeBind == 12345 && iStartCode == OI_OK && iAddCount == 0;
	fnReport("udp client bind before=%d output=%d start=%d sessions=%d bad=%d\n", bBound ? 0 : 1,
	    iBeforeCode, (int)iBeforeBind, iStartCode, iAddCount);
	if (!bBound)
	{
		TestBoard::GetInstance().Done(1);
		co_return;
	}

	const uint16_t iLocalPort = stManager.LocalPort();
	fnReport("udp local port=%u bad=%d\n", iLocalPort == 0 ? 1 : 0, (unsigned)iLocalPort);

	sid_t iSessionId = INVALID_SID;
	sid_t iAgainId = INVALID_SID;
	const int32_t iOpen = stManager.OpenPeer(m_sServerIp, m_sServerPort, iSessionId);
	const int32_t iAgain = stManager.OpenPeer(m_sServerIp, m_sServerPort, iAgainId);
	fnReport("udp open dedup bad=%d\n", (iOpen != OI_OK || iAgain != OI_OK || iSessionId == INVALID_SID || iSessionId != iAgainId) ? 1 : 0);

	fnReport("udp add peer addr bad=%d\n", stManager.AddPeerAddressOk() ? 0 : 1);
	::OI::Net::NetAddr stAddr;
	const bool bAddr = stManager.GetPeerAddr(iSessionId, stAddr) == OI_OK;
	fnReport("udp peer addr bad=%d\n", (!bAddr || std::to_string(stAddr.Port()) != m_sServerPort) ? 1 : 0);

	const std::string sSeen = co_await Diagnose(iSessionId, ::OI::RpcData::UDP_PROBE_PEER_ADDR);
	const std::string sExpect = m_sServerIp + ":" + std::to_string(iLocalPort);
	if (sSeen != sExpect)
	{
		std::printf("udp server sees %s expect %s\n", sSeen.c_str(), sExpect.c_str());
	}
	fnReport("udp server sees client port bad=%d\n", sSeen == sExpect ? 0 : 1);

	// 同一个本地 UDP 口同时与服务端和自身通信，关闭其中一个对端后另一个仍可收发。
	sid_t iSelfId = INVALID_SID;
	const int32_t iSelfOpen = stManager.OpenPeer("127.0.0.1", std::to_string(iLocalPort), iSelfId);
	bool bBoth = false;
	bool bSource = false;
	bool bOtherAfterClose = false;
	bool bSelfReopen = false;
	if (iSelfOpen == OI_OK && iSelfId != INVALID_SID && iSelfId != iSessionId)
	{
		auto stServerEcho = EchoOnce(iSessionId, 1000200, "server parallel");
		auto stSelfEcho = EchoOnce(iSelfId, 1000201, "self parallel");
		const bool bServerEcho = co_await stServerEcho;
		const bool bSelfEcho = co_await stSelfEcho;
		bBoth = bServerEcho && bSelfEcho;
		const std::string sSelfSeen = co_await Diagnose(iSelfId, ::OI::RpcData::UDP_PROBE_PEER_ADDR);
		bSource = sSelfSeen == sExpect;
		const int32_t iClose = stManager.CloseSession(iSelfId);
		bOtherAfterClose = iClose == OI_OK && co_await EchoOnce(iSessionId, 1000202, "server after close");
		sid_t iSelfReopen = INVALID_SID;
		const int32_t iReopenSelf = stManager.OpenPeer("127.0.0.1", std::to_string(iLocalPort), iSelfReopen);
		if (iReopenSelf == OI_OK && iSelfReopen != INVALID_SID && iSelfReopen != iSelfId)
		{
			bSelfReopen = co_await EchoOnce(iSelfReopen, 1000203, "self reopen");
			stManager.CloseSession(iSelfReopen);
		}
	}
	fnReport("udp peers parallel=%d source=%d remaining=%d reopen=%d bad=%d\n",
	    bBoth && bSource && bOtherAfterClose && bSelfReopen ? 0 : 1,
	    bBoth ? 1 : 0, bSource ? 1 : 0, bOtherAfterClose ? 1 : 0, bSelfReopen ? 1 : 0);

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

	static ::OI::Rpc::ProtoSessionManager stIpv6;
	stIpv6.DefaultInit("::1", "0", ::OI::Net::SESSION_TYPE_UDP);
	const int32_t iIpv6Start = stIpv6.StartServer();
	sid_t iIpv6Peer = INVALID_SID;
	int32_t iIpv6Open = OI_ERR_FAIL;
	int32_t iIpv6Oversized = OI_ERR_FAIL;
	if (iIpv6Start == OI_OK)
	{
		iIpv6Open = stIpv6.OpenPeer("::1", std::to_string(stIpv6.LocalPort()), iIpv6Peer);
		if (iIpv6Open == OI_OK)
		{
			const std::string sIpv6Large(65528, 'V');
			const ::OI::Base::Bytes stIpv6Large(sIpv6Large.data(), sIpv6Large.size());
			iIpv6Oversized = stIpv6.SendBytes(iIpv6Peer, stIpv6Large);
		}
	}
	fnReport("udp ipv6 oversized start=%d open=%d send=%d bad=%d\n",
	    iIpv6Start == OI_OK && iIpv6Open == OI_OK && iIpv6Oversized == OI_ERR_INVALID_ARGUMENT ? 0 : 1,
	    iIpv6Start, iIpv6Open, iIpv6Oversized);

	::OI::Base::Bytes stGarbage;
	stGarbage.Append("garbage-garbage", 15);
	const bool bGarbageSent = stManager.SendBytes(iSessionId, stGarbage) == OI_OK;
	const bool bAfter = co_await EchoOnce(iSessionId, 1000001, "after");
	const std::string sSessions = co_await Diagnose(iSessionId, ::OI::RpcData::UDP_PROBE_SESSIONS);
	fnReport("udp garbage keeps session sessions=%s bad=%d\n", (bGarbageSent && bAfter && sSessions == "1") ? 0 : 1, sSessions.c_str());

	bool bCheckServerIdle = false;
	TryGetConfig("TestUdp.check_server_idle", bCheckServerIdle);
	if (bCheckServerIdle)
	{
		// 持续收发超过服务端空闲时限，服务端不应重复创建会话。
		bool bActive = true;
		for (int32_t iRound = 0; iRound < 8; ++iRound)
		{
			co_await OI::Coro::yield(250);
			if (!co_await EchoOnce(iSessionId, 1000100 + iRound, "active"))
			{
				bActive = false;
			}
		}
		const std::string sActiveSessions = co_await Diagnose(iSessionId, ::OI::RpcData::UDP_PROBE_SESSIONS);
		fnReport("udp active sessions=%s bad=%d\n", bActive && sActiveSessions == "1" ? 0 : 1, sActiveSessions.c_str());

		// 保持客户端会话，等待服务端先回收，再由下一次收发触发服务端重建。
		co_await OI::Coro::yield(2000);
		::OI::Net::NetAddr stStillOpen;
		const bool bClientOpen = stManager.GetPeerAddr(iSessionId, stStillOpen) == OI_OK;
		const std::string sReopenedSessions = co_await Diagnose(iSessionId, ::OI::RpcData::UDP_PROBE_SESSIONS);
		fnReport("udp server idle client_open=%d sessions=%s bad=%d\n",
		    bClientOpen && sReopenedSessions == "2" ? 0 : 1, bClientOpen ? 1 : 0, sReopenedSessions.c_str());
	}

	const auto stIdleDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(stManager.IdleTimeoutMilliseconds() * 4LL);
	while (m_iClosedSessionId.load() != iSessionId && std::chrono::steady_clock::now() < stIdleDeadline)
	{
		co_await OI::Coro::yield(50);
	}
	::OI::Net::NetAddr stGone;
	const bool bClosed = m_iClosedSessionId.load() == iSessionId && stManager.GetPeerAddr(iSessionId, stGone) != OI_OK;
	sid_t iReopenId = INVALID_SID;
	const int32_t iReopen = stManager.OpenPeer(m_sServerIp, m_sServerPort, iReopenId);
	const bool bReopen = iReopen == OI_OK && iReopenId != INVALID_SID && iReopenId != iSessionId && co_await EchoOnce(iReopenId, 1000002, "reopen");
	if (!bClosed || !bReopen)
	{
		std::printf("udp idle closed=%d reopen=%d sid=%d new=%d\n", bClosed ? 1 : 0, bReopen ? 1 : 0, (int)iSessionId, (int)iReopenId);
	}
	fnReport("udp idle close reopen bad=%d\n", (bClosed && bReopen) ? 0 : 1);

	::OI::Base::Bytes stOldPacket("old", 3);
	const int32_t iOldSend = stManager.SendBytes(iSessionId, stOldPacket);
	static UdpSecondPeer stSecond;
	stSecond.DefaultInit("127.0.0.1", "0", ::OI::Net::SESSION_TYPE_UDP);
	stSecond.RegisterNotifyHandler(&UdpSecondPeer::HandleProbe);
	const int32_t iSecondStart = co_await stSecond.StartClient();
	sid_t iSecondId = INVALID_SID;
	const int32_t iSecondOpen = iSecondStart == OI_OK ? stSecond.OpenPeer(m_sServerIp, m_sServerPort, iSecondId) : iSecondStart;
	bool bSends = iSecondOpen == OI_OK && stSecond.LocalPort() != 0 && stSecond.LocalPort() != iLocalPort;
	if (bSends && bReopen)
	{
		::OI::RpcData::UdpNoteReq stProbe;
		stProbe.iSeq = 1000204;
		bSends = stManager.Notify(iReopenId, stProbe) == OI_OK && bSends;
		stProbe.iSeq = 1000205;
		bSends = stSecond.Notify(iSecondId, stProbe) == OI_OK && bSends;
		for (int32_t iWait = 0; iWait < 300 && (m_iPeerNoteMask.load() != 1 || stSecond.iMask.load() != 2); ++iWait)
		{
			co_await OI::Coro::yield(10);
		}
		stProbe.iSeq = 1000206;
		bSends = stSecond.Notify(iSecondId, stProbe) == OI_OK && bSends;
		stProbe.iSeq = 1000207;
		bSends = stManager.Notify(iReopenId, stProbe) == OI_OK && bSends;
		for (int32_t iWait = 0; iWait < 300 && (m_iPeerNoteMask.load() != 9 || stSecond.iMask.load() != 6); ++iWait)
		{
			co_await OI::Coro::yield(10);
		}
	}
	const bool bPeerIsolation = bSends && bReopen && m_iPeerNoteMask.load() == 9 && stSecond.iMask.load() == 6 &&
	                            m_iPeerNoteDuplicate.load() == 0 && stSecond.iDuplicate.load() == 0 && iOldSend != OI_OK;
	fnReport("udp reverse peers first=%d second=%d dup=%d/%d old=%d bad=%d\n", bPeerIsolation ? 0 : 1,
	    m_iPeerNoteMask.load(), stSecond.iMask.load(), m_iPeerNoteDuplicate.load(), stSecond.iDuplicate.load(), iOldSend);

	iBadTotal += co_await CheckUdpQueue();

	std::printf("udp done bad=%d\n", iBadTotal);
	std::fflush(stdout);
	TestBoard::GetInstance().Done(iBadTotal == 0 ? 0 : 1);
}
