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
	uv_work_t stWork;
	BlockingWorkChannel* pChannel;
	WorkCompletion stCompletion;
	OI::Base::Bytes stPacket;
	std::function<void()> fnWork;
};

BlockingWorkChannel::BlockingWorkChannel(Session* pSessionLocal)
    : Channel(NetSocket{}, pSessionLocal)
{
}

void BlockingWorkChannel::Submit(uint64_t iOperation, std::function<void()> fnWork, OI::Base::Bytes stPacket)
{
	assert(Poll::GetInstance().InLoopThread());
	auto pRequest = std::make_unique<Request>();
	pRequest->pChannel = this;
	pRequest->stCompletion.iOperation = iOperation;
	pRequest->stPacket = std::move(stPacket);
	pRequest->fnWork = std::move(fnWork);
	pRequest->stWork.data = pRequest.get();
	const int iStatus = uv_queue_work(Poll::GetInstance().Loop(), &pRequest->stWork, [](uv_work_t* pWork)
	{
		auto* pRequestLocal = static_cast<Request*>(pWork->data);
		try
		{
			pRequestLocal->fnWork();
		}
		catch (...)
		{
			pRequestLocal->stCompletion.stError = std::current_exception();
		}
	}, [](uv_work_t* pWork, int iStatusLocal)
	{
		std::unique_ptr<Request> pRequestLocal(static_cast<Request*>(pWork->data));
		if (iStatusLocal != 0)
		{
			try
			{
				throw std::runtime_error(std::string("background work completion: ") + uv_strerror(iStatusLocal));
			}
			catch (...)
			{
				pRequestLocal->stCompletion.stError = std::current_exception();
			}
		}
		auto* pChannelLocal = pRequestLocal->pChannel;
		const auto* pPrevious = std::exchange(pChannelLocal->m_pCompletion, &pRequestLocal->stCompletion);
		auto* pPreviousPacket = std::exchange(pChannelLocal->m_pPacket, &pRequestLocal->stPacket);
		pChannelLocal->DataIn();
		pChannelLocal->m_pPacket = pPreviousPacket;
		pChannelLocal->m_pCompletion = pPrevious;
	});
	if (iStatus != 0)
	{
		throw std::runtime_error(std::string("background work submit: ") + uv_strerror(iStatus));
	}
	pRequest.release();
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
