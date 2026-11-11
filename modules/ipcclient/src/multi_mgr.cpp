#include "multi_mgr.h"
#include "oi_config.h"
#include "oi_poll.h"
#include "oi_rpcdata_peerprobereq.h"
#include "oi_rpcdata_peerproberes.h"
#include "rpc_ids.h"
#include "test_board.h"
#include <cstdio>
#include <functional>
#include <vector>

class ListWatchProbe : public ::OI::Net::SessionManager
{
  public:
	std::vector<::OI::Base::ServiceInstance> vList;
	int32_t iUpdates = 0;
	void ApplyServiceList(const std::vector<::OI::Base::ServiceInstance>& vNewList) override
	{
		vList = vNewList;
		++iUpdates;
	}
	::OI::Net::Session* CloneSession() override
	{
		return nullptr;
	}
	void OnAddSession(::OI::Net::Session*) override
	{
	}
	void OnDelSession(const ::OI::Net::Session*) override
	{
	}
};

MultiClient::MultiClient(std::string sName)
    : m_sName(std::move(sName))
{
}

const std::string MultiClient::GetIDString()
{
	return m_sName;
}

void MultiClient::OnPeerConnect(int32_t iInstanceId, sid_t iSessionId)
{
	(void)iSessionId;
	::OI::Thread::MutexGuard stLock(m_stCountLocker);
	++m_mConnects[iInstanceId];
}

void MultiClient::OnPeerDisconnect(int32_t iInstanceId, sid_t iSessionId)
{
	(void)iSessionId;
	::OI::Thread::MutexGuard stLock(m_stCountLocker);
	++m_mDisconnects[iInstanceId];
}

int32_t MultiClient::Connects(int32_t iInstanceId)
{
	::OI::Thread::MutexGuard stLock(m_stCountLocker);
	auto stIterator = m_mConnects.find(iInstanceId);
	return stIterator == m_mConnects.end() ? 0 : stIterator->second;
}

int32_t MultiClient::Disconnects(int32_t iInstanceId)
{
	::OI::Thread::MutexGuard stLock(m_stCountLocker);
	auto stIterator = m_mDisconnects.find(iInstanceId);
	return stIterator == m_mDisconnects.end() ? 0 : stIterator->second;
}

// 推送得从这台自己的连接、进这个管理器。注册表实例号和服务端 [Service] id 相同。
::OI::Rpc::RpcTask MultiClient::HandleNote(const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::PeerNoteReq& stRequest)
{
	const int32_t iInstanceId = m_sName == "IpcMultiFixed" ? 0 : stRequest.iServiceId;
	if (&stContext.stManager != this || stContext.iSessionId == INVALID_SID || stContext.iSessionId != ConnectedSid(iInstanceId))
	{
		m_iNoteBad.fetch_add(1);
		co_return OI_OK;
	}
	m_iNotes.fetch_add(1);
	co_return OI_OK;
}

MultiMgr::MultiMgr()
    : m_bEnabled(false), m_stFixed("IpcMultiFixed"), m_stPeers("IpcMulti")
{
}

bool MultiMgr::Load()
{
	TryGetConfig("Test.multi", m_bEnabled);
	if (!m_bEnabled)
	{
		return true;
	}
	OI_NOTIFY_BIND_SELF(PeerNote, MultiClient, HandleNote);
	return m_stFixed.Init() && m_stPeers.Init();
}

void MultiMgr::Start()
{
	if (!m_bEnabled)
	{
		return;
	}
	TestBoard::GetInstance().Arm();
	OI::Coro::StartTask([](OI::Coro::Token)
	{
		return MultiMgr::GetInstance().Run();
	});
}

OI::Coro::TaskResult<int32_t> MultiMgr::ProbeOnce(MultiClient& stClient, int32_t iInstanceId, int32_t iTag, int32_t iNotify)
{
	::OI::RpcData::PeerProbeReq stRequest;
	stRequest.iTag = iTag;
	stRequest.iNotify = iNotify;
	::OI::RpcData::PeerProbeRes stResponse;
	const int32_t iRet = co_await stClient.Call(iInstanceId, stRequest, stResponse, ::OI::Timer::Seconds(3));
	if (iRet != OI_OK)
	{
		co_return iRet;
	}
	co_return stResponse.iTag == iTag ? stResponse.iServiceId : OI_ERR_FAIL;
}

