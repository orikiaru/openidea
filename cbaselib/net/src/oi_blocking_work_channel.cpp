#include "oi_blocking_work_channel.h"
#include "oi_session.h"
#include "oi_poll.h"
#include <memory>
#include <stdexcept>
#include <utility>
#include <uv.h>

namespace OI
{
namespace Net
{
struct BlockingWorkChannel::Request
{
	BlockingWorkChannel* pChannel;
	WorkCompletion stCompletion;
	OI::Base::Bytes stPacket;
	std::function<void()> fnWork;
};

BlockingWorkChannel::BlockingWorkChannel(Session* pSessionLocal)
    : Channel(NetSocket{}, pSessionLocal)
{
}

int32_t BlockingWorkChannel::Submit(OI::Thread::BlockingType iType, uint64_t iOperation, std::function<void()> fnWork, OI::Base::Bytes stPacket)
{
	assert(Poll::GetInstance().InLoopThread());
	auto pRequest = std::make_shared<Request>();
	pRequest->pChannel = this;
	pRequest->stCompletion.iOperation = iOperation;
	pRequest->stPacket = std::move(stPacket);
	pRequest->fnWork = std::move(fnWork);
	// 提前分配通知保活对象，避免执行完之后分配失败丢失完成通知。
	struct Notification
	{
		Poll::QueuedCall stCall;
		std::shared_ptr<Request> pRequest;
	};
	auto pNotification = std::make_unique<Notification>();
	pNotification->pRequest = pRequest;
	pNotification->stCall.pArgument = pNotification.get();
	pNotification->stCall.fnCall = [](void* pValue)
	{
		std::unique_ptr<Notification> pOwned(static_cast<Notification*>(pValue));
		auto& pRequestLocal = pOwned->pRequest;
		auto* pChannelLocal = pRequestLocal->pChannel;
		const auto* pPrevious = std::exchange(pChannelLocal->m_pCompletion, &pRequestLocal->stCompletion);
		auto* pPreviousPacket = std::exchange(pChannelLocal->m_pPacket, &pRequestLocal->stPacket);
		pChannelLocal->DataIn();
		pChannelLocal->m_pPacket = pPreviousPacket;
		pChannelLocal->m_pCompletion = pPrevious;
	};
	auto* pArgument = pNotification.get();
	const int32_t iStatus = OI::Thread::SubmitBlockingWork(iType, [pRequest, pArgument]()
	{
		try
		{
			pRequest->fnWork();
		}
		catch (...)
		{
			pRequest->stCompletion.stError = std::current_exception();
		}
		Poll::GetInstance().PostOnLoop(pArgument->stCall);
	});
	if (iStatus == OI_OK)
	{
		pNotification.release();
	}
	return iStatus;
}

const WorkCompletion& BlockingWorkChannel::Completion() const
{
	assert(m_pCompletion);
	return *m_pCompletion;
}

void BlockingWorkChannel::DataIn()
{
	pSession->SwapInBuff(*m_pPacket);
}

void BlockingWorkChannel::DataOut()
{
}

bool BlockingWorkChannel::TryClose()
{
	// 管理器排空全部请求后才销毁会话及通道。
	return false;
}

int32_t BlockingWorkChannel::AllowSend()
{
	return OI_ERR_INVALID_ARGUMENT;
}
}
}
