#include "oi_http.h"
#include "oi_bytes.h"
#include "oi_rpc_coro.h"
#include "oi_net_session.h"
#include "oi_poll.h"
#include "oi_session_manager.h"
#include "oi_timer_manager.h"
#include "uv.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <stdexcept>
#include <utility>

namespace OI
{
namespace Http
{

namespace
{
const size_t kMaxHeader = 65536;
const size_t kMaxBody = 8 * 1024 * 1024;

std::string EncodeQuery(const std::string& sValueParam)
{
	static const char* pHex = "0123456789ABCDEF";
	std::string sOutput;
	sOutput.reserve(sValueParam.size());
	for (size_t iIndexLocal = 0; iIndexLocal < sValueParam.size(); ++iIndexLocal)
	{
		unsigned char iCharacter = (unsigned char)sValueParam[iIndexLocal];
		bool bPlain = (iCharacter >= 'a' && iCharacter <= 'z') || (iCharacter >= 'A' && iCharacter <= 'Z') || (iCharacter >= '0' && iCharacter <= '9') || iCharacter == '-' || iCharacter == '_' || iCharacter == '.' || iCharacter == '!' || iCharacter == '~' || iCharacter == '*' || iCharacter == '\'' || iCharacter == '(' || iCharacter == ')';
		if (bPlain)
		{
			sOutput.push_back((char)iCharacter);
		}
		else
		{
			sOutput.push_back('%');
			sOutput.push_back(pHex[iCharacter >> 4]);
			sOutput.push_back(pHex[iCharacter & 15]);
		}
	}
	return sOutput;
}

bool EqName(const std::string& sTextParam, size_t iPositionParam, size_t iEndParam, const char* pNameParam)
{
	size_t iNumber = std::strlen(pNameParam);
	if (iPositionParam + iNumber > iEndParam)
	{
		return false;
	}
	for (size_t iIndexLocal = 0; iIndexLocal < iNumber; ++iIndexLocal)
	{
		char iCharacter = sTextParam[iPositionParam + iIndexLocal];
		if (iCharacter >= 'A' && iCharacter <= 'Z')
		{
			iCharacter = (char)(iCharacter - 'A' + 'a');
		}
		if (iCharacter != pNameParam[iIndexLocal])
		{
			return false;
		}
	}
	return true;
}

bool ParseU64(const std::string& sTextParam, size_t iPositionParam, size_t iEndParam, uint64_t& iOutputParam)
{
	if (iPositionParam >= iEndParam || sTextParam[iPositionParam] < '0' || sTextParam[iPositionParam] > '9')
	{
		return false;
	}
	uint64_t iValue = 0;
	for (size_t iIndexLocal = iPositionParam; iIndexLocal < iEndParam; ++iIndexLocal)
	{
		char iCharacter = sTextParam[iIndexLocal];
		if (iCharacter < '0' || iCharacter > '9')
		{
			return false;
		}
		if (iValue > (UINT64_MAX - uint64_t(iCharacter - '0')) / 10)
		{
			return false;
		}
		iValue = iValue * 10 + (uint64_t)(iCharacter - '0');
	}
	iOutputParam = iValue;
	return true;
}

// chunk 行里分号后面是扩展，忽略。
bool ParseHex(const std::string& sTextParam, size_t iPositionParam, size_t iEndParam, uint64_t& iOutputParam)
{
	uint64_t iValue = 0;
	size_t iDigits = 0;
	for (size_t iIndexLocal = iPositionParam; iIndexLocal < iEndParam; ++iIndexLocal)
	{
		char iCharacter = sTextParam[iIndexLocal];
		int iD = -1;
		if (iCharacter >= '0' && iCharacter <= '9')
		{
			iD = iCharacter - '0';
		}
		else if (iCharacter >= 'a' && iCharacter <= 'f')
		{
			iD = iCharacter - 'a' + 10;
		}
		else if (iCharacter >= 'A' && iCharacter <= 'F')
		{
			iD = iCharacter - 'A' + 10;
		}
		if (iD < 0)
		{
			if (iCharacter == ';' && iDigits > 0)
			{
				break;
			}
			return false;
		}
		if (iValue > (UINT64_MAX >> 4))
		{
			return false;
		}
		iValue = (iValue << 4) | (uint64_t)iD;
		iDigits += 1;
	}
	iOutputParam = iValue;
	return iDigits > 0;
}

std::string HostHeader(const std::string& sHostParam, int32_t iPortParam)
{
	std::string sLine = "Host: ";
	sLine += sHostParam;
	if (iPortParam != 80)
	{
		sLine += ":";
		sLine += std::to_string(iPortParam);
	}
	sLine += "\r\n";
	return sLine;
}
}

class HttpSession;

struct Call::Request
{
	std::shared_ptr<Client::Impl> pImpl; // 所属连接。Client 析构后由它留到结果发布。
	std::string sRequest;                // 待发报文。发布结果前释放，晚到的超时不再留着大包。
	std::string sBody;                   // 正文。exchange(Ready) 之前写完。
	std::string sError;                  // 失败原因。成功时为空。
	std::coroutine_handle<> stHandle;    // 调用方句柄。门已经是 Waiting 才 Schedule。
	std::atomic<coro::Gate> iGate;       // 完成门。Idle 还没人认领，Ready 结果已写好，Waiting 调用方已挂起。
	std::atomic<int32_t> iDone;          // 结果只发布一次。晚到的超时看见非 0 就返回。
	std::atomic<bool> bAwaitStarted;     // 一次性闩。第二次 await 或移走后再等直接抛。
	int32_t iStatus;                     // HTTP 状态码。没拿到状态行是 0。
	int32_t iTimeoutMilliseconds;        // 从入队起算的超时。
	bool bFailed;                        // 连接、超时或报文失败。
	~Request();

