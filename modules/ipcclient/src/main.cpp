#include <stdio.h>
#include <exception>
#include <cstring>
#include <thread>
#include <uv.h>
#include "oi_channel.h"
#include "oi_poll.h"
#include "oi_socket.h"
#include "oi_udp_channel.h"
#include "oi_net.h"
#include "oi_base.h"
#include "ipcclient.h"
#include "hold_mgr.h"
#include "oi_rpc_session_manager.h"
#include "oi_timer_manager.h"
#include "timer_mgr.h"

#ifdef _SINGLE_THREAD_
class PollWrongThreadProbe : public ::OI::Net::Channel
{
  public:
	explicit PollWrongThreadProbe(net_socket_t iDescriptor)
	    : Channel(::OI::Net::NetSocket(), nullptr)
	{
		stSocket.Attach(iDescriptor);
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
#endif

static int32_t CheckPollWrongThread()
{
#ifdef _SINGLE_THREAD_
	::OI::Net::NetAddr stAddress;
	if (!stAddress.Init("127.0.0.1", "0"))
	{
		return OI_ERR_FAIL;
	}
	const net_socket_t iDescriptor = stAddress.CreateUdp(true);
	if (iDescriptor == INVALID_SOCKET_ID)
	{
		return OI_ERR_SOCKET;
	}
	auto* pProbe = new PollWrongThreadProbe(iDescriptor);
	if (NetOs_BindDatagram(iDescriptor, stAddress.NetAttrPtr(), stAddress.NetAttrLen()) != 0 ||
	    ::OI::Net::Poll::GetInstance().IOEventRegister(pProbe, true, false) != OI_OK)
	{
		delete pProbe;
		return OI_ERR_FAIL;
	}
	std::fprintf(stderr, "poll wrong thread probe registered=1\n");
	std::fflush(stderr);
	std::thread stForeign([pProbe]()
	{
		delete pProbe;
	});
	stForeign.join();
	std::fprintf(stderr, "poll wrong thread probe unexpectedly survived\n");
	return OI_ERR_FAIL;
#else
	std::printf("poll wrong thread probe mt skipped=1\n");
	return OI_OK;
#endif
}

class PollUdpCloseProbe : public ::OI::Rpc::ProtoSessionManager
{
  public:
	int32_t Pause()
	{
		return ::OI::Net::Poll::GetInstance().StartListener([](void* pArgument) -> int32_t
		{
			auto* pProbe = static_cast<PollUdpCloseProbe*>(pArgument);
			if (!pProbe->pUdpChannel || pProbe->pUdpChannel->iEventMask == 0)
			{
				return OI_ERR_FAIL;
			}
			::OI::Net::Poll::GetInstance().PauseEvents(pProbe->pUdpChannel, 60000);
			return pProbe->pUdpChannel->iEventMask == 0 ? OI_OK : OI_ERR_FAIL;
		}, this);
	}
};

static int32_t CheckPollUdpClose()
{
	static PollUdpCloseProbe stManager;
	stManager.DefaultInit("127.0.0.1", "0", ::OI::Net::SESSION_TYPE_UDP);
	const int32_t iStart = stManager.StartServer();
	const uint16_t iPort = stManager.LocalPort();
	if (iStart != OI_OK || iPort == 0)
	{
		std::fprintf(stderr, "poll udp close setup start=%d port=%u\n", iStart, (unsigned)iPort);
		return OI_ERR_FAIL;
	}
	const int32_t iPause = stManager.Pause();
	const int32_t iClose = ::OI::Net::Poll::GetInstance().StopAndJoin();
	std::printf("poll udp close bound=1 paused=%d loop=%d\n", iPause == OI_OK ? 1 : 0, iClose);
	std::fflush(stdout);
	return iPause == OI_OK && iClose == 0 ? OI_OK : OI_ERR_FAIL;
}

static int32_t CheckPollLoopBusy()
{
	// 留一个不属于 watch 的活跃句柄，loop 关闭必须失败，且状态要能传回调用方。
	const int32_t iArmed = ::OI::Net::Poll::GetInstance().StartListener([](void*) -> int32_t
	{
		auto* pTimer = new uv_timer_t();
		if (uv_timer_init(::OI::Net::Poll::GetInstance().Loop(), pTimer) != 0)
		{
			delete pTimer;
			return OI_ERR_FAIL;
		}
		if (uv_timer_start(pTimer, [](uv_timer_t*)
		{
		}, 60000, 60000) != 0)
		{
			return OI_ERR_FAIL;
		}
		return OI_OK;
	}, nullptr);
	if (iArmed != OI_OK)
	{
		std::fprintf(stderr, "poll loop busy arm=%d\n", iArmed);
		return OI_ERR_FAIL;
	}
	const int32_t iClose = ::OI::Net::Poll::GetInstance().StopAndJoin();
	const bool bBusy = iClose != 0;
	std::printf("poll loop busy code=%d bad=%d\n", iClose, bBusy ? 0 : 1);
	std::fflush(stdout);
	return bBusy ? OI_OK : OI_ERR_FAIL;
}

int main(int iArgumentCount, char* vArguments[])
{
	const char* pConfigPath = "../config/ipcclient.toml";
	int32_t iCode = ::OI::Base::ParseConfigArgument(iArgumentCount, vArguments, pConfigPath);
	if (iCode != OI_OK)
	{
		return iCode;
	}
	if (iArgumentCount == 2 && std::strcmp(vArguments[1], "--thread-mode") == 0)
	{
#ifdef _SINGLE_THREAD_
		std::printf("thread_mode=st\n");
#else
		std::printf("thread_mode=mt\n");
#endif
		return 0;
	}
	if (iArgumentCount == 2 && std::strcmp(vArguments[1], "--test-hooks") == 0)
	{
#ifdef OI_ENABLE_BLOCKING_WAKE_TRACE
		std::printf("blocking_wake_trace=1\n");
		return 41;
#else
		std::printf("blocking_wake_trace=0\n");
		return 40;
#endif
	}
	if (iArgumentCount == 2 && std::strcmp(vArguments[1], "--timer-before-init") == 0)
	{
		bool bCalled = false;
		const int32_t iStatus = ::OI::Timer::BaseTimerManager::GetInstance().AddTimer([&bCalled]()
		{
			bCalled = true;
		}, 1);
		const bool bPassed = iStatus != OI_OK && !bCalled;
		std::printf("timer before init code=%d called=%d bad=%d\n", iStatus, bCalled ? 1 : 0, bPassed ? 0 : 1);
		std::fflush(stdout);
		return bPassed ? 0 : 1;
	}
	iCode = ::OI::Base::StartFramework(pConfigPath);
	if (iCode != OI_OK)
	{
		fprintf(stderr, "StartFramework failed: code=%d\n", iCode);
		return iCode;
	}
	if (iArgumentCount == 2 && std::strcmp(vArguments[1], "--regression-ready") == 0)
	{
		std::fprintf(stderr, "ipcclient framework ready\n");
		std::fflush(stderr);
	}
	if (iArgumentCount == 2 && std::strcmp(vArguments[1], "--poll-udp-close") == 0)
	{
		return CheckPollUdpClose();
	}
	if (iArgumentCount == 2 && std::strcmp(vArguments[1], "--timer-backlog") == 0)
	{
		iCode = TimerMgr::CheckBacklog();
		::OI::Base::StopFramework();
		return iCode;
	}
	if (iArgumentCount == 2 && std::strcmp(vArguments[1], "--poll-loop-busy") == 0)
	{
		return CheckPollLoopBusy();
	}
	if (iArgumentCount == 2 && std::strcmp(vArguments[1], "--poll-wrong-thread") == 0)
	{
		iCode = CheckPollWrongThread();
		::OI::Base::StopFramework();
		return iCode;
	}
	iCode = ::OI::Log::Init();
	if (iCode != OI_OK)
	{
		fprintf(stderr, "Log Init failed: code=%d\n", iCode);
		::OI::Base::StopFramework();
		return iCode;
	}

	iCode = [&]() -> int32_t
	{
		try
		{
			iCode = IpcClient::GetInstance().Init();
			if (iCode != OI_OK)
			{
				fprintf(stderr, "IpcClient Init failed: code=%d\n", iCode);
				return iCode;
			}
			if (HoldMgr::GetInstance().RunsWithoutIpc())
			{
				HoldMgr::GetInstance().Start(INVALID_SID);
			}
			else
			{
				IpcClient::GetInstance().StartClient();
			}
		}
		catch (const std::exception& stError)
		{
			fprintf(stderr, "module init failed: %s\n", stError.what());
			fflush(stderr);
			return OI_ERR_FAIL;
		}
		catch (...)
		{
			fprintf(stderr, "module init failed: unknown exception\n");
			fflush(stderr);
			return OI_ERR_FAIL;
		}
		return OI_OK;
	}();
	if (iCode != OI_OK)
	{
		::OI::Base::StopFramework();
		return iCode;
	}

	return ::OI::Base::RunFramework();
}