OI::Coro::TaskResult<bool> MultiMgr::WaitFor(std::function<bool()> fnReady, int64_t iMilliseconds)
{
	const int64_t iDeadlineMilliseconds = ::OI::Timer::SteadyMilliseconds() + iMilliseconds;
	while (!fnReady())
	{
		if (::OI::Timer::SteadyMilliseconds() >= iDeadlineMilliseconds)
		{
			co_return false;
		}
		co_await OI::Coro::yield(20);
	}
	co_return true;
}

std::string MultiMgr::JoinIds(const std::vector<int32_t>& vIds)
{
	std::string sText;
	for (int32_t iId : vIds)
	{
		if (!sText.empty())
		{
			sText += ",";
		}
		sText += std::to_string(iId);
	}
	return sText.empty() ? std::string("none") : sText;
}

OI::Coro::TaskResult<int32_t> MultiMgr::SwapFixed()
{
	// 走注册表的不能改地址。
	const bool bReject = !m_stPeers.UseAddress("127.0.0.1", "1") && !m_stFixed.UseAddress("", "1");

	struct Found
	{
		std::string sIp;
		std::string sPort;
		std::atomic<bool> bDone{false};
	};
	auto pFound = std::make_shared<Found>();
	::OI::Base::RegistryClient::GetInstance().PullAsync("peerserver", [pFound](int32_t iCode, const std::vector<::OI::Base::ServiceInstance>& vList)
	{
		for (const auto& stInst : vList)
		{
			if (iCode == OI_OK && stInst.iId == 2)
			{
				pFound->sIp = stInst.sIp;
				pFound->sPort = stInst.sPort;
			}
		}
		pFound->bDone.store(true, std::memory_order_release);
	});
	const bool bPulled = co_await WaitFor([pFound]()
	{
		return pFound->bDone.load(std::memory_order_acquire);
	}, 3000);
	const bool bListed = bPulled && !pFound->sPort.empty();

	const std::string sOldIp = m_stFixed.GetStrIP();
	const std::string sOldPort = m_stFixed.GetStrPort();
	int32_t iTwo = OI_ERR_FAIL;
	bool bSwapped = false;
	if (bListed)
	{
		const int32_t iConnects = m_stFixed.Connects(0);
		const int32_t iDisconnects = m_stFixed.Disconnects(0);
		bSwapped = m_stFixed.UseAddress(pFound->sIp, pFound->sPort) && m_stFixed.GetStrPort() == pFound->sPort;
		bSwapped = (co_await WaitFor([this, iConnects, iDisconnects]()
		{
			return m_stFixed.Connects(0) > iConnects && m_stFixed.Disconnects(0) > iDisconnects;
		}, 5000)) &&
		           bSwapped;
		iTwo = co_await ProbeOnce(m_stFixed, 0, 61, 0);
	}

	const int32_t iConnects = m_stFixed.Connects(0);
	const bool bBackCall = m_stFixed.UseAddress(sOldIp, sOldPort);
	const bool bBackUp = co_await WaitFor([this, iConnects]()
	{
		return m_stFixed.Connects(0) > iConnects;
	}, 5000);
	const int32_t iBack = co_await ProbeOnce(m_stFixed, 0, 62, 0);

	// 地址没变：不断开，照常能打。
	const int32_t iDisconnects = m_stFixed.Disconnects(0);
	const bool bSameCall = m_stFixed.UseAddress(sOldIp, sOldPort);
	const int32_t iSame = co_await ProbeOnce(m_stFixed, 0, 63, 0);
	const bool bSame = bSameCall && iSame == 1 && m_stFixed.Disconnects(0) == iDisconnects;

	const bool bConnected = m_stFixed.IsConnect();
	const int32_t iBad = (bReject && bListed && bSwapped && iTwo == 2 && bBackCall && bBackUp && iBack == 1 && bSame && bConnected) ? 0 : 1;
	std::printf("multi fixed swap reject=%d two=%d back=%d same=%d connected=%d bad=%d\n", bReject ? 1 : 0, iTwo, iBack, bSame ? 1 : 0,
	    bConnected ? 1 : 0, iBad);
	std::fflush(stdout);
	co_return iBad;
}

