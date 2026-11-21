#include "blocking_mgr.h"
#include "hold_test.h"
#include "oi_rpc_session_manager.h"
#include "oi_http.h"
#include "oi_log_client.h"
#include "oi_log.h"
#include "oi_registry_client.h"
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include "oi_rpcdata_jsonall.h"
#include <limits>
#include "oi_config.h"
#include "oi_rpcdata_allechoreq.h"
#include "oi_rpcdata_allechores.h"
#include "oi_rpcdata_holdreq.h"
#include "oi_rpcdata_holdres.h"
#include "oi_rpcdata_peerprobereq.h"
#include "ipcclient.h"
#include "rpc_ids.h"
#include "oi_redis.h"
#include "oi_poll.h"
#include "oi_blocking_work_session.h"
#include "oi_net_session.h"
#include "oi_channel.h"
#include "oi_timer_manager.h"
#include <uv.h>
#include "test_board.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

struct PollWakeProbe
{
	std::atomic<bool> bWorker{false};
	std::atomic<bool> bPoll{false};
	std::atomic<bool> bDone{false};
};

struct PollWakeBurst
{
	std::atomic<int32_t> iDone{0};
};

static int32_t PostPollWakeBurst(std::shared_ptr<PollWakeBurst> pBurst)
{
	for (int32_t iIndex = 0; iIndex < 10000; ++iIndex)
	{
		auto* pOwned = new std::shared_ptr<PollWakeBurst>(pBurst);
		try
		{
			OI::Net::Poll::GetInstance().PostOnLoop([](void* pArgument)
			{
				std::unique_ptr<std::shared_ptr<PollWakeBurst>> pHolder(static_cast<std::shared_ptr<PollWakeBurst>*>(pArgument));
				(*pHolder)->iDone.fetch_add(1);
			}, pOwned);
		}
		catch (...)
		{
			delete pOwned;
			throw;
		}
	}
	return OI_OK;
}

static int32_t PostPollWake(std::shared_ptr<PollWakeProbe> pProbe)
{
	pProbe->bWorker.store(!OI::Net::Poll::GetInstance().InLoopThread());
	auto* pOwned = new std::shared_ptr<PollWakeProbe>(std::move(pProbe));
	try
	{
		OI::Net::Poll::GetInstance().PostOnLoop([](void* pArgument)
		{
			std::unique_ptr<std::shared_ptr<PollWakeProbe>> pHolder(static_cast<std::shared_ptr<PollWakeProbe>*>(pArgument));
			(*pHolder)->bPoll.store(OI::Net::Poll::GetInstance().InLoopThread());
			(*pHolder)->bDone.store(true);
		}, pOwned);
	}
	catch (...)
	{
		delete pOwned;
		throw;
	}
	return OI_OK;
}

static int32_t BlockingCopy(std::string sInput, std::string& sOutput)
{
	sOutput = std::move(sInput);
	return 0;
}

class BlockingAccessSession : public OI::Net::Session
{
  public:
	BlockingAccessSession()
	    : Session(nullptr)
	{
	}
	void PassiveClose(int) override
	{
	}
	void ActiveClose() override
	{
	}
	void OnDataIn() override
	{
	}
	void OnDataOut() override
	{
	}
	int32_t CheckAllowSend()
	{
		return AllowSend();
	}
	int32_t CheckForbidSend()
	{
		return ForbidSend();
	}
	int32_t CheckAllowRead()
	{
		return AllowRead();
	}
	int32_t CheckForbidRead()
	{
		return ForbidRead();
	}

  protected:
	void Close(int) override
	{
	}
};

class BlockingAccessChannel : public OI::Net::Channel
{
  public:
	BlockingAccessChannel()
	    : Channel(OI::Net::NetSocket(), nullptr)
	{
	}
	void Attach(net_socket_t iDescriptor)
	{
		stSocket.Attach(iDescriptor);
	}
	net_socket_t Descriptor() const
	{
		return stSocket.GetSocket();
	}
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
};

class PollUnregisterProbe : public OI::Net::Channel
{
  public:
	std::atomic<bool> bUnregistered{false};
	std::atomic<int32_t> iRead{0};
	std::atomic<int32_t> iWriteAfter{0};
	std::atomic<int32_t> iCloseAfter{0};
	explicit PollUnregisterProbe(net_socket_t iDescriptor)
	    : Channel(OI::Net::NetSocket(), nullptr)
	{
		stSocket.Attach(iDescriptor);
	}
	void DataIn() override
	{
		char vBuffer[16];
		sockaddr_storage stPeer{};
		socklen_t iLength = sizeof(stPeer);
		if (NetOs_RecvFrom(stSocket.GetSocket(), vBuffer, sizeof(vBuffer), (sockaddr*)&stPeer, &iLength) == 1 && vBuffer[0] == 'a')
		{
			iRead.fetch_add(1);
		}
		OI::Net::Poll::GetInstance().IOEventUnRegister(this, false);
		bUnregistered.store(true);
	}
	void DataOut() override
	{
		if (bUnregistered.load())
		{
			iWriteAfter.fetch_add(1);
		}
	}
	bool TryClose() override
	{
		if (bUnregistered.load())
		{
			iCloseAfter.fetch_add(1);
		}
		return false;
	}
};

