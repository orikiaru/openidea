#include "ipcserver.h"
#include "oi_rpcdata_allechoreq.h"
#include "oi_rpcdata_allechores.h"
#include "oi_rpcdata_emptyreq.h"
#include "oi_rpcdata_emptyres.h"
#include "oi_rpcdata_holdreq.h"
#include "oi_rpcdata_holdres.h"
#include "oi_lock.h"
#include "oi_rpcdata_peernotereq.h"
#include "oi_parser.h"
#include "oi_rpc_call.h"
#include "oi_rpc_session_manager.h"
#include "rpc_ids.h"
#include "oi_config.h"
#include "oi_http.h"
#include "oi_log.h"
#include <chrono>
#include <stdio.h>
#include <string>

IpcServer::~IpcServer()
{
}

const std::string IpcServer::GetIDString()
{
	return "IpcServer";
}

static int32_t HandleAllEcho(const OI::RpcData::AllEchoReq& stRequest, OI::RpcData::AllEchoRes& stResponse)
{
	stResponse.stData = stRequest.stData;
	stResponse.iCode = 1;
	return 0;
}

static int32_t HandleEmpty(const OI::RpcData::EmptyReq& stRequest, OI::RpcData::EmptyRes& stResponse)
{
	(void)stRequest;
	(void)stResponse;
	return 0;
}

static OI::Rpc::RpcTask HandleHold(const OI::RpcData::HoldReq& stRequest, OI::RpcData::HoldRes& stResponse)
{
	if (stRequest.iDelayMilliseconds < 0)
	{
		co_return 1001; // 业务错误：延迟不能为负，回执不带响应体。
	}
	if (stRequest.iDelayMilliseconds > 0)
	{
		co_await OI::Coro::yield(stRequest.iDelayMilliseconds);
	}
	stResponse.iMark = stRequest.iMark;
	if (stRequest.iMark == 802 || stRequest.iMark == 804)
	{
		std::printf("hold late event mark=%d\n", stRequest.iMark);
		std::fflush(stdout);
	}
	co_return 0;
}

// 仅由回归配置启用，在写入部分响应后注入序列化异常。
struct EncodingFailureResponse : OI::RpcData::HoldRes
{
	bool bThrow = false;
	bool bMalformed = false;
	OI::Base::BytesStream& serialize(OI::Base::BytesStream& stOutput) const override
	{
		if (bMalformed)
		{
			// 外层 RPC 回执合法，但字段头声明四字节，实际只有一个字节。
			stOutput << uint32_t(0) << uint32_t(4) << uint8_t(1);
			return stOutput;
		}
		if (bThrow)
		{
			stOutput << int32_t(123);
			throw std::runtime_error("injected server response encoding failure");
		}
		return HoldRes::serialize(stOutput);
	}
};
static OI::Rpc::RpcTask HandleEncodingRegression(const OI::Rpc::RpcContext& stContext, const OI::RpcData::HoldReq& stRequest, EncodingFailureResponse& stResponse)
{
	if (stRequest.iMark == -2147483598)
	{
		// 先回原请求，让客户端释放出站租约；反向 RPC 独立挂起。
		OI::Coro::StartTask([pManager = &stContext.stManager, iSessionId = stContext.iSessionId](OI::Coro::Token) -> OI::Coro::Task
		{
			OI::RpcData::HoldReq stReverse;
			stReverse.iMark = -2147483598;
			stReverse.iDelayMilliseconds = 1000;
			OI::RpcData::HoldRes stReply;
			const int32_t iRet = co_await OI::Rpc::RpcCall(*pManager, iSessionId, stReverse, stReply, OI::Timer::Seconds(5));
			LOG_TRACE("hold reverse reply bad=%d", iRet != 0 || stReply.iMark != stReverse.iMark ? 1 : 0);
			fflush(stdout);
		});
	}
	if (stRequest.iMark == -2147483599)
	{
		throw std::runtime_error("injected server handler failure");
	}
	int32_t iResult = co_await HandleHold(stRequest, stResponse);
	stResponse.bThrow = stRequest.iMark == -2147483600;
	stResponse.bMalformed = stRequest.iMark == -2147483597;
	co_return iResult;
}

int32_t IpcServer::NoteCount(int32_t iTag)
{
	::OI::Thread::MutexGuard stLock(m_stNoteLocker);
	auto it = m_mNoteCount.find(iTag);
	if (it == m_mNoteCount.end())
	{
		return 0;
	}
	return it->second;
}

int32_t IpcServer::HandlePeerNote(const ::OI::RpcData::PeerNoteReq& stRequest)
{
	::OI::Thread::MutexGuard stLock(m_stNoteLocker);
	++m_mNoteCount[stRequest.iTag];
	return OI_OK;
}