	Request()
	    : iGate(coro::Gate::Idle), iDone(0), bAwaitStarted(false), iStatus(0), iTimeoutMilliseconds(5000), bFailed(false)
	{
	}
};

struct Client::Impl : public ::OI::Net::SessionManager
{
  private:
	std::string m_sHost;
	int32_t m_iPort;
	int32_t m_iTimeoutMilliseconds;
	std::deque<std::shared_ptr<Call::Request>> m_vQueue;
	std::shared_ptr<Call::Request> m_pFlight;
	HttpSession* m_pSession;
	std::shared_ptr<Impl> m_pDnsHold;
	std::weak_ptr<Impl> m_pSelf;
	uv_getaddrinfo_t m_stDns;
	sid_t m_iSessionId;
	bool m_bConnected;
	bool m_bConnecting;
	bool m_bDnsPending;
	bool m_bClosed;
	bool m_bReuse;

	void Publish(const std::shared_ptr<Call::Request>& pRequestParam, bool bFailParam, const char* pWhyParam);
	void EnqueueOnLoop(const std::shared_ptr<Call::Request>& pRequestParam);
	void ArmTimer(const std::shared_ptr<Call::Request>& pRequestParam);
	void Pump();
	void BeginConnect();
	void UseAddress(const std::string& sIpParam);
	void FailFront(const char* pWhyParam);
	static void OnDns(uv_getaddrinfo_t* pRequestParam, int iStatusParam, struct addrinfo* pResponseParam);

  public:
	Impl(const std::string& sHostParam, int32_t iPortParam, int32_t iTimeoutMillisecondsParam, bool bReuseParam);
	~Impl() override;
	const std::string GetIDString() override;
	int32_t Timeout() const
	{
		return m_iTimeoutMilliseconds;
	}
	bool Reuse() const
	{
		return m_bReuse;
	}
	const std::string& Host() const
	{
		return m_sHost;
	}
	int32_t Port() const
	{
		return m_iPort;
	}
	void Attach(const std::shared_ptr<Impl>& pSelfParam)
	{
		m_pSelf = pSelfParam;
	}
	std::shared_ptr<Impl> Hold()
	{
		return m_pSelf.lock();
	}
	void Enqueue(const std::shared_ptr<Call::Request>& pRequestParam);
	void OnTimeout(const std::shared_ptr<Call::Request>& pRequestParam);
	void Shutdown();
	void OnHttpFinished(HttpSession* pSessionParam, bool bFailParam, const char* pWhyParam, bool bCloseParam, int32_t iStatusParam, std::string sBodyParam);
	void OnIdleBytes(HttpSession* pSessionParam);

  protected:
	::OI::Net::Session* CloneSession() override;
	void OnAddSession(::OI::Net::Session* pSessionParam) override;
	void OnDelSession(const ::OI::Net::Session* pSessionParam) override;
	void OnConnectFailed() override;
	void OnSessionClosed(sid_t iSessionIdParam) override;
};

class HttpSession : public ::OI::Net::NetSession
{
	std::shared_ptr<Client::Impl> m_pImpl;
	std::string m_sIncoming;
	std::string m_sBody;
	size_t m_iHeaderBytes;
	uint64_t m_iContentLength;
	size_t m_iChunkPosition;
	unsigned m_iInterimResponses;
	int32_t m_iStatus;
	bool m_bHeadersReady;
	bool m_bHasLength;
	bool m_bChunked;
	bool m_bCloseAfter;
	bool m_bActive;

	void ReleaseIncoming();
	void NoteConnection(const std::string& sValueParam);
	bool ParseHeaders(size_t iHeaderEndParam);
	bool ConsumeChunks();
	bool Consume();
	void Succeed(size_t iConsumedParam);
	void Fail(const char* pWhyParam);