class BlockingAccessClient : public OI::Rpc::ProtoClientManager
{
  public:
	void OnClientConnect(OI::Net::Session*) override
	{
	}
	void OnClientDisconnect(const OI::Net::Session*) override
	{
	}
};

struct BlockingAccessResult
{
	int32_t iRegisterValid = 0;
	int32_t iRegisterInvalid = 0;
	int32_t iManager = 0;
	int32_t iSession = 0;
	int32_t iAllow = 0;
	int32_t iChannel = 0;
	int32_t iSessionForbid = 0;
	int32_t iSessionRead = 0;
	int32_t iSessionForbidRead = 0;
	int32_t iChannelForbid = 0;
	int32_t iChannelRead = 0;
	int32_t iChannelForbidRead = 0;
	int32_t iRouteStart = 0;
	int32_t iRouteAddress = 0;
	int32_t iRaw = 0;
	int32_t iPending = 0;
	int32_t iPeer = 0;
	int32_t iAny = 0;
	int32_t iClose = 0;
	int32_t iCloseAll = 0;
#ifdef _SINGLE_THREAD_
	bool bConnectRejected = false;
	int32_t iServer = 0;
	int32_t iOpenPeer = 0;
	sid_t iPeerOutput = INVALID_SID;
	int32_t iReconnect = 0;
	int32_t iRegistrySync = 0;
	int32_t iRegistryWatch = 0;
	int32_t iNotify = 0;
	int32_t iRedis = 0;
	int32_t iTimer = 0;
	int32_t iClosureTimer = 0;
	bool bClosureReleased = false;
#endif
};

static int32_t CheckBlockingAccess(BlockingAccessSession& stSession, BlockingAccessChannel& stChannel, BlockingAccessChannel& stValidChannel, BlockingAccessChannel& stInvalidChannel, BlockingAccessResult& stResult)
{
	stResult.iRegisterValid = OI::Net::Poll::GetInstance().IOEventRegister(&stValidChannel, true, false);
	stResult.iRegisterInvalid = OI::Net::Poll::GetInstance().IOEventRegister(&stInvalidChannel, true, false);
	const OI::Base::Bytes stBytes("probe", 5);
	stResult.iManager = IpcClient::GetInstance().SendBytes(-1, stBytes);
	stResult.iSession = stSession.Send(stBytes);
	stResult.iAllow = stSession.CheckAllowSend();
	stResult.iChannel = stChannel.AllowSend();
	stResult.iSessionForbid = stSession.CheckForbidSend();
	stResult.iSessionRead = stSession.CheckAllowRead();
	stResult.iSessionForbidRead = stSession.CheckForbidRead();
	stResult.iChannelForbid = stChannel.ForbidSend();
	stResult.iChannelRead = stChannel.AllowRead();
	stResult.iChannelForbidRead = stChannel.ForbidRead();
	BlockingAccessClient stClient;
	OI::Rpc::ProtoMultiClientManager stRoutes;
#ifdef _SINGLE_THREAD_
	stClient.DefaultInit("127.0.0.1", "0");
	try
	{
		(void)stClient.StartClient();
	}
	catch (const std::logic_error&)
	{
		stResult.bConnectRejected = true;
	}
	stResult.iServer = stClient.StartServer();
	stResult.iPeerOutput = 12345;
	stResult.iOpenPeer = stClient.OpenPeer("127.0.0.1", "9", stResult.iPeerOutput);
	static BlockingAccessClient stReconnectClient;
	stResult.iReconnect = stReconnectClient.Reconnect(600000);
	stResult.iRouteStart = stRoutes.Start();
	stResult.iRouteAddress = stRoutes.UseAddress("127.0.0.1", "1");
#endif
	const OI::Base::Bytes stEmpty;
	stResult.iRaw = stClient.SendRaw(stEmpty);
	stResult.iPending = stClient.SendRawOrPend(stEmpty, 0);
	OI::RpcData::PeerProbeReq stPeerRequest;
	stResult.iPeer = stRoutes.NotifyPeer(1, stPeerRequest);
	stResult.iAny = stRoutes.NotifyAny(stPeerRequest);
	stResult.iClose = stClient.CloseSession(INVALID_SID);
	stResult.iCloseAll = stClient.CloseAllSessions();
#ifdef _SINGLE_THREAD_
	OI::RpcData::HoldReq stRequest;
	stResult.iNotify = OI::Rpc::RpcNotify(IpcClient::GetInstance(), INVALID_SID, stRequest);
	stResult.iRedis = OI::Redis::RedisClient::GetInstance().CallSync({"PING"}).GetRet();
	std::vector<OI::Base::ServiceInstance> vServices;
	stResult.iRegistrySync = OI::Base::RegistryClient::GetInstance().PullSync("blocking-probe", vServices);
	stResult.iRegistryWatch = OI::Base::RegistryClient::GetInstance().Watch(&stClient, "blocking-probe");
	auto& stTimers = OI::Timer::BaseTimerManager::GetInstance();
	stResult.iTimer = stTimers.AddTimer([]()
	{
	}, 1);
	auto pClosure = std::make_shared<int>(1);
	std::weak_ptr<int> pWeak = pClosure;
	stResult.iClosureTimer = stTimers.AddTimer([pClosure]()
	{
	}, 60000);
	pClosure.reset();
	stResult.bClosureReleased = pWeak.expired();
#endif
	return OI_OK;
}

