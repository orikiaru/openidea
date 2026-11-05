#include "oi_rpc_session.h"
#include "oi_rpc_call.h"
#include "oi_poll.h"
#include "oi_session_manager.h"
#include <uv.h>
#include <new>
namespace OI
{
namespace Rpc
{
ProtoSession::ProtoSession(::OI::Net::SessionManager* pMgrParam)
    : NetSession(pMgrParam), m_iFrameBytes(pMgrParam->MaxFrameBytes()), m_iFrameTimeoutMilliseconds(pMgrParam->IncompleteFrameTimeoutMilliseconds())
{
}
struct ProtoSession::PartialFrameTimer
{
	uv_timer_t stTimer;
	ProtoSession* pSession;
};

void ProtoSession::CancelIncompleteTimer()
{
	if (!m_pIncompleteTimer)
	{
		return;
	}
	auto* pTimer = m_pIncompleteTimer;
	m_pIncompleteTimer = nullptr;
	uv_timer_stop(&pTimer->stTimer);
	uv_close(reinterpret_cast<uv_handle_t*>(&pTimer->stTimer), [](uv_handle_t* pHandleParam)
	{
		auto* pTimerLocal = static_cast<PartialFrameTimer*>(pHandleParam->data);
		pTimerLocal->pSession->Release();
		delete pTimerLocal;
	});
}

void ProtoSession::ArmIncompleteTimer()
{
	// 同一个残包的期限不随新字节延长，避免滴流一直占用接收内存。
	if (m_pIncompleteTimer || IsClosed())
	{
		return;
	}
	auto* pTimer = new (std::nothrow) PartialFrameTimer{};
	if (!pTimer)
	{
		PassiveClose(ENOBUFS);
		return;
	}
	if (uv_timer_init(OI::Net::Poll::GetInstance().Loop(), &pTimer->stTimer) != 0)
	{
		delete pTimer;
		PassiveClose(ENOBUFS);
		return;
	}
	pTimer->pSession = this;
	pTimer->stTimer.data = pTimer;
	AddRef(); // uv_close 完成前，计时器持有会话。
	m_pIncompleteTimer = pTimer;
	if (uv_timer_start(&pTimer->stTimer, [](uv_timer_t* pHandleParam)
	{
		auto* pSessionLocal = static_cast<PartialFrameTimer*>(pHandleParam->data)->pSession;
		pSessionLocal->CancelIncompleteTimer();
		pSessionLocal->PassiveClose(ETIMEDOUT);
	}, m_iFrameTimeoutMilliseconds, 0) != 0)
	{
		CancelIncompleteTimer();
		PassiveClose(ENOBUFS);
	}
}

void ProtoSession::OnClose()
{
	CancelIncompleteTimer();
	NetSession::OnClose();
}

void ProtoSession::OnDataIn()
{
	// 一包就是 协议id + 总大小 + 序列化结构。对不上或者解析失败，丢掉这一包。
	// 借 _ibuff 的内存原地解析，不整块拷一份。
	::OI::Base::BytesStream stOutputStream;
	stOutputStream.Swap(stInputBuffer);
	while (stOutputStream.RemainSize())
	{
		if (stOutputStream.RemainSize() >= sizeof(uint32_t) * 2)
		{
			uint32_t iDeclaredSize = 0;
			memcpy(&iDeclaredSize, stOutputStream.GetCurPtr() + sizeof(uint32_t), sizeof(iDeclaredSize));
			if (iDeclaredSize > m_iFrameBytes)
			{
				PassiveClose(EMSGSIZE);
				break;
			}
		}
		stOutputStream.StartTransaction();
		uint32_t iIdLocal = 0;
		uint32_t iSize = 0;
		::OI::Base::PbHeaderState iSt = stOutputStream.PreparePbHeader(iIdLocal, iSize);
		if (iSt == ::OI::Base::PB_HEADER_NEED_MORE)
		{
			stOutputStream.RollBack();
			break;
		}
		if (iSt != ::OI::Base::PB_HEADER_OK)
		{
			PassiveClose(PARSER_ERR_OTHER);
			break;
		}
		::OI::Base::BytesStream stBody;
		CancelIncompleteTimer();
		if (iSize > 0)
		{
			stBody.Append(stOutputStream.GetCurPtr(), iSize);
		}
		stOutputStream.AddCurPos(iSize);
		DeliverRpc((proto_type_t)iIdLocal, stBody, pSessionManager, iSessionId);
	}
	stInputBuffer.Swap(stOutputStream);
	stInputBuffer.Erase(0, stOutputStream.GetCurPos());
	if (IsClosed())
	{
		CancelIncompleteTimer();
		stInputBuffer.Clear();
	}
	else if (stInputBuffer.Size() != 0)
	{
		ArmIncompleteTimer();
	}
	else
	{
		CancelIncompleteTimer();
	}
}

void ProtoSession::OnDataOut()
{
}
}
}