  public:
	explicit HttpSession(const std::shared_ptr<Client::Impl>& pImplParam);
	~HttpSession() override;
	void BeginResponse();
	void Abandon();
	void FinishEof();
	void OnDataIn() override;
	void OnDataOut() override;
	void OnClose() override;
};

struct LoopRequest
{
	std::shared_ptr<Client::Impl> pImpl;
	std::shared_ptr<Call::Request> pRequest;
};

Call::Request::~Request() = default;
Client::Impl::~Impl() = default;
HttpSession::~HttpSession() = default;

HttpSession::HttpSession(const std::shared_ptr<Client::Impl>& pImplParam)
    : ::OI::Net::NetSession(pImplParam.get()), m_pImpl(pImplParam), m_iHeaderBytes(0), m_iContentLength(0), m_iChunkPosition(0), m_iInterimResponses(0), m_iStatus(0), m_bHeadersReady(false), m_bHasLength(false), m_bChunked(false), m_bCloseAfter(false), m_bActive(false)
{
}

void HttpSession::ReleaseIncoming()
{
	std::string().swap(m_sIncoming);
	::OI::Base::Bytes stEmpty;
	stInputBuffer.Swap(stEmpty);
}

void HttpSession::BeginResponse()
{
	m_sBody.clear();
	m_iHeaderBytes = 0;
	m_iContentLength = 0;
	m_iChunkPosition = 0;
	m_iInterimResponses = 0;
	m_iStatus = 0;
	m_bHeadersReady = false;
	m_bHasLength = false;
	m_bChunked = false;
	m_bCloseAfter = false;
	m_bActive = true;
	ReleaseIncoming();
}

void HttpSession::Abandon()
{
	m_bActive = false;
	ReleaseIncoming();
}

void HttpSession::NoteConnection(const std::string& sValueParam)
{
	bool bClose = false;
	bool bKeep = false;
	size_t iPosition = 0;
	while (iPosition < sValueParam.size())
	{
		while (iPosition < sValueParam.size() && (sValueParam[iPosition] == ' ' || sValueParam[iPosition] == '\t' || sValueParam[iPosition] == ','))
		{
			iPosition += 1;
		}
		if (iPosition >= sValueParam.size())
		{
			break;
		}
		size_t iEnd = iPosition;
		while (iEnd < sValueParam.size() && sValueParam[iEnd] != ',')
		{
			iEnd += 1;
		}
		size_t iTokenEnd = iEnd;
		while (iTokenEnd > iPosition && (sValueParam[iTokenEnd - 1] == ' ' || sValueParam[iTokenEnd - 1] == '\t'))
		{
			iTokenEnd -= 1;
		}
		if (EqName(sValueParam, iPosition, iTokenEnd, "close") && iPosition + std::strlen("close") == iTokenEnd)
		{
			bClose = true;
		}
		else if (EqName(sValueParam, iPosition, iTokenEnd, "keep-alive") && iPosition + std::strlen("keep-alive") == iTokenEnd)
		{
			bKeep = true;
		}
		if (iEnd >= sValueParam.size())
		{
			break;
		}
		iPosition = iEnd + 1;
	}
	if (bClose)
	{
		m_bCloseAfter = true;
	}
	else if (bKeep)
	{
		m_bCloseAfter = false;
	}
}

bool HttpSession::ParseHeaders(size_t iHeaderEndParam)
{
	size_t iLineEnd = m_sIncoming.find("\r\n");
	if (iLineEnd == std::string::npos || iLineEnd > iHeaderEndParam)
	{
		return false;
	}
	if (iLineEnd < 12 || (m_sIncoming.compare(0, 9, "HTTP/1.1 ") != 0 && m_sIncoming.compare(0, 9, "HTTP/1.0 ") != 0))
	{
		return false;
	}
	if (iLineEnd > 12 && m_sIncoming[12] != ' ')
	{
		return false;
	}
	uint64_t iCodeLocal = 0;
	if (!ParseU64(m_sIncoming, 9, 12, iCodeLocal) || iCodeLocal < 100 || iCodeLocal > 599 || iCodeLocal == 101)
	{
		return false;
	}
	m_iStatus = static_cast<int32_t>(iCodeLocal);
	m_bHasLength = false;
	m_bChunked = false;
	m_iContentLength = 0;
	m_bCloseAfter = m_sIncoming.compare(0, 9, "HTTP/1.0 ") == 0;
	size_t iPosition = iLineEnd + 2;
	while (iPosition < iHeaderEndParam)
	{
		size_t iEnd = m_sIncoming.find("\r\n", iPosition);
		if (iEnd == std::string::npos || iEnd > iHeaderEndParam)
		{
			break;
		}
		size_t iColon = m_sIncoming.find(':', iPosition);
		if (iColon != std::string::npos && iColon < iEnd)
		{
			size_t iValue = iColon + 1;
			while (iValue < iEnd && (m_sIncoming[iValue] == ' ' || m_sIncoming[iValue] == '\t'))
			{
				iValue += 1;
			}
			size_t iValueEnd = iEnd;
			while (iValueEnd > iValue && (m_sIncoming[iValueEnd - 1] == ' ' || m_sIncoming[iValueEnd - 1] == '\t'))
			{
				--iValueEnd;
			}
			size_t iNameEnd = iColon;
			while (iNameEnd > iPosition && m_sIncoming[iNameEnd - 1] == ' ')
			{
				iNameEnd -= 1;
			}
			if (EqName(m_sIncoming, iPosition, iNameEnd, "content-length") && iPosition + std::strlen("content-length") == iNameEnd)
			{
				uint64_t iLength = 0;
				if (!ParseU64(m_sIncoming, iValue, iValueEnd, iLength) || (m_iStatus != 304 && m_iStatus >= 200 && iLength > kMaxBody) || (m_bHasLength && m_iContentLength != iLength))
				{
					return false;
				}
				m_bHasLength = true;
				m_iContentLength = iLength;
			}
			else if (EqName(m_sIncoming, iPosition, iNameEnd, "transfer-encoding") && iPosition + std::strlen("transfer-encoding") == iNameEnd)
			{
				std::string sCoding = m_sIncoming.substr(iValue, iValueEnd - iValue);
				for (size_t iIndexLocal = 0; iIndexLocal < sCoding.size(); ++iIndexLocal)
				{
					if (sCoding[iIndexLocal] >= 'A' && sCoding[iIndexLocal] <= 'Z')
					{
						sCoding[iIndexLocal] = (char)(sCoding[iIndexLocal] - 'A' + 'a');
					}
				}
				if (m_bChunked || sCoding != "chunked")
				{
					return false;
				}
				m_bChunked = true;
			}
			else if (EqName(m_sIncoming, iPosition, iNameEnd, "connection") && iPosition + std::strlen("connection") == iNameEnd)
			{
				NoteConnection(m_sIncoming.substr(iValue, iValueEnd - iValue));
			}
		}
		iPosition = iEnd + 2;
	}
	return !(m_bHasLength && m_bChunked);
}

bool HttpSession::ConsumeChunks()
{
	for (;;)
	{
		size_t iLineEnd = m_sIncoming.find("\r\n", m_iChunkPosition);
		if (iLineEnd == std::string::npos)
		{
			if (m_sIncoming.size() - m_iChunkPosition > 1024)
			{
				Fail("chunk");
				return true;
			}
			return false;
		}
		uint64_t iSize = 0;
		if (!ParseHex(m_sIncoming, m_iChunkPosition, iLineEnd, iSize))
		{
			Fail("chunk");
			return true;
		}
		size_t iDataAt = iLineEnd + 2;
		if (iSize == 0)
		{
			size_t iPosition = iDataAt;
			for (;;)
			{
				size_t iEnd = m_sIncoming.find("\r\n", iPosition);
				if (iEnd == std::string::npos)
				{
					return false;
				}
				if (iEnd == iPosition)
				{
					Succeed(iEnd + 2);
					return true;
				}
				iPosition = iEnd + 2;
			}
		}
		if (iSize > kMaxBody || m_sBody.size() + (size_t)iSize > kMaxBody)
		{
			Fail("size");
			return true;
		}
		if (m_sIncoming.size() < iDataAt + (size_t)iSize + 2)
		{
			return false;
		}
		if (m_sIncoming.compare(iDataAt + static_cast<size_t>(iSize), 2, "\r\n") != 0)
		{
			Fail("chunk");
			return true;
		}
		m_sBody.append(m_sIncoming, iDataAt, (size_t)iSize);
		m_iChunkPosition = iDataAt + (size_t)iSize + 2;
	}
}

bool HttpSession::Consume()
{
	while (!m_bHeadersReady)
	{
		size_t iHeaderEnd = m_sIncoming.find("\r\n\r\n");
		if (iHeaderEnd == std::string::npos)
		{
			if (m_sIncoming.size() > kMaxHeader)
			{
				Fail("header");
				return true;
			}
			return false;
		}
		if (iHeaderEnd > kMaxHeader || !ParseHeaders(iHeaderEnd))
		{
			Fail("status");
			return true;
		}
		if (m_iStatus < 200)
		{
			if (++m_iInterimResponses > 16)
			{
				Fail("status");
				return true;
			}
			m_sIncoming.erase(0, iHeaderEnd + 4);
			continue;
		}
		m_bHeadersReady = true;
		m_iHeaderBytes = iHeaderEnd + 4;
		m_iChunkPosition = m_iHeaderBytes;
	}
	if (m_iStatus == 204 || m_iStatus == 304)
	{
		Succeed(m_iHeaderBytes);
		return true;
	}
	if (m_bChunked)
	{
		return ConsumeChunks();
	}
	if (!m_bHasLength)
	{
		m_bCloseAfter = true;
		return false;
	}
	if (m_sIncoming.size() - m_iHeaderBytes < m_iContentLength)
	{
		return false;
	}
	m_sBody.assign(m_sIncoming, m_iHeaderBytes, (size_t)m_iContentLength);
	Succeed(m_iHeaderBytes + (size_t)m_iContentLength);
	return true;
}

void HttpSession::Succeed(size_t iConsumedParam)
{
	if (!m_bActive)
	{
		return;
	}
	// 同一段缓冲里还剩下一响没用到的字节。丢掉后续复用会让下一条请求从半截报文开始解析。
	if (iConsumedParam < m_sIncoming.size())
	{
		m_bCloseAfter = true;
	}
	m_bActive = false;
	int32_t iStatusLocal = m_iStatus;
	bool bCloseLocal = m_bCloseAfter;
	std::string sBodyLocal = std::move(m_sBody);
	ReleaseIncoming();
	m_pImpl->OnHttpFinished(this, false, NULL, bCloseLocal, iStatusLocal, std::move(sBodyLocal));
}

void HttpSession::Fail(const char* pWhyParam)
{
	if (!m_bActive)
	{
		return;
	}
	m_bActive = false;
	int32_t iStatusLocal = m_iStatus;
	ReleaseIncoming();
	m_pImpl->OnHttpFinished(this, true, pWhyParam, true, iStatusLocal, std::string());
}

void HttpSession::FinishEof()
{
	if (!m_bActive)
	{
		return;
	}
	if (Consume())
	{
		return;
	}
	if (!m_bHeadersReady || m_bHasLength || m_bChunked)
	{
		Fail("eof");
		return;
	}
	m_sBody.assign(m_sIncoming, m_iHeaderBytes, std::string::npos);
	m_bCloseAfter = true;
	Succeed(m_sIncoming.size());
}

void HttpSession::OnDataIn()
{
	if (!m_bActive)
	{
		if (stInputBuffer.Size() > 0)
		{
			::OI::Base::Bytes stEmpty;
			stInputBuffer.Swap(stEmpty);
			m_pImpl->OnIdleBytes(this);
		}
		return;
	}
	if (m_sIncoming.size() + stInputBuffer.Size() > kMaxHeader + kMaxBody)
	{
		::OI::Base::Bytes stEmpty;
		stInputBuffer.Swap(stEmpty);
		Fail("size");
		return;
	}
	if (stInputBuffer.Size() > 0)
	{
		m_sIncoming.append(stInputBuffer.Data(), stInputBuffer.Size());
	}
	{
		::OI::Base::Bytes stEmpty;
		stInputBuffer.Swap(stEmpty);
	}
	Consume();
}

void HttpSession::OnDataOut()
{
}

void HttpSession::OnClose()
{
	FinishEof();
	::OI::Net::NetSession::OnClose();
}

Client::Impl::Impl(const std::string& sHostParam, int32_t iPortParam, int32_t iTimeoutMillisecondsParam, bool bReuseParam)
    : m_sHost(sHostParam), m_iPort(iPortParam), m_iTimeoutMilliseconds(iTimeoutMillisecondsParam), m_pSession(NULL), m_iSessionId(INVALID_SID), m_bConnected(false), m_bConnecting(false), m_bDnsPending(false), m_bClosed(false), m_bReuse(bReuseParam)
{
	std::memset(&m_stDns, 0, sizeof(m_stDns));
	iMaxReceiveBufferBytes = kMaxHeader + kMaxBody;
}

const std::string Client::Impl::GetIDString()
{
	return "HttpClient";
}

void Client::Impl::Publish(const std::shared_ptr<Call::Request>& pRequestParam, bool bFailParam, const char* pWhyParam)
{
	if (!pRequestParam)
	{
		return;
	}
	if (pRequestParam->iDone.exchange(1, std::memory_order_acq_rel) != 0)
	{
		return;
	}
	pRequestParam->bFailed = bFailParam;
	if (bFailParam && pRequestParam->sError.empty() && pWhyParam)
	{
		pRequestParam->sError = pWhyParam;
	}
	std::string().swap(pRequestParam->sRequest);
	// Idle→Ready：结果先写好，调用方还没挂起，留给 await_suspend 发现后不挂起。
	// Waiting→Ready：调用方已经挂起，这里 Schedule。
	coro::Gate iPrevious = pRequestParam->iGate.exchange(coro::Gate::Ready, std::memory_order_acq_rel);
	if (iPrevious == coro::Gate::Waiting && pRequestParam->stHandle)
	{
		coro::Schedule(pRequestParam->stHandle);
	}
}

void Client::Impl::UseAddress(const std::string& sIpParam)
{
	std::string sPortLocal = std::to_string(m_iPort);
	DefaultInit(sIpParam, sPortLocal);
	sEndpoint = sIpParam + ":" + sPortLocal;
}

void Client::Impl::FailFront(const char* pWhyParam)
{
	if (m_vQueue.empty())
	{
		return;
	}
	std::shared_ptr<Call::Request> pRequestLocal = m_vQueue.front();
	m_vQueue.pop_front();
	Publish(pRequestLocal, true, pWhyParam);
}

void Client::Impl::OnHttpFinished(HttpSession* pSessionParam, bool bFailParam, const char* pWhyParam, bool bCloseParam, int32_t iStatusParam, std::string sBodyParam)
{
	if (m_pFlight && pSessionParam == m_pSession)
	{
		std::shared_ptr<Call::Request> pFlightLocal = std::move(m_pFlight);
		pFlightLocal->iStatus = iStatusParam;
		if (!bFailParam)
		{
			pFlightLocal->sBody = std::move(sBodyParam);
		}
		Publish(pFlightLocal, bFailParam, pWhyParam);
	}
	bool bDropLocal = bFailParam || bCloseParam || !m_bReuse;
	if (bDropLocal && pSessionParam == m_pSession && pSessionParam && !pSessionParam->IsClosed())
	{
		pSessionParam->Abandon();
		CloseSession(pSessionParam->GetSessionID());
		return;
	}
	if (!m_bClosed)
	{
		Pump();
	}
}

void Client::Impl::OnIdleBytes(HttpSession* pSessionParam)
{
	if (pSessionParam == m_pSession && pSessionParam && !pSessionParam->IsClosed())
	{
		CloseSession(pSessionParam->GetSessionID());
	}
}

void Client::Impl::Pump()
{
	if (m_bClosed || m_pFlight || m_vQueue.empty())
	{
		return;
	}
	if (m_bConnected && m_pSession && !m_pSession->IsClosed())
	{
		std::shared_ptr<Call::Request> pRequestLocal = m_vQueue.front();
		m_vQueue.pop_front();
		m_pFlight = pRequestLocal;
		m_pSession->BeginResponse();
		::OI::Base::Bytes stBytes(pRequestLocal->sRequest.data(), pRequestLocal->sRequest.size());
		if (!SendBytes(m_iSessionId, stBytes))
		{
			m_pFlight.reset();
			m_pSession->Abandon();
			Publish(pRequestLocal, true, "send");
			if (m_pSession && !m_pSession->IsClosed())
			{
				CloseSession(m_iSessionId);
			}
			return;
		}
		return;
	}
	if (m_bConnecting || m_bDnsPending)
	{
		return;
	}
	m_bConnecting = true;
	BeginConnect();
}

void Client::Impl::BeginConnect()
{
	in_addr stAddr;
	if (inet_pton(AF_INET, m_sHost.c_str(), &stAddr) == 1)
	{
		UseAddress(m_sHost);
		StartClient();
		return;
	}
	m_bDnsPending = true;
	m_pDnsHold = Hold();
	std::memset(&m_stDns, 0, sizeof(m_stDns));
	m_stDns.data = this;
	addrinfo stHints;
	std::memset(&stHints, 0, sizeof(stHints));
	stHints.ai_family = AF_INET;
	stHints.ai_socktype = SOCK_STREAM;
	if (uv_getaddrinfo(::OI::Net::Poll::GetInstance().Loop(), &m_stDns, Impl::OnDns, m_sHost.c_str(), NULL, &stHints) != 0)
	{
		m_bDnsPending = false;
		m_pDnsHold.reset();
		m_bConnecting = false;
		FailFront("dns");
		Pump();
	}
}

void Client::Impl::OnDns(uv_getaddrinfo_t* pRequestParam, int iStatusParam, struct addrinfo* pResponseParam)
{
	Impl* pSelf = static_cast<Impl*>(pRequestParam->data);
	std::shared_ptr<Impl> pHold = std::move(pSelf->m_pDnsHold);
	pSelf->m_bDnsPending = false;
	if (pSelf->m_bClosed || iStatusParam != 0 || pResponseParam == NULL || pResponseParam->ai_addr == NULL || pResponseParam->ai_family != AF_INET)
	{
		if (pResponseParam)
		{
			uv_freeaddrinfo(pResponseParam);
		}
		if (!pSelf->m_bClosed)
		{
			pSelf->m_bConnecting = false;
			pSelf->FailFront("dns");
			pSelf->Pump();
		}
		return;
	}
	char sIp[INET_ADDRSTRLEN];
	std::memset(sIp, 0, sizeof(sIp));
	sockaddr_in stAddr = *reinterpret_cast<const sockaddr_in*>(pResponseParam->ai_addr);
	uv_freeaddrinfo(pResponseParam);
	if (inet_ntop(AF_INET, &stAddr.sin_addr, sIp, sizeof(sIp)) == NULL)
	{
		pSelf->m_bConnecting = false;
		pSelf->FailFront("dns");
		pSelf->Pump();
		return;
	}
	pSelf->UseAddress(sIp);
	pSelf->StartClient();
}

void Client::Impl::ArmTimer(const std::shared_ptr<Call::Request>& pRequestParam)
{
	std::shared_ptr<Call::Request> pHeld = pRequestParam;
	int32_t iTimeout = pHeld->iTimeoutMilliseconds > 0 ? pHeld->iTimeoutMilliseconds : 5000;
	::OI::Timer::BaseTimerManager::GetInstance().AddTimer([pHeld]()
	{
		auto pJob = std::make_unique<std::shared_ptr<Call::Request>>(pHeld);
		::OI::Net::Poll::GetInstance().CallOnLoop([](void* pArgumentParam)
		{
			std::unique_ptr<std::shared_ptr<Call::Request>> pOwned(static_cast<std::shared_ptr<Call::Request>*>(pArgumentParam));
			if (*pOwned && (*pOwned)->pImpl)
			{
				(*pOwned)->pImpl->OnTimeout(*pOwned);
			}
		},
		    pJob.release());
	},
	    iTimeout);
}

void Client::Impl::OnTimeout(const std::shared_ptr<Call::Request>& pRequestParam)
{
	if (!pRequestParam || pRequestParam->iDone.load(std::memory_order_acquire) != 0)
	{
		return;
	}
	if (m_pFlight == pRequestParam)
	{
		if (m_pSession)
		{
			m_pSession->Abandon();
		}
		m_pFlight.reset();
		Publish(pRequestParam, true, "timeout");
		if (m_pSession && !m_pSession->IsClosed())
		{
			CloseSession(m_iSessionId);
		}
		else if (!m_bClosed)
		{
			Pump();
		}
		return;
	}
	for (auto it = m_vQueue.begin(); it != m_vQueue.end(); ++it)
	{
		if (*it == pRequestParam)
		{
			m_vQueue.erase(it);
			Publish(pRequestParam, true, "timeout");
			break;
		}
	}
}

void Client::Impl::EnqueueOnLoop(const std::shared_ptr<Call::Request>& pRequestParam)
{
	if (m_bClosed)
	{
		Publish(pRequestParam, true, "closed");
		return;
	}
	if (m_iPort <= 0 || m_iPort > 65535 || m_sHost.empty())
	{
		Publish(pRequestParam, true, "host");
		return;
	}
	m_vQueue.push_back(pRequestParam);
	ArmTimer(pRequestParam);
	Pump();
}

void Client::Impl::Enqueue(const std::shared_ptr<Call::Request>& pRequestParam)
{
	if (!::OI::Net::Poll::GetInstance().InLoopThread())
	{
		auto pJob = std::make_unique<LoopRequest>();
		pJob->pImpl = Hold();
		pJob->pRequest = pRequestParam;
		::OI::Net::Poll::GetInstance().PostOnLoop([](void* pArgumentParam)
		{
			std::unique_ptr<LoopRequest> pOwned(static_cast<LoopRequest*>(pArgumentParam));
			pOwned->pImpl->EnqueueOnLoop(pOwned->pRequest);
		},
		    pJob.release());
		return;
	}
	EnqueueOnLoop(pRequestParam);
}

void Client::Impl::Shutdown()
{
	if (!::OI::Net::Poll::GetInstance().InLoopThread())
	{
		auto pJob = std::make_unique<std::shared_ptr<Impl>>(Hold());
		::OI::Net::Poll::GetInstance().PostOnLoop([](void* pArgumentParam)
		{
			std::unique_ptr<std::shared_ptr<Impl>> pOwned(static_cast<std::shared_ptr<Impl>*>(pArgumentParam));
			(*pOwned)->Shutdown();
		},
		    pJob.release());
		return;
	}
	if (m_bClosed)
	{
		return;
	}
	m_bClosed = true;
	if (m_pSession)
	{
		m_pSession->Abandon();
	}
	std::shared_ptr<Call::Request> pFlightLocal = std::move(m_pFlight);
	std::deque<std::shared_ptr<Call::Request>> vQueueLocal = std::move(m_vQueue);
	if (pFlightLocal)
	{
		Publish(pFlightLocal, true, "closed");
	}
	for (const auto& pRequestLocal : vQueueLocal)
	{
		Publish(pRequestLocal, true, "closed");
	}
	CloseAllSessions();
}

::OI::Net::Session* Client::Impl::CloneSession()
{
	return new HttpSession(Hold());
}

void Client::Impl::OnAddSession(::OI::Net::Session* pSessionParam)
{
	m_pSession = static_cast<HttpSession*>(pSessionParam);
	m_iSessionId = pSessionParam->GetSessionID();
	m_bConnected = true;
	m_bConnecting = false;
	if (m_bClosed)
	{
		CloseSession(m_iSessionId);
		return;
	}
	Pump();
}

void Client::Impl::OnDelSession(const ::OI::Net::Session* pSessionParam)
{
	if (m_pSession == pSessionParam)
	{
		m_pSession = NULL;
		m_bConnected = false;
		m_iSessionId = INVALID_SID;
	}
}

void Client::Impl::OnConnectFailed()
{
	m_bConnecting = false;
	m_bConnected = false;
	m_pSession = NULL;
	m_iSessionId = INVALID_SID;
	if (m_bClosed)
	{
		return;
	}
	FailFront("connect");
	Pump();
}

void Client::Impl::OnSessionClosed(sid_t iSessionIdParam)
{
	if (m_pSession && m_pSession->GetSessionID() == iSessionIdParam)
	{
		HttpSession* pSessionLocal = m_pSession;
		pSessionLocal->FinishEof();
		if (m_pSession == pSessionLocal)
		{
			m_pSession = NULL;
			m_bConnected = false;
			m_iSessionId = INVALID_SID;
		}
	}
	if (!m_bClosed)
	{
		Pump();
	}
}

Call::Call(std::shared_ptr<Request> pRequestParam)
    : m_pRequest(std::move(pRequestParam))
{
}

Call::Call(Call&& stOtherParam) noexcept
    : m_pRequest(std::move(stOtherParam.m_pRequest))
{
}

Call::~Call() = default;

bool Call::await_suspend(std::coroutine_handle<> stHandleParam)
{
	if (!m_pRequest || m_pRequest->bAwaitStarted.exchange(true, std::memory_order_acq_rel))
	{
		throw std::logic_error("HTTP Call can only be awaited once and must not be moved from");
	}
	m_pRequest->stHandle = stHandleParam;
	std::shared_ptr<Client::Impl> pImplLocal = m_pRequest->pImpl;
	if (!pImplLocal)
	{
		throw std::logic_error("HTTP Call can only be awaited once and must not be moved from");
	}
	pImplLocal->Enqueue(m_pRequest);
	// Idle→Waiting：还在等，挂起。Ready→Waiting：入队时已经写完结果，不挂起。
	return m_pRequest->iGate.exchange(coro::Gate::Waiting, std::memory_order_acq_rel) != coro::Gate::Ready;
}

Response Call::await_resume()
{
	if (!m_pRequest)
	{
		throw std::logic_error("HTTP Call can only be awaited once and must not be moved from");
	}
	Response stOutput;
	stOutput.iStatus = m_pRequest->iStatus;
	stOutput.sBody = std::move(m_pRequest->sBody);
	stOutput.bFail = m_pRequest->bFailed || m_pRequest->iStatus == 0;
	stOutput.sError = std::move(m_pRequest->sError);
	return stOutput;
}

Client::Client(const std::string& sHostParam, int32_t iPortParam, int32_t iTimeoutMillisecondsParam, bool bReuseParam)
    : m_pImpl(std::make_shared<Impl>(sHostParam, iPortParam, iTimeoutMillisecondsParam, bReuseParam))
{
	m_pImpl->Attach(m_pImpl);
}

Client::~Client()
{
	if (m_pImpl)
	{
		m_pImpl->Shutdown();
		m_pImpl.reset();
	}
}

Call Client::Get(const std::string& sPathParam, const std::vector<std::pair<std::string, std::string>>& vQueryParam)
{
	std::string sTarget = sPathParam.empty() ? "/" : sPathParam;
	if (!vQueryParam.empty())
	{
		sTarget.push_back('?');
		for (size_t iIndexLocal = 0; iIndexLocal < vQueryParam.size(); ++iIndexLocal)
		{
			if (iIndexLocal != 0)
			{
				sTarget.push_back('&');
			}
			sTarget += EncodeQuery(vQueryParam[iIndexLocal].first);
			sTarget.push_back('=');
			sTarget += EncodeQuery(vQueryParam[iIndexLocal].second);
		}
	}
	auto pRequest = std::make_shared<Call::Request>();
	pRequest->pImpl = m_pImpl;
	pRequest->iTimeoutMilliseconds = m_pImpl->Timeout();
	pRequest->sRequest = "GET " + sTarget + " HTTP/1.1\r\n";
	pRequest->sRequest += HostHeader(m_pImpl->Host(), m_pImpl->Port());
	pRequest->sRequest += m_pImpl->Reuse() ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
	return Call(pRequest);
}

Call Client::Post(const std::string& sPathParam, const std::string& sBodyParam, const std::string& sContentTypeParam)
{
	std::string sTarget = sPathParam.empty() ? "/" : sPathParam;
	auto pRequest = std::make_shared<Call::Request>();
	pRequest->pImpl = m_pImpl;
	pRequest->iTimeoutMilliseconds = m_pImpl->Timeout();
	pRequest->sRequest = "POST " + sTarget + " HTTP/1.1\r\n";
	pRequest->sRequest += HostHeader(m_pImpl->Host(), m_pImpl->Port());
	pRequest->sRequest += "Content-Type: " + sContentTypeParam + "\r\n";
	pRequest->sRequest += "Content-Length: " + std::to_string(sBodyParam.size()) + "\r\n";
	pRequest->sRequest += m_pImpl->Reuse() ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
	pRequest->sRequest.append(sBodyParam.data(), sBodyParam.size());
	return Call(pRequest);
}

}
}