OI::Coro::TaskResult<int> BlockingMgr::BlockingWaiter(OI::Coro::TaskResult<int32_t> stTask)
{
	const int32_t iFirst = co_await stTask;
	const int32_t iSecond = co_await stTask;
	co_return iFirst == 0 && iSecond == 0 ? 0 : 1;
}

struct BlockingPipeProbe
{
	uv_file vFiles[2]{-1, -1};
	std::atomic<bool> bStarted{false};
	std::atomic<bool> bFinished{false};
	std::atomic<bool> bOtherThread{false};
	std::thread::id stCallerThread;
	~BlockingPipeProbe()
	{
		for (const auto iFile : vFiles)
		{
			if (iFile >= 0)
			{
				uv_fs_t stClose{};
				uv_fs_close(nullptr, &stClose, iFile, nullptr);
				uv_fs_req_cleanup(&stClose);
			}
		}
	}
};

static int32_t ReadBlocking(std::shared_ptr<BlockingPipeProbe> pProbe, std::string& sOutput)
{
	char vBuffer[16]{};
	uv_buf_t stBuffer = uv_buf_init(vBuffer, sizeof(vBuffer));
	uv_fs_t stRead{};
	pProbe->bOtherThread.store(std::this_thread::get_id() != pProbe->stCallerThread);
	pProbe->bStarted.store(true);
	const int iRead = uv_fs_read(nullptr, &stRead, pProbe->vFiles[0], &stBuffer, 1, -1, nullptr);
	uv_fs_req_cleanup(&stRead);
	if (iRead > 0)
	{
		sOutput.assign(vBuffer, static_cast<size_t>(iRead));
	}
	pProbe->bFinished.store(true);
	return iRead > 0 ? 0 : OI_ERR_FAIL;
}

