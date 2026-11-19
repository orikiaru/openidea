#include "oi_http.h"
#include "oi_config.h"
#include "oi_tcp_channel.h"
#include "oi_singleton.h"
#include <compare>
#include "oi_http_stream.h"
#include "oi_tls_session.h"
#include "oi_http_header_fields.h"
#include "oi_bytes.h"
#include "oi_rpc_coro.h"
#include "oi_poll.h"
#include "oi_session_manager.h"
#include "oi_socket.h"
#include "oi_timer_manager.h"
#include <atomic>
#include <deque>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>

namespace OI
{
namespace Http
{

QueryParams::QueryParams(std::initializer_list<Entry> vEntries)
    : m_vEntries(vEntries)
{
}

void QueryParams::Add(std::string sName, std::string sValue)
{
	m_vEntries.emplace_back(std::move(sName), std::move(sValue));
}

QueryParams::const_iterator QueryParams::find(std::string_view sName) const
{
	return std::find_if(begin(), end(), [sName](const auto& stEntry)
	{
		return stEntry.first == sName;
	});
}

const std::string* QueryParams::Get(std::string_view sName) const
{
	auto it = find(sName);
	return it == end() ? nullptr : &it->second;
}

std::vector<std::string> QueryParams::GetAll(std::string_view sName) const
{
	std::vector<std::string> vResult;
	for (const auto& [sKey, sValue] : m_vEntries)
	{
		if (sKey == sName)
		{
			vResult.push_back(sValue);
		}
	}
	return vResult;
}

const std::string& QueryParams::at(std::string_view sName) const
{
	if (const auto* pValue = Get(sName))
	{
		return *pValue;
	}
	throw std::out_of_range("HTTP query parameter");
}

size_t QueryParams::count(std::string_view sName) const
{
	return std::count_if(begin(), end(), [sName](const auto& stEntry)
	{
		return stEntry.first == sName;
	});
}

std::pair<QueryParams::const_iterator, bool> QueryParams::emplace(std::string sName, std::string sValue)
{
	auto it = find(sName);
	if (it != end())
	{
		return {it, false};
	}
	Add(std::move(sName), std::move(sValue));
	return {end() - 1, true};
}

std::string& QueryParams::operator[](std::string sName)
{
	auto it = emplace(std::move(sName), {}).first;
	return m_vEntries[it - begin()].second;
}

QueryParams::const_iterator QueryParams::begin() const
{
	return m_vEntries.begin();
}

QueryParams::const_iterator QueryParams::end() const
{
	return m_vEntries.end();
}

const std::string* Message::GetHeader(std::string_view sName) const
{
	for (const auto& [sKey, sValue] : vHeaders)
	{
		if (HeaderFields::NameEqual(sKey, sName))
		{
			return &sValue;
		}
	}
	return nullptr;
}

namespace
{
const size_t kMaxHeader = 65536;
const size_t kMaxBody = 8 * 1024 * 1024;

std::string EncodeQuery(const std::string& sValue)
{
	static const char* pHex = "0123456789ABCDEF";
	std::string sOutput;
	sOutput.reserve(sValue.size());
	for (size_t iIndexLocal = 0; iIndexLocal < sValue.size(); ++iIndexLocal)
	{
		unsigned char iCharacter = (unsigned char)sValue[iIndexLocal];
		// clang-format off
		bool bPlain = (iCharacter >= 'a' && iCharacter <= 'z')
			|| (iCharacter >= 'A' && iCharacter <= 'Z')
			|| (iCharacter >= '0' && iCharacter <= '9')
			|| iCharacter == '-'
			|| iCharacter == '_'
			|| iCharacter == '.'
			|| iCharacter == '!'
			|| iCharacter == '~'
			|| iCharacter == '*'
			|| iCharacter == '\''
			|| iCharacter == '('
			|| iCharacter == ')';
		// clang-format on
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

bool EqName(const std::string& sText, size_t iPosition, size_t iEnd, const char* pName)
{
	size_t iNumber = std::strlen(pName);
	if (iPosition + iNumber > iEnd)
	{
		return false;
	}
	for (size_t iIndexLocal = 0; iIndexLocal < iNumber; ++iIndexLocal)
	{
		char iCharacter = sText[iPosition + iIndexLocal];
		if (iCharacter >= 'A' && iCharacter <= 'Z')
		{
			iCharacter = (char)(iCharacter - 'A' + 'a');
		}
		if (iCharacter != pName[iIndexLocal])
		{
			return false;
		}
	}
	return true;
}

bool ParseU64(const std::string& sText, size_t iPosition, size_t iEnd, uint64_t& iOutput)
{
	if (iPosition >= iEnd || sText[iPosition] < '0' || sText[iPosition] > '9')
	{
		return false;
	}
	uint64_t iValue = 0;
	for (size_t iIndexLocal = iPosition; iIndexLocal < iEnd; ++iIndexLocal)
	{
		char iCharacter = sText[iIndexLocal];
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
	iOutput = iValue;
	return true;
}

// chunk 行里分号后面是扩展，忽略。
bool ParseHex(const std::string& sText, size_t iPosition, size_t iEnd, uint64_t& iOutput)
{
	uint64_t iValue = 0;
	size_t iDigits = 0;
	for (size_t iIndexLocal = iPosition; iIndexLocal < iEnd; ++iIndexLocal)
	{
		char iCharacter = sText[iIndexLocal];
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
	iOutput = iValue;
	return iDigits > 0;
}

std::string HostHeader(const std::string& sHost, int32_t iPort, bool bSecure)
{
	std::string sLine = "Host: ";
	::OI::Net::Ipv6Addr stAddress;
	if (!sHost.starts_with('[') && stAddress.Init(sHost, 0))
	{
		sLine += "[" + sHost + "]";
	}
	else
	{
		sLine += sHost;
	}
	if (iPort != (bSecure ? 443 : 80))
	{
		sLine += ":";
		sLine += std::to_string(iPort);
	}
	sLine += "\r\n";
	return sLine;
}
}

class HttpSession;
class HttpDial;

struct Origin
{
	std::string sHost;
	int32_t iPort = 0;
	bool bHttps = false;
	auto operator<=>(const Origin&) const = default;
};

struct Call::Request
{
	std::shared_ptr<BodySource> pSource;
	std::shared_ptr<BodySink> pSink;
	bool Cancelled() const
	{
		return (pSource && pSource->IsCancelled()) || (pSink && pSink->IsCancelled());
	}
	Origin stOrigin;
	std::string sCaFile;
	bool bReuse = true;
	sid_t iSessionId = INVALID_SID;
	HttpDial* pDial = nullptr; // poll thread only

	std::string sRequest; // 待发报文。发布结果前释放，晚到的超时不再留着大包。
	Headers vHeaders;
	std::string sBody;                 // 正文。exchange(Ready) 之前写完。
	std::string sError;                // 失败原因。成功时为空。
	std::coroutine_handle<> stHandle;  // 调用方句柄。门已经是 Waiting 才 Schedule。
	std::atomic<OI::Coro::Gate> iGate; // 完成门。Idle 还没人认领，Ready 结果已写好，Waiting 调用方已挂起。
	std::atomic<int32_t> iDone;        // 结果只发布一次。晚到的超时看见非 0 就返回。
	std::atomic<bool> bAwaitStarted;   // 一次性闩。第二次 await 或移走后再等直接抛。
	int32_t iStatus;                   // HTTP 状态码。没拿到状态行是 0。
	int32_t iTimeoutMilliseconds;      // 从入队起算的超时。
	int32_t iCode;                     // 连接、超时或报文失败。
	~Request();

	Request()
	    : iGate(OI::Coro::Gate::Idle), iDone(0), bAwaitStarted(false), iStatus(0), iTimeoutMilliseconds(5000), iCode(OI_OK)
	{
	}
};

class HttpSessionManager final : public Net::SessionManager
{
	DECLARE_SINGLETON(HttpSessionManager)
	using Pending = std::shared_ptr<Call::Request>;
	struct Pool
	{
		std::deque<Pending> vWaiting;
		std::set<Pending> vActive;
		std::deque<HttpSession*> vIdle;
		std::unique_ptr<Net::TlsClientContext> pTls;
		std::string sCaFile;
		bool bPumping = false;
	};
	std::map<Origin, Pool> m_mPools;
	std::map<sid_t, HttpSession*> m_mSessions;
	size_t m_iLimit = 4;
	uint64_t m_iIdleMilliseconds = 60000;
	bool m_bStopped = false;
	HttpSessionManager();
	void EnqueueOnLoop(const Pending& pCall);
	void Pump(const Origin& stOrigin, Pool& stPool);
	void Send(HttpSession& stSession, const Pending& pCall);
	static void Resolved(void* pArgument, bool bOk, Net::NetAddr stAddress);
	void Finish(const Pending& pCall, int32_t iCode, const char* pReason);
	static void Publish(const Pending& pCall, int32_t iCode, const char* pReason);
	static void Stop();
	void Sweep();
	Net::Session* CloneSession() override
	{
		return nullptr;
	}
	void OnAddSession(Net::Session* pSession) override;
	void OnDelSession(const Net::Session*) override
	{
	}
	void OnSessionClosed(sid_t iSessionId) override;

  public:
	void Enqueue(const Pending& pCall);
	void Connected(const Pending& pCall, net_socket_t iSocket);
	void Complete(HttpSession& stSession, int32_t iCode, const char* pReason, bool bClose, int32_t iStatus, std::string sBody, Headers vHeaders);
	Coro::Task OnSecondUpdate(time_t);
};

class HttpSession : public ::OI::Net::TlsSession
{
	Detail::StreamPump m_stStreamPump;
	Detail::StreamBody m_stStreamBody;
	std::shared_ptr<BodySource> m_pSource;
	std::shared_ptr<BodySink> m_pSink;
	bool m_bReadPaused = false;
	void PumpStream();
	void ClearStream(bool bCancel);
	friend class HttpSessionManager;
	std::shared_ptr<Call::Request> m_pFlight;
	Origin m_stOrigin;
	std::string m_sCaFile;
	uint64_t m_iIdleSince = 0;
	std::string m_sIncoming;
	Headers m_vHeaders;
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
	void NoteConnection(const std::string& sValue);
	bool ParseHeaders(size_t iHeaderEnd);
	bool ConsumeChunks();
	bool Consume();
	void Succeed(size_t iConsumed);
	void Fail(const char* pWhy, int32_t iCode = OI_ERR_PROTOCOL);

  public:
	HttpSession(HttpSessionManager& stManager, const Origin& stOrigin, const Net::TlsClientContext* pTls);
	~HttpSession() override;
	bool BeginResponse(const Call::Request& stCall);
	void Abandon();
	void FinishEof();
	void OnPlaintext(std::string_view sData) override;
	void OnTransportEnd(int32_t iCode, const char* pReason) override;
	void OnDataOut() override;
};

Call::Request::~Request() = default;
HttpSession::~HttpSession() = default;

HttpSession::HttpSession(HttpSessionManager& stManager, const Origin& stOrigin, const Net::TlsClientContext* pTls)
    : Net::TlsSession(&stManager, pTls, stOrigin.sHost), m_stOrigin(stOrigin), m_iHeaderBytes(0), m_iContentLength(0), m_iChunkPosition(0), m_iInterimResponses(0), m_iStatus(0), m_bHeadersReady(false), m_bHasLength(false), m_bChunked(false), m_bCloseAfter(false), m_bActive(false)
{
}

void HttpSession::ReleaseIncoming()
{
	std::string().swap(m_sIncoming);
}

bool HttpSession::BeginResponse(const Call::Request& stCall)
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
	m_pSource = stCall.pSource;
	m_pSink = stCall.pSink;
	return (!m_pSource && !m_pSink) || m_stStreamPump.Start(this, [](uv_timer_t* pTimer)
	{
		static_cast<HttpSession*>(pTimer->data)->PumpStream();
	});
}

void HttpSession::ClearStream(bool bCancel)
{
	m_stStreamPump.Stop();
	if (m_pSource)
	{
		m_pSource->Cancel();
		m_pSource.reset();
	}
	if (m_pSink && bCancel)
	{
		m_pSink->Cancel();
	}
	m_pSink.reset();
	if (m_bReadPaused)
	{
		m_bReadPaused = false;
		AllowRead();
	}
}

void HttpSession::PumpStream()
{
	if (m_pFlight && m_pFlight->Cancelled())
	{
		Fail("stream cancelled", OI_ERR_CANCELLED);
		return;
	}
	if (!TransportReady())
	{
		return;
	}
	if (m_pSource)
	{
		bool bDone = false, bProgress = false;
		const int32_t iCode = Detail::StreamBody::Produce(*this, *m_pSource, bDone, bProgress);
		if (iCode != OI_OK)
		{
			Fail("request body", iCode);
			return;
		}
		if (bDone)
		{
			m_pSource.reset();
		}
	}
	if (m_bReadPaused)
	{
		if (!Consume() && !m_bReadPaused && PeerEnded())
		{
			FinishEof();
		}
	}
}

void HttpSession::Abandon()
{
	ClearStream(true);
	m_bActive = false;
	ReleaseIncoming();
}

void HttpSession::NoteConnection(const std::string& sValue)
{
	bool bClose = false;
	bool bKeep = false;
	size_t iPosition = 0;
	while (iPosition < sValue.size())
	{
		while (iPosition < sValue.size() && (sValue[iPosition] == ' ' || sValue[iPosition] == '\t' || sValue[iPosition] == ','))
		{
			iPosition += 1;
		}
		if (iPosition >= sValue.size())
		{
			break;
		}
		size_t iEnd = iPosition;
		while (iEnd < sValue.size() && sValue[iEnd] != ',')
		{
			iEnd += 1;
		}
		size_t iTokenEnd = iEnd;
		while (iTokenEnd > iPosition && (sValue[iTokenEnd - 1] == ' ' || sValue[iTokenEnd - 1] == '\t'))
		{
			iTokenEnd -= 1;
		}
		if (EqName(sValue, iPosition, iTokenEnd, "close") && iPosition + std::strlen("close") == iTokenEnd)
		{
			bClose = true;
		}
		else if (EqName(sValue, iPosition, iTokenEnd, "keep-alive") && iPosition + std::strlen("keep-alive") == iTokenEnd)
		{
			bKeep = true;
		}
		if (iEnd >= sValue.size())
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

bool HttpSession::ParseHeaders(size_t iHeaderEnd)
{
	m_vHeaders.clear();
	size_t iLineEnd = m_sIncoming.find("\r\n");
	if (iLineEnd == std::string::npos || iLineEnd > iHeaderEnd)
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
	while (iPosition < iHeaderEnd)
	{
		size_t iEnd = m_sIncoming.find("\r\n", iPosition);
		if (iEnd == std::string::npos || iEnd > iHeaderEnd)
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
			m_vHeaders.emplace_back(m_sIncoming.substr(iPosition, iNameEnd - iPosition), m_sIncoming.substr(iValue, iValueEnd - iValue));
			if (EqName(m_sIncoming, iPosition, iNameEnd, "content-length") && iPosition + std::strlen("content-length") == iNameEnd)
			{
				uint64_t iLength = 0;
				if (!ParseU64(m_sIncoming, iValue, iValueEnd, iLength) || (!m_pSink && m_iStatus != 304 && m_iStatus >= 200 && iLength > kMaxBody) || (m_bHasLength && m_iContentLength != iLength))
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
		if (m_pSink && m_iStatus != 204 && m_iStatus != 304)
		{
			m_stStreamBody.Begin(m_bChunked, m_bHasLength, m_iContentLength);
			m_sIncoming.erase(0, m_iHeaderBytes);
			m_iHeaderBytes = 0;
		}
	}
	if (m_iStatus == 204 || m_iStatus == 304)
	{
		Succeed(m_iHeaderBytes);
		return true;
	}
	if (m_pSink)
	{
		size_t iConsumed = 0;
		int32_t iCode = m_stStreamBody.Consume(m_sIncoming, *m_pSink, iConsumed);
		m_sIncoming.erase(0, iConsumed);
		if (iCode == OI_ERR_BUSY)
		{
			if (!m_bReadPaused)
			{
				m_bReadPaused = true;
				ForbidRead();
			}
			return false;
		}
		if (iCode != OI_OK)
		{
			Fail("response body", iCode);
			return true;
		}
		if (m_bReadPaused)
		{
			m_bReadPaused = false;
			AllowRead();
		}
		if (m_stStreamBody.Done())
		{
			Succeed(0);
			return true;
		}
		return false;
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

void HttpSession::Succeed(size_t iConsumed)
{
	if (!m_bActive)
	{
		return;
	}
	// 同一段缓冲里还剩下一响没用到的字节。丢掉后续复用会让下一条请求从半截报文开始解析。
	if (iConsumed < m_sIncoming.size())
	{
		m_bCloseAfter = true;
	}
	m_bActive = false;
	int32_t iStatusLocal = m_iStatus;
	bool bCloseLocal = m_bCloseAfter || bool(m_pSource) || bReadEnded || PeerEnded();
	if (bCloseLocal || !m_pFlight->bReuse)
	{
		ShutdownTransport();
	}
	ClearStream(false);
	std::string sBodyLocal = std::move(m_sBody);
	ReleaseIncoming();
	HttpSessionManager::GetInstance().Complete(*this, OI_OK, NULL, bCloseLocal, iStatusLocal, std::move(sBodyLocal), std::move(m_vHeaders));
}

void HttpSession::Fail(const char* pWhy, int32_t iCode)
{
	if (!m_bActive)
	{
		return;
	}
	m_bActive = false;
	ClearStream(true);
	int32_t iStatusLocal = m_iStatus;
	ReleaseIncoming();
	HttpSessionManager::GetInstance().Complete(*this, iCode, pWhy, true, iStatusLocal, std::string(), {});
}

void HttpSession::FinishEof()
{
	if (!m_bActive)
	{
		return;
	}
	if (Consume() || m_bReadPaused)
	{
		return;
	}
	if (!m_bHeadersReady || m_bHasLength || m_bChunked)
	{
		Fail("eof");
		return;
	}
	if (!m_pSink)
	{
		m_sBody.assign(m_sIncoming, m_iHeaderBytes, std::string::npos);
	}
	m_bCloseAfter = true;
	Succeed(m_sIncoming.size());
}

void HttpSession::OnPlaintext(std::string_view sData)
{
	if (!m_bActive)
	{
		if (!IsClosed())
		{
			HttpSessionManager::GetInstance().CloseSession(GetSessionID());
		}
		return;
	}
	if (sData.size() > kMaxHeader + kMaxBody - m_sIncoming.size())
	{
		Fail("size");
		return;
	}
	m_sIncoming.append(sData);
	Consume();
}

void HttpSession::OnTransportEnd(int32_t iCode, const char* pReason)
{
	if (!m_bActive)
	{
		if (!IsClosed())
		{
			HttpSessionManager::GetInstance().CloseSession(GetSessionID());
		}
	}
	else if (iCode != OI_OK)
	{
		Fail(pReason, iCode);
	}
	else if (!m_bReadPaused)
	{
		FinishEof();
	}
}

void HttpSession::OnDataOut()
{
}

void HttpSessionManager::Publish(const std::shared_ptr<Call::Request>& pRequestLocal, int32_t iCode, const char* pWhy)
{
	if (!pRequestLocal)
	{
		return;
	}
	if (pRequestLocal->iDone.exchange(1, std::memory_order_acq_rel) != 0)
	{
		return;
	}
	pRequestLocal->iCode = iCode;
	if (iCode != OI_OK && pRequestLocal->pSource)
	{
		pRequestLocal->pSource->Cancel();
	}
	if (iCode != OI_OK && pRequestLocal->pSink)
	{
		pRequestLocal->pSink->Cancel();
	}
	pRequestLocal->pSource.reset();
	pRequestLocal->pSink.reset();
	if (iCode != OI_OK && pRequestLocal->sError.empty() && pWhy)
	{
		pRequestLocal->sError = pWhy;
	}
	std::string().swap(pRequestLocal->sRequest);
	// Idle→Ready：结果先写好，调用方还没挂起，留给 await_suspend 发现后不挂起。
	// Waiting→Ready：调用方已经挂起，这里 Schedule。
	OI::Coro::Gate iPrevious = pRequestLocal->iGate.exchange(OI::Coro::Gate::Ready, std::memory_order_acq_rel);
	if (iPrevious == OI::Coro::Gate::Waiting && pRequestLocal->stHandle)
	{
		OI::Coro::Schedule(pRequestLocal->stHandle);
	}
}

// One in-progress TCP connect. The manager owns its request until completion or cancellation.
class HttpDial final : public Net::Channel
{
	std::shared_ptr<Call::Request> m_pCall;
	void Ready(bool bReadEvent)
	{
		if (!m_pCall)
		{
			return;
		}
		const int iError = NetOs_GetSocketError(stSocket.GetSocket());
		if (!bReadEvent && (iError == EINPROGRESS || iError == EINTR))
		{
			return;
		}
		auto pCall = std::move(m_pCall);
		pCall->pDial = nullptr;
		HttpSessionManager::GetInstance().Connected(pCall, iError ? INVALID_SOCKET_ID : NetOs_Dup(stSocket.GetSocket()));
	}

  public:
	HttpDial(Net::NetSocket stSocketLocal, const std::shared_ptr<Call::Request>& pCall)
	    : Channel(stSocketLocal, nullptr), m_pCall(pCall)
	{
	}
	void DataIn() override
	{
		Ready(true);
	}
	void DataOut() override
	{
		Ready(false);
	}
	bool TryClose() override
	{
		if (m_pCall)
		{
			return false;
		}
		delete this;
		return true;
	}
};

HttpSessionManager::HttpSessionManager()
{
	int64_t iLimit = 4, iIdle = 60000;
	TryGetConfig("HttpClientPool.max_connections_per_server", iLimit);
	TryGetConfig("HttpClientPool.idle_timeout_ms", iIdle);
	m_iLimit = iLimit > 0 ? static_cast<size_t>(iLimit) : 4;
	m_iIdleMilliseconds = iIdle > 0 ? static_cast<uint64_t>(iIdle) : 60000;
	iMaxReceiveBufferBytes = kMaxHeader + kMaxBody;
}

void HttpSessionManager::Enqueue(const Pending& pCall)
{
	if (Net::Poll::GetInstance().InLoopThread())
	{
		EnqueueOnLoop(pCall);
		return;
	}
	Net::Poll::GetInstance().PostOnLoop([](void* pArg)
	{
		std::unique_ptr<Pending> pCallLocal(static_cast<Pending*>(pArg));
		GetInstance().EnqueueOnLoop(*pCallLocal);
	}, new Pending(pCall));
}

void HttpSessionManager::EnqueueOnLoop(const Pending& pCall)
{
	if (m_bStopped || Thread::ThreadPool::GetInstance().AddStopHandler(&Stop) != OI_OK)
	{
		Publish(pCall, OI_ERR_CONNECT, "HTTP pool stopped");
		return;
	}
	if (pCall->Cancelled())
	{
		Publish(pCall, OI_ERR_CANCELLED, "cancelled");
		return;
	}
	// Only a live call needs a timeout. Completed requests and their bodies can be released immediately.
	std::weak_ptr<Call::Request> pWeak = pCall;
	const int32_t iTimerCode = Timer::BaseTimerManager::GetInstance().AddTimer([pWeak]()
	{
		if (auto pHeld = pWeak.lock(); pHeld && !pHeld->iDone.load(std::memory_order_acquire))
		{
			Net::Poll::GetInstance().CallOnLoop([](void* pArg)
			{
				std::unique_ptr<Pending> pCallLocal(static_cast<Pending*>(pArg));
				GetInstance().Finish(*pCallLocal, OI_ERR_TIMEOUT, "timeout");
			}, new Pending(std::move(pHeld)));
		}
	}, pCall->iTimeoutMilliseconds > 0 ? pCall->iTimeoutMilliseconds : 5000);
	if (iTimerCode != OI_OK)
	{
		Publish(pCall, iTimerCode, "HTTP request timer");
		return;
	}
	auto& stPool = m_mPools[pCall->stOrigin];
	stPool.vWaiting.push_back(pCall);
	Pump(pCall->stOrigin, stPool);
}

void HttpSessionManager::Pump(const Origin& stOrigin, Pool& stPool)
{
	if (m_bStopped || stPool.bPumping)
	{
		return;
	}
	stPool.bPumping = true;
	while (!stPool.vWaiting.empty() && stPool.vActive.size() < m_iLimit)
	{
		auto pCall = std::move(stPool.vWaiting.front());
		stPool.vWaiting.pop_front();
		if (pCall->Cancelled())
		{
			Publish(pCall, OI_ERR_CANCELLED, "cancelled");
			continue;
		}
		if (pCall->bReuse)
		{
			auto it = std::find_if(stPool.vIdle.begin(), stPool.vIdle.end(), [pCall](const HttpSession* pSession)
			{
				return pSession->m_sCaFile == pCall->sCaFile;
			});
			if (it != stPool.vIdle.end())
			{
				auto* pSession = *it;
				stPool.vIdle.erase(it);
				stPool.vActive.insert(pCall);
				Send(*pSession, pCall);
				continue;
			}
		}
		if (stPool.vIdle.size() + stPool.vActive.size() >= m_iLimit)
		{
			CloseSession(stPool.vIdle.front()->GetSessionID());
		}
		stPool.vActive.insert(pCall);
		Net::NetAddr::Resolve(stOrigin.sHost, std::to_string(stOrigin.iPort), &Resolved, new Pending(pCall));
	}
	stPool.bPumping = false;
}

void HttpSessionManager::Resolved(void* pArgument, bool bOk, Net::NetAddr stAddress)
{
	std::unique_ptr<Pending> pHeld(static_cast<Pending*>(pArgument));
	const auto& pCall = *pHeld;
	if (pCall->iDone.load())
	{
		return;
	}
	if (!bOk)
	{
		GetInstance().Finish(pCall, OI_ERR_CONNECT, "DNS resolution failed");
		return;
	}
	Net::NetSocket stSocket;
	stSocket.Attach(stAddress.CreateTcp(true));
	if (stSocket.GetSocket() == INVALID_SOCKET_ID)
	{
		GetInstance().Finish(pCall, OI_ERR_CONNECT, "TCP socket creation failed");
		return;
	}
	const int iResult = NetOs_Connect(stSocket.GetSocket(), stAddress.NetAttrPtr(), stAddress.NetAttrLen());
	if (iResult == 0)
	{
		GetInstance().Connected(pCall, stSocket.GetSocket());
		return;
	}
	if (errno != EINPROGRESS)
	{
		NetOs_Close(stSocket.GetSocket());
		GetInstance().Finish(pCall, OI_ERR_CONNECT, "TCP connect failed");
		return;
	}
	pCall->pDial = new HttpDial(stSocket, pCall);
	Net::Poll::GetInstance().IOEventRegister(pCall->pDial, false, true);
	if (!pCall->pDial->pPollToken)
	{
		GetInstance().Finish(pCall, OI_ERR_CONNECT, "TCP connect registration failed");
	}
}

void HttpSessionManager::Connected(const Pending& pCall, net_socket_t iSocket)
{
	if (iSocket == INVALID_SOCKET_ID)
	{
		Finish(pCall, OI_ERR_CONNECT, "TCP connect failed");
		return;
	}
	auto& stPool = m_mPools.at(pCall->stOrigin);
	if (pCall->stOrigin.bHttps && (!stPool.pTls || stPool.sCaFile != pCall->sCaFile))
	{
		stPool.pTls = std::make_unique<Net::TlsClientContext>(pCall->sCaFile);
		stPool.sCaFile = pCall->sCaFile;
	}
	auto* pSession = new HttpSession(*this, pCall->stOrigin, stPool.pTls.get());
	pSession->m_sCaFile = pCall->sCaFile;
	Net::NetSocket stSocket;
	stSocket.Attach(iSocket);
	auto* pChannel = new Net::TCP_Channel(stSocket, pSession);
	pSession->SetChannel(pChannel);
	AddSession(pSession, Net::NetAddr::Endpoint(pCall->stOrigin.sHost, std::to_string(pCall->stOrigin.iPort)), iMaxSendQueueBytes, iMaxReceiveBufferBytes);
	Net::Poll::GetInstance().IOEventRegister(pChannel, true, false);
	if (!pChannel->pPollToken)
	{
		CloseSession(pSession->GetSessionID());
		pChannel->TryClose();
		Finish(pCall, OI_ERR_CONNECT, "TCP session registration failed");
		return;
	}
	Send(*pSession, pCall);
}

void HttpSessionManager::OnAddSession(Net::Session* pSession)
{
	m_mSessions.emplace(pSession->GetSessionID(), static_cast<HttpSession*>(pSession));
}

void HttpSessionManager::Send(HttpSession& stSession, const Pending& pCall)
{
	stSession.m_pFlight = pCall;
	pCall->iSessionId = stSession.GetSessionID();
	if (pCall->Cancelled())
	{
		Finish(pCall, OI_ERR_CANCELLED, "cancelled");
		return;
	}
	const bool bReady = stSession.BeginResponse(*pCall);
	const int32_t iCode = bReady ? SendBytes(pCall->iSessionId, Base::Bytes(pCall->sRequest.data(), pCall->sRequest.size())) : OI_ERR_FAIL;
	if (iCode != OI_OK)
	{
		Finish(pCall, iCode, stSession.TransportError());
	}
}

void HttpSessionManager::Finish(const Pending& pCall, int32_t iCode, const char* pReason)
{
	if (pCall->iDone.load())
	{
		return;
	}
	auto& stPool = m_mPools.at(pCall->stOrigin);
	if (auto* pDial = std::exchange(pCall->pDial, nullptr))
	{
		delete pDial;
	}
	if (pCall->iSessionId != INVALID_SID)
	{
		auto it = m_mSessions.find(pCall->iSessionId);
		if (it != m_mSessions.end())
		{
			it->second->Abandon();
			it->second->m_pFlight.reset();
			CloseSession(pCall->iSessionId);
		}
	}
	if (!stPool.vActive.erase(pCall))
	{
		std::erase(stPool.vWaiting, pCall);
	}
	Publish(pCall, iCode, pReason);
	Pump(pCall->stOrigin, stPool);
}

void HttpSessionManager::Complete(HttpSession& stSession, int32_t iCode, const char* pReason, bool bClose, int32_t iStatus, std::string sBody, Headers vHeaders)
{
	auto pCall = std::move(stSession.m_pFlight);
	if (!pCall)
	{
		return;
	}
	auto& stPool = m_mPools.at(pCall->stOrigin);
	pCall->iStatus = iStatus;
	pCall->sBody = std::move(sBody);
	pCall->vHeaders = std::move(vHeaders);
	stPool.vActive.erase(pCall);
	if (iCode != OI_OK || bClose || !pCall->bReuse || stSession.IsClosed())
	{
		if (!stSession.IsClosed())
		{
			CloseSession(stSession.GetSessionID());
		}
	}
	else
	{
		stSession.m_iIdleSince = uv_now(Net::Poll::GetInstance().Loop());
		stPool.vIdle.push_back(&stSession);
	}
	Publish(pCall, iCode, pReason);
	Pump(pCall->stOrigin, stPool);
}

void HttpSessionManager::OnSessionClosed(sid_t iSessionId)
{
	auto it = m_mSessions.find(iSessionId);
	if (it == m_mSessions.end())
	{
		return;
	}
	auto* pSession = it->second;
	m_mSessions.erase(it);
	auto& stPool = m_mPools.at(pSession->m_stOrigin);
	std::erase(stPool.vIdle, pSession);
	pSession->EndTransport();
	if (auto pCall = pSession->m_pFlight)
	{
		pSession->Abandon();
		pSession->m_pFlight.reset();
		Finish(pCall, OI_ERR_CONNECT, "connection closed");
	}
}

void HttpSessionManager::Stop()
{
	auto& stManager = GetInstance();
	stManager.m_bStopped = true;
	for (auto& [stOrigin, stPool] : stManager.m_mPools)
	{
		while (!stPool.vWaiting.empty())
		{
			auto pCall = stPool.vWaiting.front();
			stManager.Finish(pCall, OI_ERR_CONNECT, "HTTP pool stopped");
		}
		while (!stPool.vActive.empty())
		{
			auto pCall = *stPool.vActive.begin();
			stManager.Finish(pCall, OI_ERR_CONNECT, "HTTP pool stopped");
		}
	}
	stManager.CloseAllSessions();
	stManager.m_mPools.clear();
}

Coro::Task HttpSessionManager::OnSecondUpdate(time_t)
{
	Net::Poll::GetInstance().CallOnLoop([](void*)
	{
		GetInstance().Sweep();
	}, nullptr);
	co_return;
}

void HttpSessionManager::Sweep()
{
	const auto iNow = uv_now(Net::Poll::GetInstance().Loop());
	for (auto it = m_mPools.begin(); it != m_mPools.end();)
	{
		auto& stPool = it->second;
		while (!stPool.vIdle.empty() && iNow - stPool.vIdle.front()->m_iIdleSince >= m_iIdleMilliseconds)
		{
			CloseSession(stPool.vIdle.front()->GetSessionID());
		}
		std::vector<Pending> vCancelled;
		for (const auto& pCall : stPool.vWaiting)
		{
			if (pCall->Cancelled())
			{
				vCancelled.push_back(pCall);
			}
		}
		for (const auto& pCall : stPool.vActive)
		{
			if (pCall->Cancelled())
			{
				vCancelled.push_back(pCall);
			}
		}
		for (const auto& pCall : vCancelled)
		{
			Finish(pCall, OI_ERR_CANCELLED, "cancelled");
		}
		if (stPool.vIdle.empty() && stPool.vActive.empty() && stPool.vWaiting.empty())
		{
			it = m_mPools.erase(it);
		}
		else
		{
			++it;
		}
	}
}
Call::Call(std::shared_ptr<Request> pRequestLocal)
    : m_pRequest(std::move(pRequestLocal))
{
}

Call::Call(Call&& stOther) noexcept
    : m_pRequest(std::move(stOther.m_pRequest))
{
}

Call::~Call() = default;

bool Call::await_suspend(std::coroutine_handle<> stHandleLocal)
{
	if (!m_pRequest || m_pRequest->bAwaitStarted.exchange(true, std::memory_order_acq_rel))
	{
		throw std::logic_error("HTTP Call can only be awaited once and must not be moved from");
	}
	m_pRequest->stHandle = stHandleLocal;
	if (m_pRequest->iCode == OI_ERR_INVALID_ARGUMENT)
	{
		return false;
	}
	HttpSessionManager::GetInstance().Enqueue(m_pRequest);
	// Idle→Waiting：还在等，挂起。Ready→Waiting：入队时已经写完结果，不挂起。
	return m_pRequest->iGate.exchange(OI::Coro::Gate::Waiting, std::memory_order_acq_rel) != OI::Coro::Gate::Ready;
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
	stOutput.iCode = m_pRequest->iCode;
	if (stOutput.iCode == OI_OK && m_pRequest->iStatus == 0)
	{
		stOutput.iCode = OI_ERR_PROTOCOL;
	}
	stOutput.vHeaders = std::move(m_pRequest->vHeaders);
	if (const auto* pType = stOutput.GetHeader("content-type"))
	{
		stOutput.sContentType = *pType;
	}
	stOutput.sError = std::move(m_pRequest->sError);
	return stOutput;
}

Client::Client(const std::string& sHost, int32_t iPort, int32_t iTimeoutMilliseconds, bool bReuse)
    : m_sHost(sHost), m_iPort(iPort), m_stOptions{iTimeoutMilliseconds, {}}, m_bHttps(false), m_bReuse(bReuse)
{
}

Client::Client(const std::string& sHost, int32_t iPort, const TlsOptions& stTls, int32_t iTimeoutMilliseconds, bool bReuse)
    : m_sHost(sHost), m_iPort(iPort), m_stOptions{iTimeoutMilliseconds, stTls.sCaFile}, m_bHttps(true), m_bReuse(bReuse)
{
}

static std::string QueryTarget(std::string_view sPath, std::span<const std::pair<std::string, std::string>> vQuery)
{
	std::string sTarget(sPath.empty() ? "/" : sPath);
	if (!vQuery.empty())
	{
		const size_t iQueryStart = sTarget.find('?');
		if (iQueryStart == std::string::npos)
		{
			sTarget.push_back('?');
		}
		else if (iQueryStart + 1 < sTarget.size() && sTarget.back() != '&')
		{
			sTarget.push_back('&');
		}
		for (size_t iIndexLocal = 0; iIndexLocal < vQuery.size(); ++iIndexLocal)
		{
			if (iIndexLocal != 0)
			{
				sTarget.push_back('&');
			}
			sTarget += EncodeQuery(vQuery[iIndexLocal].first);
			sTarget.push_back('=');
			sTarget += EncodeQuery(vQuery[iIndexLocal].second);
		}
	}
	return sTarget;
}

Call Client::GetUrl(std::string_view sUrl, const ClientOptions& stOptions)
{
	return SendUrl(sUrl, Request{}, stOptions);
}

Call Client::PostUrl(std::string_view sUrl, const std::string& sBody, const std::string& sContentType, const ClientOptions& stOptions)
{
	Request stRequest;
	stRequest.sMethod = "POST";
	stRequest.sBody = sBody;
	stRequest.sContentType = sContentType;
	return SendUrl(sUrl, stRequest, stOptions);
}

Call Client::SendUrl(std::string_view sUrl, const Request& stRequest, const ClientOptions& stOptions)
{
	auto fnInvalid = []()
	{
		auto pCall = std::make_shared<Call::Request>();
		pCall->iCode = OI_ERR_INVALID_ARGUMENT;
		pCall->sError = "invalid absolute HTTP URL";
		return Call(std::move(pCall));
	};
	const size_t iScheme = sUrl.find("://");
	if (iScheme == sUrl.npos)
	{
		return fnInvalid();
	}
	const auto sScheme = sUrl.substr(0, iScheme);
	const bool bHttps = HeaderFields::NameEqual(sScheme, "https");
	if (!bHttps && !HeaderFields::NameEqual(sScheme, "http"))
	{
		return fnInvalid();
	}
	sUrl.remove_prefix(iScheme + 3);
	sUrl = sUrl.substr(0, sUrl.find('#'));
	const size_t iPath = sUrl.find_first_of("/?");
	const auto sAuthority = sUrl.substr(0, iPath);
	auto sHost = sAuthority;
	size_t iColon = sAuthority.find(':');
	if (sAuthority.starts_with('['))
	{
		const size_t iEnd = sAuthority.find(']');
		if (iEnd == sAuthority.npos || (iEnd + 1 != sAuthority.size() && sAuthority[iEnd + 1] != ':'))
		{
			return fnInvalid();
		}
		sHost = sAuthority.substr(1, iEnd - 1);
		Net::Ipv6Addr stAddress;
		if (!stAddress.Init(std::string(sHost), 0))
		{
			return fnInvalid();
		}
		iColon = iEnd + 1 == sAuthority.size() ? sAuthority.npos : iEnd + 1;
	}
	else
	{
		sHost = sAuthority.substr(0, iColon);
		if (sHost.find_first_of("[]") != sHost.npos)
		{
			return fnInvalid();
		}
	}
	int32_t iPort = bHttps ? 443 : 80;
	if (iColon != sAuthority.npos)
	{
		const auto sPort = sAuthority.substr(iColon + 1);
		auto stResult = std::from_chars(sPort.data(), sPort.data() + sPort.size(), iPort);
		if (stResult.ec != std::errc{} || stResult.ptr != sPort.data() + sPort.size())
		{
			return fnInvalid();
		}
	}
	Client stClient(std::string(sHost), iPort, stOptions.iTimeoutMilliseconds);
	stClient.m_bHttps = bHttps;
	if (bHttps)
	{
		stClient.m_stOptions.sCaFile = stOptions.sCaFile;
	}
	const auto sPath = iPath == sUrl.npos ? std::string_view("/") : sUrl.substr(iPath);
	if (sPath.starts_with('?'))
	{
		return stClient.Build(stRequest, "/" + std::string(sPath));
	}
	return stClient.Build(stRequest, sPath);
}

Call Client::Get(const std::string& sPath, const std::vector<std::pair<std::string, std::string>>& vQuery)
{
	Request stRequest;
	stRequest.sPath = QueryTarget(sPath, vQuery);
	return Send(stRequest);
}

Call Client::Post(const std::string& sPath, const std::string& sBodyLocal, const std::string& sContentType)
{
	Request stRequest;
	stRequest.sMethod = "POST";
	stRequest.sPath = sPath;
	stRequest.sBody = sBodyLocal;
	stRequest.sContentType = sContentType;
	return Send(stRequest);
}

Call Client::Send(const Request& stRequest)
{
	return Build(stRequest, stRequest.sPath);
}

Call Client::Build(const Request& stRequest, std::string_view sPath) const
{
	auto pRequest = std::make_shared<Call::Request>();
	pRequest->stOrigin = {m_sHost, m_iPort, m_bHttps};
	Net::Ipv6Addr stIpv6;
	if (stIpv6.Init(m_sHost, 0))
	{
		pRequest->stOrigin.sHost = stIpv6.Host();
	}
	else
	{
		for (auto& iByte : pRequest->stOrigin.sHost)
		{
			if (iByte >= 'A' && iByte <= 'Z')
			{
				iByte += 'a' - 'A';
			}
		}
	}
	pRequest->sCaFile = m_stOptions.sCaFile;
	pRequest->bReuse = m_bReuse && !stRequest.bFreshConnection;
	pRequest->iTimeoutMilliseconds = m_stOptions.iTimeoutMilliseconds;
	if (m_iPort <= 0 || m_iPort > 65535 || m_sHost.empty() || std::any_of(m_sHost.begin(), m_sHost.end(), [](unsigned char iByte)
	{
		return iByte <= 32 || iByte == 127 || std::string_view("/?#@").find(iByte) != std::string_view::npos;
	}))
	{
		pRequest->iCode = OI_ERR_INVALID_ARGUMENT;
		pRequest->sError = "invalid HTTP authority";
		return Call(pRequest);
	}
	const bool bPost = stRequest.sMethod == "POST";
	if ((!bPost && stRequest.sMethod != "GET") || (!bPost && (!stRequest.sBody.empty() || stRequest.pBodySource)) ||
	    (stRequest.pBodySource && !stRequest.sBody.empty()) ||
	    !HeaderFields::ValidForSend(stRequest))
	{
		pRequest->iCode = OI_ERR_INVALID_ARGUMENT;
		pRequest->sError = "invalid HTTP method, body or headers";
		return Call(pRequest);
	}
	std::string sTarget = QueryTarget(sPath, {stRequest.mQuery.begin(), stRequest.mQuery.end()});
	if (sTarget.front() != '/' || std::any_of(sTarget.begin(), sTarget.end(), [](unsigned char iByte)
	{
		return iByte <= 32 || iByte == 127 || iByte == '#';
	}))
	{
		pRequest->iCode = OI_ERR_INVALID_ARGUMENT;
		pRequest->sError = "invalid HTTP request target";
		return Call(pRequest);
	}
	auto& sWire = pRequest->sRequest;
	sWire = stRequest.sMethod + " " + sTarget + " HTTP/1.1\r\n";
	sWire += HostHeader(m_sHost, m_iPort, m_bHttps);
	for (const auto& [sKey, sValue] : stRequest.vHeaders)
	{
		sWire += sKey + ": " + sValue + "\r\n";
	}
	if (!stRequest.sContentType.empty())
	{
		sWire += "Content-Type: " + stRequest.sContentType + "\r\n";
	}
	pRequest->pSource = stRequest.pBodySource;
	pRequest->pSink = stRequest.pBodySink;
	if (pRequest->pSource)
	{
		sWire += "Transfer-Encoding: chunked\r\n";
	}
	else if (bPost)
	{
		sWire += "Content-Length: " + std::to_string(stRequest.sBody.size()) + "\r\n";
	}
	sWire += pRequest->bReuse ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
	sWire += stRequest.sBody;
	return Call(pRequest);
}

}
}