::OI::Rpc::RpcTask IpcServer::HandlePeerProbe(const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::PeerProbeReq& stRequest,
    ::OI::RpcData::PeerProbeRes& stResponse)
{
	if (stRequest.iNotify != 0)
	{
		::OI::RpcData::PeerNoteReq stNote;
		stNote.iTag = stRequest.iTag;
		stNote.iServiceId = m_iServiceId;
		const int32_t iRet = stContext.Notify(stNote);
		if (iRet != OI_OK)
		{
			co_return iRet;
		}
	}
	stResponse.iTag = stRequest.iTag;
	stResponse.iServiceId = m_iServiceId;
	stResponse.iNoteCount = NoteCount(stRequest.iTag);
	co_return 0;
}

int32_t IpcServer::Init()
{
	bool bInitFail = false;
	TryGetConfig("Regression.rpc_init_fail", bInitFail);
	if (bInitFail)
	{
		return OI_ERR_FAIL;
	}
	TryGetConfig("Service.id", m_iServiceId);
	RegisterRpcHandler(HandleAllEcho);
	bool bDuplicateRegression = false;
	TryGetConfig("Regression.rpc_duplicate_handler", bDuplicateRegression);
	if (bDuplicateRegression)
	{
		RegisterRpcHandler(HandleAllEcho);
	}
	RegisterRpcHandler(HandleEmpty);
	RegisterRpcHandler(&IpcServer::HandlePeerProbe);
	RegisterNotifyHandler(&IpcServer::HandlePeerNote);
	bool bEncodingRegression = false;
	TryGetConfig("Regression.encoding_failure", bEncodingRegression);
	if (bEncodingRegression)
	{
		BindRpcHandler(OI::Rpc::RpcProtoId<OI::RpcData::HoldReq>::iValue,
		    OI::Rpc::RpcHandlerBuilder::Make<OI::RpcData::HoldReq, EncodingFailureResponse>(HandleEncodingRegression,
		        OI::Rpc::RpcProtoId<OI::RpcData::HoldRes>::iValue));
	}
	else
	{
		RegisterRpcHandler(HandleHold);
	}
	bool bReadLimitRegression = false;
	TryGetConfig("Regression.rpc_read_limit", bReadLimitRegression);
	if (bReadLimitRegression)
	{
		BindRpcHandler(31000, [](OI::Base::BytesStream& stBody, OI::Rpc::RpcDelivery&&)
		{
			uint8_t iPart = 0;
			stBody >> iPart;
		});
	}
	return ::OI::Rpc::ProtoSessionManager::Init();
}

static std::string HtmlTitle(const std::string& sHtml)
{
	size_t iOpen = sHtml.find("<title>");
	if (iOpen == std::string::npos)
	{
		return "";
	}
	iOpen += 7;
	size_t iClose = sHtml.find("</title>", iOpen);
	if (iClose == std::string::npos)
	{
		return "";
	}
	return sHtml.substr(iOpen, iClose - iOpen);
}

static OI::Coro::Task HttpProbe(std::string sHost, int32_t iPort, std::string sPath, int32_t iTimeoutMilliseconds)
{
	::OI::Http::Client stCli(sHost, iPort, iTimeoutMilliseconds);
	std::chrono::steady_clock::time_point stBegin = std::chrono::steady_clock::now();
	::OI::Http::Response stResponseLocal = co_await stCli.Get(sPath, {});
	long long iMillisecondsLocal = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stBegin).count();
	printf("http probe host=%s port=%d path=%s status=%d bytes=%zu ms=%lld code=%d err=%s title=%s\n",
	    sHost.c_str(), iPort, sPath.c_str(), stResponseLocal.iStatus, stResponseLocal.sBody.size(), iMillisecondsLocal, stResponseLocal.iCode,
	    stResponseLocal.sError.c_str(), HtmlTitle(stResponseLocal.sBody).c_str());
	fflush(stdout);
}

void IpcServer::StartHttpProbe()
{
	std::string sHostLocal;
	int32_t iPortLocal = 80;
	std::string sPath = "/";
	int32_t iTimeoutMillisecondsLocal = 5000;
	TryGetConfig("HttpProbe.host", sHostLocal);
	TryGetConfig("HttpProbe.port", iPortLocal);
	TryGetConfig("HttpProbe.path", sPath);
	TryGetConfig("HttpProbe.timeout_ms", iTimeoutMillisecondsLocal);
	if (sHostLocal.empty())
	{
		return;
	}
	OI::Coro::StartTask([sHostLocal, iPortLocal, sPath, iTimeoutMillisecondsLocal](OI::Coro::Token)
	{
		return HttpProbe(sHostLocal, iPortLocal, sPath, iTimeoutMillisecondsLocal);
	});
}

void IpcServer::OnAddSession(::OI::Net::Session* pSession)
{
	printf("ipc server session %d\n", pSession->GetSessionID());
	fflush(stdout);
}

void IpcServer::OnDelSession(const ::OI::Net::Session* pSession)
{
	(void)pSession;
}