OI::Coro::TaskResult<int> BlockingMgr::CheckBlockingCategories()
{
	using OI::Thread::BlockingType;
	int iBad = 0;
	const int32_t iMissing = co_await OI::Coro::RunBlocking(BlockingType::Sdk, []() -> int32_t
	{
		return 0;
	});
	const int32_t iMysqlConfig = OI::Thread::ConfigureBlockingPool(BlockingType::Mysql, 1, 1);
	const int32_t iFileConfig = OI::Thread::ConfigureBlockingPool(BlockingType::File, 1, 4);
	if (iMissing != OI_ERR_NOT_FOUND || iMysqlConfig != 0 || iFileConfig != 0 ||
	    OI::Thread::ConfigureBlockingPool(BlockingType::Sdk, 0, 1) != OI_ERR_INVALID_ARGUMENT ||
	    OI::Thread::ConfigureBlockingPool(BlockingType::Sdk, 1, 1, 0) != OI_ERR_INVALID_ARGUMENT)
	{
		std::fprintf(stderr, "blocking category setup missing=%d mysql=%d file=%d\n", iMissing, iMysqlConfig, iFileConfig);
		co_return 1;
	}
	auto pProbe = std::make_shared<BlockingPipeProbe>();
	if (uv_pipe(pProbe->vFiles, 0, 0) != 0)
	{
		std::fprintf(stderr, "blocking category pipe creation failed\n");
		co_return 1;
	}
	pProbe->stCallerThread = std::this_thread::get_id();
	std::string sRead;
	auto pQueuedRuns = std::make_shared<std::atomic<int>>(0);
	auto stRead = OI::Coro::RunBlocking(BlockingType::Mysql, ReadBlocking, pProbe, sRead);
	struct WriteEndGuard
	{
		std::shared_ptr<BlockingPipeProbe> pProbe;
		void Close()
		{
			if (pProbe->vFiles[1] >= 0)
			{
				uv_fs_t stClose{};
				uv_fs_close(nullptr, &stClose, pProbe->vFiles[1], nullptr);
				uv_fs_req_cleanup(&stClose);
				pProbe->vFiles[1] = -1;
			}
		}
		~WriteEndGuard()
		{
			Close();
		}
	} stWriteGuard{pProbe};
	std::optional<OI::Coro::TaskResult<int32_t>> stQueued;
	std::optional<OI::Coro::TaskResult<int32_t>> stBusy;
	int32_t iReconfigure = 0;
	int32_t iFile = 0;
	int32_t iRpc = OI_ERR_FAIL;
	int iMark = 0;
	int iWritten = -1;
	bool bBlocked = false;
	bool bProgressException = false;
	try
	{
		for (int iIndex = 0; iIndex < 500 && !pProbe->bStarted.load(); ++iIndex)
		{
			co_await OI::Coro::yield(1);
		}
		if (pProbe->bStarted.load())
		{
			stQueued.emplace(OI::Coro::RunBlocking(BlockingType::Mysql, [pQueuedRuns]() -> int32_t
			{
				++*pQueuedRuns;
				return 0;
			}));
			stBusy.emplace(OI::Coro::RunBlocking(BlockingType::Mysql, []() -> int32_t
			{
				return 0;
			}));
			iReconfigure = OI::Thread::ConfigureBlockingPool(BlockingType::Mysql, 2, 2);
			iFile = co_await OI::Coro::RunBlocking(BlockingType::File, []() -> int32_t
			{
				return 17;
			});
			co_await OI::Coro::yield(200);
			OI::RpcData::HoldReq stRequest;
			stRequest.iMark = 43;
			OI::RpcData::HoldRes stResponse;
			iRpc = co_await OI::Rpc::RpcCall(IpcClient::GetInstance(), stRequest, stResponse, OI::Timer::Seconds(2));
			iMark = stResponse.iMark;
			bBlocked = !pProbe->bFinished.load() && pQueuedRuns->load() == 0;
			char vMessage[] = "category";
			uv_buf_t stMessage = uv_buf_init(vMessage, sizeof(vMessage) - 1);
			uv_fs_t stWrite{};
			iWritten = uv_fs_write(nullptr, &stWrite, pProbe->vFiles[1], &stMessage, 1, -1, nullptr);
			uv_fs_req_cleanup(&stWrite);
		}
	}
	catch (const std::exception& stError)
	{
		std::fprintf(stderr, "blocking category progress exception: %s\n", stError.what());
		bProgressException = true;
	}
	catch (...)
	{
		std::fprintf(stderr, "blocking category progress unknown exception\n");
		bProgressException = true;
	}
	stWriteGuard.Close();
	const int32_t iRead = co_await stRead;
	int32_t iQueued = OI_ERR_FAIL;
	int32_t iBusy = OI_ERR_FAIL;
	if (stQueued)
	{
		iQueued = co_await *stQueued;
	}
	if (stBusy)
	{
		iBusy = co_await *stBusy;
	}
	const int32_t iError = co_await OI::Coro::RunBlocking(BlockingType::File, []() -> int32_t
	{
		return OI_ERR_FAIL;
	});
	bool bException = false;
	try
	{
		(void)(co_await OI::Coro::RunBlocking(BlockingType::File, []() -> int32_t
		{
			throw std::runtime_error("category failure");
		}));
	}
	catch (const std::runtime_error&)
	{
		bException = true;
	}
	const int32_t iRecovery = co_await OI::Coro::RunBlocking(BlockingType::File, []() -> int32_t
	{
		return 23;
	});
	if (bProgressException || !bBlocked || !pProbe->bOtherThread.load() || iBusy != OI_ERR_BUSY || iReconfigure != OI_ERR_BUSY ||
	    iFile != 17 || iRpc != 0 || iMark != 43 || iWritten != 8 ||
	    iRead != 0 || sRead != "category" || iQueued != 0 || pQueuedRuns->load() != 1 ||
	    iError != OI_ERR_FAIL || !bException || iRecovery != 23)
	{
		std::fprintf(stderr, "blocking category failed blocked=%d thread=%d busy=%d reconfigure=%d file=%d rpc=%d mark=%d write=%d read=%d queued=%d runs=%d error=%d exception=%d recovery=%d\n",
		    bBlocked, pProbe->bOtherThread.load(), iBusy, iReconfigure, iFile, iRpc, iMark, iWritten, iRead, iQueued, pQueuedRuns->load(), iError, bException, iRecovery);
		++iBad;
	}
	LOG_TRACE("hold blocking categories bad=%d", iBad);
	co_return iBad;
}

