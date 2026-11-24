#include "oi_blocking_work_channel.h"
#include "oi_session.h"
#include "oi_poll.h"
#include <memory>
#include <cstdio>
#include "oi_net_os.h"
#include <utility>
#ifdef OI_ENABLE_BLOCKING_WAKE_TRACE
#include <chrono>
#endif

namespace OI
{
namespace Net
{
#ifdef OI_ENABLE_BLOCKING_WAKE_TRACE
namespace
{
thread_local BlockingWakeTrace* pNextTrace = nullptr;
thread_local bool bTracePoll = false;

int64_t TraceNow()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

BlockingWakeTrace::BlockingWakeTrace()
{
	assert(Poll::GetInstance().InLoopThread());
	assert(!bTracePoll);
	pNextTrace = this;
	bTracePoll = true;
}

BlockingWakeTrace::~BlockingWakeTrace()
{
	pNextTrace = nullptr;
	bTracePoll = false;
}
#endif

struct BlockingWorkChannel::Request
{
#ifdef OI_ENABLE_BLOCKING_WAKE_TRACE
	BlockingWakeTrace* pTrace = nullptr;
#endif
	BlockingWorkChannel* pChannel;
	WorkCompletion stCompletion;
	OI::Base::Bytes stPacket;
	std::function<void()> fnWork;
	std::shared_ptr<Request> pNext;
};

BlockingWorkChannel::BlockingWorkChannel(Session* pSessionLocal)
    : FifoChannel(pSessionLocal)
{
}

int32_t BlockingWorkChannel::Open()
{
	const int32_t iStatus = OpenPair();
	if (iStatus != OI_OK)
	{
		return iStatus;
	}
	const int32_t iRegistered = Poll::GetInstance().IOEventRegister(this, true, false);
	if (iRegistered != OI_OK)
	{
		NetOs_Close(stSocket.GetSocket());
		DetachSocket();
		CloseWriter();
		return iRegistered;
	}
	m_bAttached = true;
	return OI_OK;
}

int32_t BlockingWorkChannel::Submit(OI::Thread::BlockingType iType, uint64_t iOperation, std::function<void()> fnWork, OI::Base::Bytes stPacket)
{
	assert(Poll::GetInstance().InLoopThread());
	if (!m_bAttached)
	{
		const int32_t iStatus = Open();
		if (iStatus != OI_OK)
		{
			return iStatus;
		}
	}
	auto pRequest = std::make_shared<Request>();
#ifdef OI_ENABLE_BLOCKING_WAKE_TRACE
	if (iType == OI::Thread::BlockingType::Default)
	{
		pRequest->pTrace = std::exchange(pNextTrace, nullptr);
	}
#endif
	pRequest->pChannel = this;
	pRequest->stCompletion.iOperation = iOperation;
	pRequest->stPacket = std::move(stPacket);
	pRequest->fnWork = std::move(fnWork);
	const int32_t iStatus = OI::Thread::SubmitBlockingWork(iType, [pRequest]()
	{
		try
		{
#ifdef OI_ENABLE_BLOCKING_WAKE_TRACE
			if (pRequest->pTrace)
			{
				pRequest->pTrace->iWorkerStart = TraceNow();
			}
#endif
			pRequest->fnWork();
		}
		catch (...)
		{
			pRequest->stCompletion.stError = std::current_exception();
		}
#ifdef OI_ENABLE_BLOCKING_WAKE_TRACE
		if (pRequest->pTrace)
		{
			pRequest->pTrace->iWorkerDone = TraceNow();
		}
#endif
		pRequest->pChannel->Publish(pRequest);
	});
	if (iStatus == OI_OK)
	{
		++m_iInFlight;
	}
	return iStatus;
}

void BlockingWorkChannel::Publish(const std::shared_ptr<Request>& pRequest)
{
	::OI::Thread::NativeMutexGuard stGuard(m_stReadyLock);
	if (m_pReadyTail)
	{
		m_pReadyTail->pNext = pRequest;
		m_pReadyTail = pRequest.get();
		return;
	}
	m_pReadyHead = pRequest;
	m_pReadyTail = pRequest.get();
#ifdef OI_ENABLE_BLOCKING_WAKE_TRACE
	if (pRequest->pTrace)
	{
		pRequest->pTrace->iPost = TraceNow();
	}
#endif
	Wakeup();
}

const WorkCompletion& BlockingWorkChannel::Completion() const
{
	assert(m_pCompletion);
	return *m_pCompletion;
}

void BlockingWorkChannel::DataIn()
{
#ifdef OI_ENABLE_BLOCKING_WAKE_TRACE
	const int64_t iPoll = bTracePoll ? TraceNow() : 0;
#endif
	std::shared_ptr<Request> pReady;
	bool bBroken = false;
	{
		::OI::Thread::NativeMutexGuard stGuard(m_stReadyLock);
		bBroken = !ReadWakeup();
		pReady = std::move(m_pReadyHead);
		m_pReadyTail = nullptr;
	}
	while (pReady)
	{
		auto pRequest = std::move(pReady);
#ifdef OI_ENABLE_BLOCKING_WAKE_TRACE
		if (pRequest->pTrace)
		{
			pRequest->pTrace->iPoll = iPoll;
		}
#endif
		pReady = std::move(pRequest->pNext);
		--m_iInFlight;
		const auto* pPrevious = std::exchange(m_pCompletion, &pRequest->stCompletion);
		pSession->SwapInBuff(pRequest->stPacket);
		m_pCompletion = pPrevious;
	}
	if (bBroken && m_iInFlight == 0 && m_bAttached)
	{
		Poll::GetInstance().IOEventUnRegister(this);
		m_bAttached = false;
	}
}

}
}