OI::Coro::Task MultiMgr::Run()
{
	int32_t iBadTotal = 0;
	auto fnReport = [&iBadTotal](const char* sFormat, int32_t iBad, auto... vArgs)
	{
		iBadTotal += iBad;
		std::printf(sFormat, vArgs..., iBad);
		std::fflush(stdout);
	};

	// 第二个观察者初次拉取失败后，下一次成功拉取即使名单没变也要收到完整名单。
	struct WatchResult
	{
		std::atomic<bool> bDone{false};
		bool bRecovered = false;
	};
	static WatchResult stWatchResult;
	auto* pWatchResult = &stWatchResult;
	::OI::Net::Poll::GetInstance().CallOnLoop([](void* pArgument)
	{
		auto* pResult = static_cast<WatchResult*>(pArgument);
		static ListWatchProbe stFirstWatcher;
		static ListWatchProbe stSecondWatcher;
		::OI::Base::ServiceInstance stInstance;
		stInstance.sName = "multi-watch-probe";
		stInstance.iId = 17;
		stInstance.sIp = "127.0.0.1";
		stInstance.sPort = "12345";
		const std::vector<::OI::Base::ServiceInstance> vWatchList{stInstance};
		auto& stTable = ::OI::Base::ServiceTable::GetInstance();
		stTable.WatchList(&stFirstWatcher, stInstance.sName, vWatchList);
		stTable.Apply({stInstance.sName}, vWatchList);
		const int32_t iFirstUpdates = stFirstWatcher.iUpdates;
		stTable.WatchList(&stSecondWatcher, stInstance.sName, {});
		stTable.Apply({stInstance.sName}, vWatchList);
		pResult->bRecovered = stFirstWatcher.iUpdates == iFirstUpdates && stSecondWatcher.iUpdates == 1 &&
		                      stSecondWatcher.vList.size() == 1 && stSecondWatcher.vList[0].iId == 17;
		pResult->bDone.store(true, std::memory_order_release);
	}, pWatchResult);
	const bool bWatchDone = co_await WaitFor([pWatchResult]()
	{
		return pWatchResult->bDone.load(std::memory_order_acquire);
	}, 5000);
	const bool bWatchRecovered = bWatchDone && pWatchResult->bRecovered;
	fnReport("multi watcher unchanged recovered=%d bad=%d\n", bWatchRecovered ? 0 : 1, bWatchRecovered ? 1 : 0);

	// 写死地址：一台，实例号 0。没连上就发的调用先暂存，连上补发。
	const auto stWaitBaseline = ::OI::Rpc::GetRpcWaitStats();
	const bool bNotConnected = m_stFixed.ConnectedSid(0) == INVALID_SID;
	::OI::RpcData::PeerProbeReq stEarlyNotify;
	stEarlyNotify.iTag = 901;
	stEarlyNotify.iNotify = 1;
	const int32_t iEarlyNotify = m_stFixed.NotifyPeer(0, stEarlyNotify);
	m_stFixed.Start();
	const bool bEarlyDelivered = co_await WaitFor([this]()
	{
		return m_stFixed.Notes() == 1;
	}, 5000);
	fnReport("multi notify early accepted=%d delivered=%d bad=%d\n", bNotConnected && iEarlyNotify == OI_OK && bEarlyDelivered && m_stFixed.NoteBad() == 0 ? 0 : 1,
	    iEarlyNotify, bEarlyDelivered ? 1 : 0);
	const sid_t iOldSid = m_stFixed.ConnectedSid(0);
	m_stFixed.CloseSession(iOldSid);
	const bool bDisconnected = co_await WaitFor([this]()
	{
		return m_stFixed.Disconnects(0) >= 1;
	}, 5000);
	const int32_t iReconnectNotify = m_stFixed.NotifyAny(stEarlyNotify);
	const bool bReconnectDelivered = co_await WaitFor([this]()
	{
		return m_stFixed.Notes() == 2;
	}, 5000);
	const bool bDrained = co_await WaitFor([stWaitBaseline]()
	{
		const auto stNow = ::OI::Rpc::GetRpcWaitStats();
		return stNow.iCalls == stWaitBaseline.iCalls && stNow.iExpirations == stWaitBaseline.iExpirations &&
		       stNow.iSessionEntries == stWaitBaseline.iSessionEntries;
	}, 5000);
	fnReport("multi notify reconnect disconnected=%d accepted=%d delivered=%d drained=%d bad=%d\n",
	    bDisconnected && iReconnectNotify == OI_OK && bReconnectDelivered && bDrained && m_stFixed.NoteBad() == 0 ? 0 : 1,
	    bDisconnected ? 1 : 0, iReconnectNotify, bReconnectDelivered ? 1 : 0, bDrained ? 1 : 0);
	const int32_t iFixedEarly = co_await ProbeOnce(m_stFixed, 0, 1, 0);
	const bool bFixedUp = co_await WaitFor([this]()
	{
		return m_stFixed.Connects(0) >= 1;
	}, 5000);
	const std::vector<int32_t> vFixedIds = m_stFixed.InstanceIds();
	const int32_t iFixedMissing = co_await ProbeOnce(m_stFixed, 1, 2, 0);
	fnReport("multi fixed early=%d connected=%d ids=%s missing=%d bad=%d\n",
	    (iFixedEarly == 1 && bFixedUp && vFixedIds == std::vector<int32_t>{0} && iFixedMissing == OI_ERR_CONNECT) ? 0 : 1, iFixedEarly,
	    bFixedUp ? 1 : 0, JoinIds(vFixedIds).c_str(), iFixedMissing);

	// 注册表：Init 时拿到的名单，Start 后每台各连一条。
	const std::vector<int32_t> vSeedIds = m_stPeers.InstanceIds();
	m_stPeers.Start();
	const bool bPeersUp = co_await WaitFor([this]()
	{
		return m_stPeers.Connects(1) >= 1 && m_stPeers.Connects(2) >= 1;
	}, 10000);
	const bool bSidsDiffer = m_stPeers.ConnectedSid(1) != INVALID_SID && m_stPeers.ConnectedSid(2) != INVALID_SID && m_stPeers.ConnectedSid(1) != m_stPeers.ConnectedSid(2);
	fnReport("multi registry seed=%s connected=%d bad=%d\n", (vSeedIds == std::vector<int32_t>{1, 2} && bPeersUp && bSidsDiffer) ? 0 : 1,
	    JoinIds(vSeedIds).c_str(), bPeersUp ? 1 : 0);

	const int32_t iOne = co_await ProbeOnce(m_stPeers, 1, 11, 0);
	const int32_t iTwo = co_await ProbeOnce(m_stPeers, 2, 12, 0);
	const int32_t iNone = co_await ProbeOnce(m_stPeers, 9, 13, 0);
	fnReport("multi call by instance one=%d two=%d none=%d bad=%d\n", (iOne == 1 && iTwo == 2 && iNone == OI_ERR_CONNECT) ? 0 : 1, iOne, iTwo,
	    iNone);

	std::map<int32_t, int32_t> mHits;
	int32_t iAnyBad = 0;
	for (int32_t iIndex = 0; iIndex < 20; ++iIndex)
	{
		::OI::RpcData::PeerProbeReq stRequest;
		stRequest.iTag = 20 + iIndex;
		::OI::RpcData::PeerProbeRes stResponse;
		const int32_t iRet = co_await m_stPeers.CallAny(stRequest, stResponse, ::OI::Timer::Seconds(3));
		if (iRet != OI_OK || stResponse.iTag != stRequest.iTag)
		{
			++iAnyBad;
			continue;
		}
		++mHits[stResponse.iServiceId];
	}
	fnReport("multi call any one=%d two=%d failed=%d bad=%d\n", (iAnyBad == 0 && mHits.size() == 2 && mHits[1] > 0 && mHits[2] > 0) ? 0 : 1,
	    mHits[1], mHits[2], iAnyBad);

	::OI::RpcData::PeerProbeReq stAllRequest;
	stAllRequest.iTag = 40;
	auto vAll = co_await m_stPeers.CallAll<::OI::RpcData::PeerProbeRes>(stAllRequest, ::OI::Timer::Seconds(3));
	bool bAllOk = vAll.size() == 2;
	for (size_t iIndex = 0; bAllOk && iIndex < vAll.size(); ++iIndex)
	{
		const auto& stPeer = vAll[iIndex];
		bAllOk = stPeer.iInstanceId == (int32_t)iIndex + 1 && stPeer.stResult.GetRet() == OI_OK && stPeer.stResult.GetRes().iTag == 40 && stPeer.stResult.GetRes().iServiceId == stPeer.iInstanceId;
	}
	fnReport("multi call all slots=%d bad=%d\n", bAllOk ? 0 : 1, (int32_t)vAll.size());

	iBadTotal += co_await SwapFixed();

	// 服务端顺着请求所在的连接推回来。第二台用单向消息触发，顺带验 NotifyPeer。
	const int32_t iPushOne = co_await ProbeOnce(m_stPeers, 1, 51, 1);
	::OI::RpcData::PeerProbeReq stPushRequest;
	stPushRequest.iTag = 52;
	stPushRequest.iNotify = 1;
	const int32_t iPushTwo = m_stPeers.NotifyPeer(2, stPushRequest);
	const int32_t iPushNone = m_stPeers.NotifyPeer(9, stPushRequest);
	const int32_t iPushAny = m_stPeers.NotifyAny(stPushRequest);
	const bool bNotes = co_await WaitFor([this]()
	{
		return m_stPeers.Notes() >= 3;
	}, 3000);
	fnReport("multi reverse notify call=%d notify=%d none=%d any=%d received=%d wrong=%d bad=%d\n",
	    (iPushOne == 1 && iPushTwo == OI_OK && iPushNone == OI_ERR_CONNECT && iPushAny == OI_OK && bNotes && m_stPeers.Notes() == 3 && m_stPeers.NoteBad() == 0) ? 0 : 1,
	    iPushOne, iPushTwo, iPushNone, iPushAny, m_stPeers.Notes(), m_stPeers.NoteBad());

	// 脚本看到这行就杀掉 2 号。先断连接，注册表撤掉之后名单里也没了。
	std::printf("multi request stop id=2\n");
	std::fflush(stdout);
	const bool bDown = co_await WaitFor([this]()
	{
		return m_stPeers.Disconnects(2) >= 1;
	}, 10000);
	const bool bGone = co_await WaitFor([this]()
	{
		return m_stPeers.InstanceIds() == std::vector<int32_t>{1};
	}, 10000);
	const int32_t iStillOne = co_await ProbeOnce(m_stPeers, 1, 71, 0);
	const int32_t iGoneTwo = co_await ProbeOnce(m_stPeers, 2, 72, 0);
	fnReport("multi peer down disconnect=%d gone=%d one=%d two=%d bad=%d\n",
	    (bDown && bGone && iStillOne == 1 && iGoneTwo == OI_ERR_CONNECT && m_stPeers.Disconnects(1) == 0) ? 0 : 1, bDown ? 1 : 0, bGone ? 1 : 0,
	    iStillOne, iGoneTwo);

	// 脚本看到这行就重启 2 号。重新登记后名单回来，自动建连。
	std::printf("multi request start id=2\n");
	std::fflush(stdout);
	const bool bBack = co_await WaitFor([this]()
	{
		return m_stPeers.InstanceIds() == std::vector<int32_t>{1, 2} && m_stPeers.Connects(2) >= 2;
	}, 20000);
	const int32_t iBackTwo = co_await ProbeOnce(m_stPeers, 2, 81, 0);
	fnReport("multi peer back listed=%d two=%d connects=%d bad=%d\n", (bBack && iBackTwo == 2) ? 0 : 1, bBack ? 1 : 0, iBackTwo,
	    m_stPeers.Connects(2));

	std::printf("multi done bad=%d\n", iBadTotal);
	std::fflush(stdout);
	TestBoard::GetInstance().Done(iBadTotal == 0 ? 0 : 1);
}