static OI::Coro::TaskResult<int> CheckPollUnregister()
{
	OI::Net::NetAddr stAddress;
	if (!stAddress.Init("127.0.0.1", "0"))
	{
		co_return 1;
	}
	const net_socket_t iDescriptor = stAddress.CreateUdp(true);
	if (iDescriptor == INVALID_SOCKET_ID)
	{
		co_return 1;
	}
	PollUnregisterProbe stProbe(iDescriptor);
	sockaddr_storage stBound{};
	socklen_t iLength = sizeof(stBound);
	if (NetOs_Bind(iDescriptor, stAddress.NetAttrPtr(), stAddress.NetAttrLen()) != 0 ||
	    NetOs_GetSockName(iDescriptor, (sockaddr*)&stBound, &iLength) != 0 ||
	    NetOs_SendTo(iDescriptor, "a", 1, (sockaddr*)&stBound, iLength) != 1)
	{
		std::fprintf(stderr, "poll unregister setup failed errno=%d\n", errno);
		co_return 1;
	}
	const int32_t iRegister = OI::Net::Poll::GetInstance().IOEventRegister(&stProbe, true, true);
	if (iRegister != OI_OK)
	{
		std::fprintf(stderr, "poll unregister registration failed=%d\n", iRegister);
		co_return 1;
	}
	for (int32_t iWait = 0; iWait < 500 && !stProbe.bUnregistered.load(); ++iWait)
	{
		co_await OI::Coro::yield(1);
	}
	if (!stProbe.bUnregistered.load())
	{
		OI::Net::Poll::GetInstance().IOEventUnRegister(&stProbe, false);
	}
	co_await OI::Coro::yield(50);
	const int32_t iSent = NetOs_SendTo(iDescriptor, "b", 1, (sockaddr*)&stBound, iLength);
	char vBuffer[16]{};
	sockaddr_storage stPeer{};
	socklen_t iPeerLength = sizeof(stPeer);
	const int32_t iReceived = NetOs_RecvFrom(iDescriptor, vBuffer, sizeof(vBuffer), (sockaddr*)&stPeer, &iPeerLength);
	NetOs_Close(iDescriptor);
	const bool bOk = stProbe.bUnregistered.load() && stProbe.iRead.load() == 1 && stProbe.iWriteAfter.load() == 0 &&
	                 stProbe.iCloseAfter.load() == 0 && iSent == 1 && iReceived == 1 && vBuffer[0] == 'b';
	if (!bOk)
	{
		std::fprintf(stderr, "poll unregister done=%d read=%d write_after=%d close_after=%d sent=%d received=%d byte=%d\n",
		    stProbe.bUnregistered.load(), stProbe.iRead.load(), stProbe.iWriteAfter.load(), stProbe.iCloseAfter.load(), iSent, iReceived, (int)vBuffer[0]);
	}
	LOG_TRACE("hold poll unregister read=%d write_after=%d close_after=%d reused=%d bad=%d", stProbe.iRead.load(),
	    stProbe.iWriteAfter.load(), stProbe.iCloseAfter.load(), iSent == 1 && iReceived == 1 && vBuffer[0] == 'b', bOk ? 0 : 1);
	co_return bOk ? 0 : 1;
}

OI::Coro::TaskResult<int> BlockingMgr::CheckBlockingWork()
{
	int iBad = 0;
	const bool bFifoType = OI::Net::BlockingWorkSessionManager::GetInstance().GetSessionType() == OI::Net::SESSION_TYPE_FIFO &&
	                       OI::Net::SessionManager::GetSessionTypeByString("fifo") == OI::Net::SESSION_TYPE_FIFO;
	if (!bFifoType)
	{
		++iBad;
	}
	LOG_TRACE("hold blocking fifo type bad=%d", bFifoType ? 0 : 1);
	auto pWakeProbe = std::make_shared<PollWakeProbe>();
	const int32_t iWakePost = co_await OI::Coro::RunBlocking(PostPollWake, std::shared_ptr<PollWakeProbe>(pWakeProbe));
	for (int32_t iWait = 0; iWait < 300 && !pWakeProbe->bDone.load(); ++iWait)
	{
		co_await OI::Coro::yield(10);
	}
	const bool bWake = iWakePost == OI_OK && pWakeProbe->bWorker.load() && pWakeProbe->bPoll.load() && pWakeProbe->bDone.load();
	if (!bWake)
	{
		++iBad;
	}
	LOG_TRACE("hold poll worker wake worker=%d poll=%d done=%d bad=%d", pWakeProbe->bWorker.load(),
	    pWakeProbe->bPoll.load(), pWakeProbe->bDone.load(), bWake ? 0 : 1);
	auto pBurst = std::make_shared<PollWakeBurst>();
	const int32_t iBurstPost = co_await OI::Coro::RunBlocking(PostPollWakeBurst, std::shared_ptr<PollWakeBurst>(pBurst));
	for (int32_t iWait = 0; iWait < 300 && pBurst->iDone.load() < 10000; ++iWait)
	{
		co_await OI::Coro::yield(10);
	}
	const bool bBurst = iBurstPost == OI_OK && pBurst->iDone.load() == 10000;
	if (!bBurst)
	{
		++iBad;
	}
	LOG_TRACE("hold poll wake burst posts=10000 done=%d bad=%d", pBurst->iDone.load(), bBurst ? 0 : 1);
	iBad += co_await CheckPollUnregister();
	BlockingAccessSession stAccessSession;
	BlockingAccessChannel stAccessChannel;
	BlockingAccessResult stAccessResult;
	BlockingAccessChannel stValidChannel;
	BlockingAccessChannel stInvalidChannel;
	OI::Net::NetAddr stAddress;
	if (!stAddress.Init("127.0.0.1", "0"))
	{
		co_return 1;
	}
	const net_socket_t iDescriptor = stAddress.CreateUdp(true);
	if (iDescriptor == INVALID_SOCKET_ID)
	{
		std::fprintf(stderr, "blocking poll socket creation failed\n");
		co_return 1;
	}
	stValidChannel.Attach(iDescriptor);
	if (NetOs_Bind(iDescriptor, stAddress.NetAttrPtr(), stAddress.NetAttrLen()) != 0)
	{
		std::fprintf(stderr, "blocking poll socket bind failed\n");
		co_return 1;
	}
	BlockingAccessSession stLoopSession;
	const OI::Base::Bytes stLoopBytes("loop", 4);
	const int32_t iLoopSend = stLoopSession.Send(stLoopBytes);
	const int32_t iLoopAllow = stLoopSession.CheckAllowSend();
	const int32_t iLoopChannel = stAccessChannel.AllowSend();
	if (iLoopSend != OI_OK || stLoopSession.PendingSendBytes() != 4 || iLoopAllow != OI_OK || iLoopChannel != OI_OK)
	{
		std::fprintf(stderr, "blocking loop access send=%d allow=%d channel=%d\n", iLoopSend, iLoopAllow, iLoopChannel);
		++iBad;
	}
	const int32_t iAccessTask = co_await OI::Coro::RunBlocking(CheckBlockingAccess, stAccessSession, stAccessChannel, stValidChannel, stInvalidChannel, stAccessResult);
#ifdef _SINGLE_THREAD_
	const bool bPollOk = stAccessResult.iRegisterValid == OI_ERR_WRONG_THREAD && stAccessResult.iRegisterInvalid == OI_ERR_WRONG_THREAD &&
	                     stValidChannel.Descriptor() == iDescriptor;
	const bool bAccessOk = stAccessResult.iManager == OI_ERR_WRONG_THREAD && stAccessResult.iSession == OI_ERR_WRONG_THREAD &&
	                       stAccessResult.iAllow == OI_ERR_WRONG_THREAD && stAccessResult.iChannel == OI_ERR_WRONG_THREAD && stAccessSession.PendingSendBytes() == 0 &&
	                       stAccessResult.iRaw == OI_ERR_WRONG_THREAD && stAccessResult.iPending == OI_ERR_WRONG_THREAD && stAccessResult.iNotify == OI_ERR_WRONG_THREAD &&
	                       stAccessResult.iPeer == OI_ERR_WRONG_THREAD && stAccessResult.iAny == OI_ERR_WRONG_THREAD &&
	                       stAccessResult.iSessionForbid == OI_ERR_WRONG_THREAD && stAccessResult.iSessionRead == OI_ERR_WRONG_THREAD &&
	                       stAccessResult.iSessionForbidRead == OI_ERR_WRONG_THREAD && stAccessResult.iChannelForbid == OI_ERR_WRONG_THREAD &&
	                       stAccessResult.iChannelRead == OI_ERR_WRONG_THREAD && stAccessResult.iChannelForbidRead == OI_ERR_WRONG_THREAD &&
	                       stAccessResult.iRouteStart == OI_ERR_WRONG_THREAD && stAccessResult.iRouteAddress == OI_ERR_WRONG_THREAD &&
	                       stAccessResult.bConnectRejected && stAccessResult.iServer == OI_ERR_WRONG_THREAD &&
	                       stAccessResult.iOpenPeer == OI_ERR_WRONG_THREAD && stAccessResult.iPeerOutput == 12345 && stAccessResult.iReconnect == OI_ERR_WRONG_THREAD &&
	                       stAccessResult.iClose == OI_ERR_WRONG_THREAD && stAccessResult.iCloseAll == OI_ERR_WRONG_THREAD &&
	                       stAccessResult.iRegistrySync == OI_ERR_WRONG_THREAD && stAccessResult.iRegistryWatch == OI_ERR_WRONG_THREAD &&
	                       stAccessResult.iRedis == OI_ERR_WRONG_THREAD && stAccessResult.iTimer == OI_ERR_WRONG_THREAD &&
	                       stAccessResult.iClosureTimer == OI_ERR_WRONG_THREAD && stAccessResult.bClosureReleased;
#else
	const bool bPollOk = stAccessResult.iRegisterValid == OI_OK && stAccessResult.iRegisterInvalid == OI_ERR_SOCKET &&
	                     stValidChannel.Descriptor() == iDescriptor && stInvalidChannel.Descriptor() == INVALID_SOCKET_ID;
	const bool bAccessOk = stAccessResult.iManager == OI_ERR_CONNECT && stAccessResult.iSession == OI_OK &&
	                       stAccessResult.iAllow == OI_OK && stAccessResult.iChannel == OI_OK && stAccessSession.PendingSendBytes() == 5 &&
	                       stAccessResult.iRaw == OI_ERR_CONNECT && stAccessResult.iPending == OI_OK &&
	                       stAccessResult.iPeer == OI_ERR_CONNECT && stAccessResult.iAny == OI_ERR_CONNECT &&
	                       stAccessResult.iSessionForbid == OI_OK && stAccessResult.iSessionRead == OI_OK && stAccessResult.iSessionForbidRead == OI_OK &&
	                       stAccessResult.iChannelForbid == OI_OK && stAccessResult.iChannelRead == OI_OK && stAccessResult.iChannelForbidRead == OI_OK &&
	                       stAccessResult.iClose == OI_ERR_NOT_FOUND && stAccessResult.iCloseAll == OI_OK;
#endif
	if (!bPollOk)
	{
		std::fprintf(stderr, "blocking poll register valid=%d invalid=%d valid_descriptor=%d invalid_descriptor=%d\n",
		    stAccessResult.iRegisterValid, stAccessResult.iRegisterInvalid, stValidChannel.Descriptor() == iDescriptor,
		    stInvalidChannel.Descriptor() == INVALID_SOCKET_ID);
		++iBad;
	}
	LOG_TRACE("hold blocking poll register bad=%d", bPollOk ? 0 : 1);
	if (iAccessTask != OI_OK || !bAccessOk || stAccessChannel.iEventMask != 0)
	{
		std::fprintf(stderr, "blocking access failed task=%d manager=%d session=%d allow=%d channel=%d pending=%zu mask=%d raw=%d pend=%d peer=%d any=%d close=%d all=%d\n",
		    iAccessTask, stAccessResult.iManager, stAccessResult.iSession, stAccessResult.iAllow, stAccessResult.iChannel,
		    stAccessSession.PendingSendBytes(), stAccessChannel.iEventMask, stAccessResult.iRaw, stAccessResult.iPending,
		    stAccessResult.iPeer, stAccessResult.iAny, stAccessResult.iClose, stAccessResult.iCloseAll);
		++iBad;
	}
	LOG_TRACE("hold blocking access bad=%d", iBad);
	iBad += co_await CheckBlockingCategories();
	std::vector<OI::Coro::TaskResult<int>> vTasks;
	constexpr uint32_t iSeed = 20261110;
	std::mt19937 stRandom(iSeed);
	std::uniform_int_distribution<int> stDelay(1, 20);
	for (int iIndex = 0; iIndex < 128; ++iIndex)
	{
		const int iDelayMilliseconds = stDelay(stRandom);
		vTasks.push_back(OI::Coro::RunBlocking([iIndex, iDelayMilliseconds]()
		{
			uv_sleep(iDelayMilliseconds);
			return iIndex * 3;
		}));
	}
	LOG_TRACE("hold blocking delay seed=%u min_ms=1 max_ms=20", iSeed);
	const auto vResults = co_await OI::Coro::WhenAll(std::move(vTasks));
	for (int iIndex = 0; iIndex < 128; ++iIndex)
	{
		if (vResults[iIndex] != iIndex * 3)
		{
			std::fprintf(stderr, "blocking check failed line=%d\n", __LINE__);
			++iBad;
		}
	}
	auto pVoidRuns = std::make_shared<std::atomic<int>>(0);
	auto stVoid = OI::Coro::RunBlocking([pVoidRuns]()
	{
		++*pVoidRuns;
		return int32_t{0};
	});
	co_await stVoid;
	co_await stVoid;
	if (pVoidRuns->load() != 1)
	{
		std::fprintf(stderr, "blocking check failed line=%d\n", __LINE__);
		++iBad;
	}
	auto stMove = OI::Coro::RunBlocking([pValue = std::make_unique<int>(73)]()
	{
		return *pValue;
	});
	if ((co_await stMove) != 73)
	{
		std::fprintf(stderr, "blocking check failed line=%d\n", __LINE__);
		++iBad;
	}
	auto stException = OI::Coro::RunBlocking([]() -> int
	{
		throw std::runtime_error("blocking failure");
	});
	for (int iIndex = 0; iIndex < 2; ++iIndex)
	{
		try
		{
			(void)(co_await stException);
			std::fprintf(stderr, "blocking check failed line=%d\n", __LINE__);
			++iBad;
		}
		catch (const std::runtime_error& stError)
		{
			if (std::string(stError.what()) != "blocking failure")
			{
				std::fprintf(stderr, "blocking check failed line=%d\n", __LINE__);
				++iBad;
			}
		}
	}
	std::string sOutput;
	const int32_t iCopyCode = co_await OI::Coro::RunBlocking(BlockingCopy, std::string(256, 'r'), sOutput);
	if (iCopyCode != 0 || sOutput != std::string(256, 'r'))
	{
		std::fprintf(stderr, "blocking direct function failed line=%d\n", __LINE__);
		++iBad;
	}
	auto pRuns = std::make_shared<std::atomic<int>>(0);
	auto pSharedOutput = std::make_shared<std::string>();
	auto stShared = OI::Coro::RunBlocking([pRuns, pSharedOutput]()
	{
		++*pRuns;
		uv_sleep(20);
		*pSharedOutput = std::string(256, 's');
		return int32_t{0};
	});
	std::vector<OI::Coro::TaskResult<int>> vWaiters;
	for (int iIndex = 0; iIndex < 64; ++iIndex)
	{
		vWaiters.push_back(BlockingWaiter(stShared));
	}
	for (const auto& stWaiter : vWaiters)
	{
		iBad += co_await stWaiter;
	}
	if (pRuns->load() != 1 || *pSharedOutput != std::string(256, 's'))
	{
		std::fprintf(stderr, "blocking check failed line=%d\n", __LINE__);
		++iBad;
	}
	auto pProbe = std::make_shared<BlockingPipeProbe>();
	if (uv_pipe(pProbe->vFiles, 0, 0) != 0)
	{
		std::fprintf(stderr, "blocking pipe creation failed line=%d\n", __LINE__);
		co_return iBad + 1;
	}
	pProbe->stCallerThread = std::this_thread::get_id();
	std::string sRead;
	auto stBlocking = OI::Coro::RunBlocking(ReadBlocking, pProbe, sRead);
	bool bStarted = false;
	bool bTimerProgress = false;
	bool bRpcProgress = false;
	bool bCompletedBeforeWrite = false;
	try
	{
		for (int iIndex = 0; iIndex < 500 && !pProbe->bStarted.load(); ++iIndex)
		{
			co_await OI::Coro::yield(1);
		}
		bStarted = pProbe->bStarted.load();
		co_await OI::Coro::yield(200);
		bTimerProgress = !pProbe->bFinished.load();
		OI::RpcData::HoldReq stRequest;
		stRequest.iMark = 42;
		OI::RpcData::HoldRes stResponse;
		const int32_t iCode = co_await OI::Rpc::RpcCall(IpcClient::GetInstance(), stRequest, stResponse, OI::Timer::Seconds(2));
		bRpcProgress = iCode == 0 && stResponse.iMark == 42;
	}
	catch (const std::exception& stError)
	{
		std::fprintf(stderr, "blocking progress exception: %s\n", stError.what());
		++iBad;
	}
	catch (...)
	{
		std::fprintf(stderr, "blocking progress unknown exception\n");
		++iBad;
	}
	bCompletedBeforeWrite = pProbe->bFinished.load();
	char vMessage[] = "pipe-ok";
	uv_buf_t stMessage = uv_buf_init(vMessage, sizeof(vMessage) - 1);
	uv_fs_t stWrite{};
	const int iWritten = uv_fs_write(nullptr, &stWrite, pProbe->vFiles[1], &stMessage, 1, -1, nullptr);
	uv_fs_req_cleanup(&stWrite);
	if (iWritten != static_cast<int>(sizeof(vMessage) - 1))
	{
		// 关闭写端让读端收到 EOF，即便写失败也不留下永久等待。
		uv_fs_t stClose{};
		uv_fs_close(nullptr, &stClose, pProbe->vFiles[1], nullptr);
		uv_fs_req_cleanup(&stClose);
		pProbe->vFiles[1] = -1;
	}
	const int32_t iReadCode = co_await stBlocking;
	if (!bStarted || !bTimerProgress || !bRpcProgress || bCompletedBeforeWrite ||
	    !pProbe->bOtherThread.load() || iWritten != static_cast<int>(sizeof(vMessage) - 1) ||
	    iReadCode != 0 || sRead != "pipe-ok")
	{
		std::fprintf(stderr, "blocking read failed started=%d timer=%d rpc=%d early=%d thread=%d write=%d read=%d value=%d\n",
		    bStarted, bTimerProgress, bRpcProgress, bCompletedBeforeWrite, pProbe->bOtherThread.load(), iWritten, iReadCode, sRead == "pipe-ok");
		++iBad;
	}
	LOG_TRACE("hold blocking read started=%d timer=%d rpc=%d early=%d content=%d bad=%d",
	    bStarted, bTimerProgress, bRpcProgress, bCompletedBeforeWrite, sRead == "pipe-ok", iBad);
	LOG_TRACE("hold blocking tasks=128 waiters=64 runs=%d bad=%d", pRuns->load(), iBad);
	fflush(stdout);
	co_return iBad;
}

// 阻塞池按类型隔离。128 个后台任务和 64 个等待者只执行一次；管道读在后台时，前台定时器和 RPC 仍能跑。
OI::Coro::TaskResult<int> BlockingMgr::Run()
{
	co_return co_await CheckBlockingWork();
}
