#include "oi_http.h"
#include "oi_channel.h"
#include "oi_net_os.h"
#include "oi_poll.h"
#include "oi_rpc_coro.h"
#include "oi_socket.h"
#include "oi_thread.h"
#include "oi_timer_manager.h"
#include "uv.h"
#include <atomic>
#include <cstdio>
#include <cstring>

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
}

struct Call::State
{
	enum Phase
	{
		PHASE_CONNECT = 0,
		PHASE_WRITE = 1,
		PHASE_READ = 2,
	};

	std::string sHost;
	int32_t iPort;
	int32_t iTimeoutMilliseconds;
	std::string sRequest;
	std::string sIncoming;
	std::string sBody;
	std::string sError;
	size_t iSent;
	size_t iHeaderBytes;
	uint64_t iContentLength;
	int32_t iStatus;
	int32_t iPhase;
	unsigned iInterimResponses = 0;
	bool bHeadersReady;
	bool bHasLength;
	bool bChunked;
	size_t iChunkPosition;
	bool bFailed;
	bool bInIo;
	net_socket_t iDescriptor;
	HttpChannel* pChannel;
	std::coroutine_handle<> stHandle;
	std::atomic<int32_t> iDone;
	std::atomic<int32_t> iWoke;
	// Call、超时定时器、域名解析各持一份。定时器不能取消，请求结束后它还会来。
	std::atomic<int32_t> iReferences;
	uv_getaddrinfo_t stDns;

	State()
	    : iPort(0), iTimeoutMilliseconds(5000), iSent(0), iHeaderBytes(0), iContentLength(0), iStatus(0), iPhase(PHASE_CONNECT),
	      bHeadersReady(false), bHasLength(false), bChunked(false), iChunkPosition(0), bFailed(false), bInIo(false),
	      iDescriptor(INVALID_SOCKET_ID), pChannel(NULL), stHandle(), iDone(0), iWoke(0), iReferences(1)
	{
	}
	~State();

	void Retain()
	{
		iReferences.fetch_add(1);
	}

	void Release()
	{
		if (iReferences.fetch_sub(1) == 1)
		{
			delete this;
		}
	}

	void Wake()
	{
		if (iWoke.exchange(1) != 0)
		{
			return;
		}
		if (stHandle)
		{
			coro::Schedule(stHandle);
		}
	}

	void DropChannel();

	void Complete()
	{
		if (iDone.exchange(1) != 0)
		{
			return;
		}
		// 完成后不会再读写网络缓冲。先释放容量，再把结果发布给工作线程。
		std::string().swap(sRequest);
		std::string().swap(sIncoming);
		if (bInIo)
		{
			return;
		}
		DropChannel();
		Wake();
	}

	void Fail(const char* pWhyParam)
	{
		// 都在 poll 线程上调。已经收完的结果不能被晚到的超时改掉。
		if (iDone.load() != 0)
		{
			return;
		}
		bFailed = true;
		if (sError.empty() && pWhyParam)
		{
			sError = pWhyParam;
		}
		Complete();
	}

	bool ParseHeaders(size_t iHeaderEndParam)
	{
		size_t iLineEnd = sIncoming.find("\r\n");
		if (iLineEnd == std::string::npos || iLineEnd > iHeaderEndParam)
		{
			return false;
		}
		if (iLineEnd < 12 || (sIncoming.compare(0, 9, "HTTP/1.1 ") != 0 && sIncoming.compare(0, 9, "HTTP/1.0 ") != 0))
		{
			return false;
		}
		if (iLineEnd > 12 && sIncoming[12] != ' ')
		{
			return false;
		}
		uint64_t iCodeLocal = 0;
		if (!ParseU64(sIncoming, 9, 12, iCodeLocal) || iCodeLocal < 100 || iCodeLocal > 599 || iCodeLocal == 101)
		{
			return false;
		}
		iStatus = static_cast<int32_t>(iCodeLocal);
		bHasLength = false;
		bChunked = false;
		iContentLength = 0;
		size_t iPosition = iLineEnd + 2;
		while (iPosition < iHeaderEndParam)
		{
			size_t iEnd = sIncoming.find("\r\n", iPosition);
			if (iEnd == std::string::npos || iEnd > iHeaderEndParam)
			{
				break;
			}
			size_t iColon = sIncoming.find(':', iPosition);
			if (iColon != std::string::npos && iColon < iEnd)
			{
				size_t iValue = iColon + 1;
				while (iValue < iEnd && (sIncoming[iValue] == ' ' || sIncoming[iValue] == '\t'))
				{
					iValue += 1;
				}
				size_t iValueEnd = iEnd;
				while (iValueEnd > iValue && (sIncoming[iValueEnd - 1] == ' ' || sIncoming[iValueEnd - 1] == '\t'))
				{
					--iValueEnd;
				}
				size_t iNameEnd = iColon;
				while (iNameEnd > iPosition && sIncoming[iNameEnd - 1] == ' ')
				{
					iNameEnd -= 1;
				}
				if (EqName(sIncoming, iPosition, iNameEnd, "content-length") && iPosition + std::strlen("content-length") == iNameEnd)
				{
					uint64_t iLength = 0;
					if (!ParseU64(sIncoming, iValue, iValueEnd, iLength) || (iStatus != 304 && iStatus >= 200 && iLength > kMaxBody) || (bHasLength && iContentLength != iLength))
					{
						return false;
					}
					bHasLength = true;
					iContentLength = iLength;
				}
				else if (EqName(sIncoming, iPosition, iNameEnd, "transfer-encoding") && iPosition + std::strlen("transfer-encoding") == iNameEnd)
				{
					std::string sCoding = sIncoming.substr(iValue, iValueEnd - iValue);
					for (size_t iIndexLocal = 0; iIndexLocal < sCoding.size(); ++iIndexLocal)
					{
						if (sCoding[iIndexLocal] >= 'A' && sCoding[iIndexLocal] <= 'Z')
						{
							sCoding[iIndexLocal] = (char)(sCoding[iIndexLocal] - 'A' + 'a');
						}
					}
					if (bChunked || sCoding != "chunked")
					{
						return false;
					}
					bChunked = true;
				}
			}
			iPosition = iEnd + 2;
		}
		return !(bHasLength && bChunked);
	}

	// 头或正文已经收齐就结束。返回 true 表示这一轮不用再读。
	bool Consume()
	{
		while (!bHeadersReady)
		{
			size_t iHeaderEnd = sIncoming.find("\r\n\r\n");
			if (iHeaderEnd == std::string::npos)
			{
				if (sIncoming.size() > kMaxHeader)
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
			if (iStatus < 200)
			{
				if (++iInterimResponses > 16)
				{
					Fail("status");
					return true;
				}
				sIncoming.erase(0, iHeaderEnd + 4);
				continue;
			}
			bHeadersReady = true;
			iHeaderBytes = iHeaderEnd + 4;
			iChunkPosition = iHeaderBytes;
		}
		if (iStatus == 204 || iStatus == 304)
		{
			Complete();
			return true;
		}
		if (bChunked)
		{
			return ConsumeChunks();
		}
		if (!bHasLength)
		{
			return false;
		}
		if (sIncoming.size() - iHeaderBytes < iContentLength)
		{
			return false;
		}
		sBody.assign(sIncoming, iHeaderBytes, (size_t)iContentLength);
		Complete();
		return true;
	}

	// chunk_pos 停在下一个 chunk 的长度行；冲突的长度字段已在头解析阶段拒绝。
	bool ConsumeChunks()
	{
		for (;;)
		{
			size_t iLineEnd = sIncoming.find("\r\n", iChunkPosition);
			if (iLineEnd == std::string::npos)
			{
				if (sIncoming.size() - iChunkPosition > 1024)
				{
					Fail("chunk");
					return true;
				}
				return false;
			}
			uint64_t iSize = 0;
			if (!ParseHex(sIncoming, iChunkPosition, iLineEnd, iSize))
			{
				Fail("chunk");
				return true;
			}
			size_t iDataAt = iLineEnd + 2;
			if (iSize == 0)
			{
				// 最后一块后面可以跟 trailer，以空行结束。
				size_t iPosition = iDataAt;
				for (;;)
				{
					size_t iEnd = sIncoming.find("\r\n", iPosition);
					if (iEnd == std::string::npos)
					{
						return false;
					}
					if (iEnd == iPosition)
					{
						Complete();
						return true;
					}
					iPosition = iEnd + 2;
				}
			}
			if (iSize > kMaxBody || sBody.size() + (size_t)iSize > kMaxBody)
			{
				Fail("size");
				return true;
			}
			if (sIncoming.size() < iDataAt + (size_t)iSize + 2)
			{
				return false;
			}
			if (sIncoming.compare(iDataAt + static_cast<size_t>(iSize), 2, "\r\n") != 0)
			{
				Fail("chunk");
				return true;
			}
			sBody.append(sIncoming, iDataAt, (size_t)iSize);
			iChunkPosition = iDataAt + (size_t)iSize + 2;
		}
	}

	void OnEof()
	{
		if (Consume())
		{
			return;
		}
		if (!bHeadersReady || bHasLength || bChunked)
		{
			Fail("eof");
			return;
		}
		sBody.assign(sIncoming, iHeaderBytes, std::string::npos);
		Complete();
	}

	void PumpRead()
	{
		char vBuffer[16384];
		for (;;)
		{
			int iNumber = NetOs_Recv(iDescriptor, vBuffer, sizeof(vBuffer));
			if (iNumber > 0)
			{
				if (sIncoming.size() + (size_t)iNumber > kMaxHeader + kMaxBody)
				{
					Fail("size");
					return;
				}
				sIncoming.append(vBuffer, (size_t)iNumber);
				if (Consume())
				{
					return;
				}
				continue;
			}
			if (iNumber == 0)
			{
				OnEof();
				return;
			}
			if (errno == EINTR)
			{
				continue;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				return;
			}
			Fail("recv");
			return;
		}
	}

	void PumpWrite()
	{
		while (iSent < sRequest.size())
		{
			int iNumber = NetOs_Send(iDescriptor, sRequest.data() + iSent, sRequest.size() - iSent);
			if (iNumber > 0)
			{
				iSent += (size_t)iNumber;
				continue;
			}
			if (iNumber < 0 && errno == EINTR)
			{
				continue;
			}
			if (iNumber < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			{
				Arm(::OI::Net::IO_EV_WRITE);
				return;
			}
			Fail("send");
			return;
		}
		iPhase = PHASE_READ;
		Arm(::OI::Net::IO_EV_READ);
	}

	void Arm(int32_t iMaskParam);

	void OnWritable()
	{
		if (iDone.load() != 0)
		{
			return;
		}
		bInIo = true;
		if (iPhase == PHASE_CONNECT)
		{
			int iError = NetOs_GetSocketError(iDescriptor);
			if (iError == EINPROGRESS || iError == EINTR || iError == EAGAIN || iError == EWOULDBLOCK)
			{
				bInIo = false;
				return;
			}
			if (iError != 0)
			{
				Fail("connect");
				bInIo = false;
				return;
			}
			iPhase = PHASE_WRITE;
		}
		if (iPhase == PHASE_WRITE)
		{
			PumpWrite();
		}
		bInIo = false;
	}

	void OnReadable()
	{
		if (iDone.load() != 0)
		{
			return;
		}
		if (iPhase != PHASE_READ)
		{
			bInIo = true;
			Fail("connect");
			bInIo = false;
			return;
		}
		bInIo = true;
		PumpRead();
		bInIo = false;
	}

	static void OnTimeout(void* pArgumentParam);
	static void Start(void* pArgumentParam);
	static void OnResolved(uv_getaddrinfo_t* pRequestParam, int iStatusParam, struct addrinfo* pResponseParam);
	void Connect(const sockaddr_in& stAddrParam);
};

class HttpChannel : public ::OI::Net::Channel
{
	Call::State* m_pState;

  public:
	HttpChannel(::OI::Net::NetSocket stSockParam, Call::State* pStateParam)
	    : ::OI::Net::Channel(stSockParam, NULL), m_pState(pStateParam)
	{
	}
	void DataIn()
	{
		m_pState->OnReadable();
	}
	void DataOut()
	{
		m_pState->OnWritable();
	}
	bool TryClose()
	{
		if (m_pState->iDone.load() == 0)
		{
			return false;
		}
		m_pState->pChannel = NULL;
		m_pState->iDescriptor = INVALID_SOCKET_ID;
		Call::State* pStateLocal = m_pState;
		delete this;
		pStateLocal->Wake();
		return true;
	}
};

void Call::State::Arm(int32_t iMaskParam)
{
	if (!pChannel)
	{
		return;
	}
	bool bRead = (iMaskParam & ::OI::Net::IO_EV_READ) != 0;
	bool bWrite = (iMaskParam & ::OI::Net::IO_EV_WRITE) != 0;
	if (pChannel->pPollToken == NULL)
	{
		::OI::Net::Poll::GetInstance().IOEventRegister(pChannel, bRead, bWrite);
		return;
	}
	::OI::Net::Poll::GetInstance().IOEventMod(pChannel, iMaskParam);
}

void Call::State::DropChannel()
{
	if (pChannel)
	{
		HttpChannel* pHeld = pChannel;
		pChannel = NULL;
		iDescriptor = INVALID_SOCKET_ID;
		delete pHeld;
		return;
	}
	if (iDescriptor != INVALID_SOCKET_ID)
	{
		NetOs_Close(iDescriptor);
		iDescriptor = INVALID_SOCKET_ID;
	}
}

void Call::State::OnTimeout(void* pArgumentParam)
{
	State* pSelf = (State*)pArgumentParam;
	pSelf->Fail("timeout");
	pSelf->Release();
}

void Call::State::Start(void* pArgumentParam)
{
	State* pSelf = (State*)pArgumentParam;
	if (pSelf->iPort <= 0 || pSelf->iPort > 65535 || pSelf->sHost.empty())
	{
		pSelf->Fail("host");
		return;
	}
	int32_t iTimeout = pSelf->iTimeoutMilliseconds > 0 ? pSelf->iTimeoutMilliseconds : 5000;
	pSelf->Retain();
	::OI::Timer::BaseTimerManager::GetInstance().AddTimer([pSelf]()
	{
		::OI::Net::Poll::GetInstance().CallOnLoop(State::OnTimeout, pSelf);
	}, iTimeout);
	sockaddr_in stAddr;
	std::memset(&stAddr, 0, sizeof(stAddr));
	stAddr.sin_family = AF_INET;
	stAddr.sin_port = htons((uint16_t)pSelf->iPort);
	if (inet_pton(AF_INET, pSelf->sHost.c_str(), &stAddr.sin_addr) == 1)
	{
		pSelf->Connect(stAddr);
		return;
	}
	// 域名交给 libuv 的线程池解析，回调回到 poll 线程，不卡 poll。
	addrinfo stHints;
	std::memset(&stHints, 0, sizeof(stHints));
	stHints.ai_family = AF_INET;
	stHints.ai_socktype = SOCK_STREAM;
	pSelf->stDns.data = pSelf;
	pSelf->Retain();
	if (uv_getaddrinfo(::OI::Net::Poll::GetInstance().Loop(), &pSelf->stDns, State::OnResolved, pSelf->sHost.c_str(), NULL, &stHints) != 0)
	{
		pSelf->Fail("dns");
		pSelf->Release();
	}
}

void Call::State::OnResolved(uv_getaddrinfo_t* pRequestParam, int iStatusParam, struct addrinfo* pResponseParam)
{
	State* pSelf = (State*)pRequestParam->data;
	if (pSelf->iDone.load() == 0)
	{
		if (iStatusParam != 0 || pResponseParam == NULL || pResponseParam->ai_addr == NULL || pResponseParam->ai_family != AF_INET)
		{
			pSelf->Fail("dns");
		}
		else
		{
			sockaddr_in stAddr = *(const sockaddr_in*)pResponseParam->ai_addr;
			stAddr.sin_port = htons((uint16_t)pSelf->iPort);
			pSelf->Connect(stAddr);
		}
	}
	uv_freeaddrinfo(pResponseParam);
	pSelf->Release();
}

void Call::State::Connect(const sockaddr_in& stAddrParam)
{
	iDescriptor = NetOs_CreateTcp(true);
	if (iDescriptor == INVALID_SOCKET_ID)
	{
		Fail("socket");
		return;
	}
	NetOs_SetNoDelay(iDescriptor);
	::OI::Net::NetSocket stSock;
	stSock.Attach(iDescriptor);
	pChannel = new HttpChannel(stSock, this);
	int iReturnCodeLocal = -1;
	for (;;)
	{
		iReturnCodeLocal = NetOs_Connect(iDescriptor, (const sockaddr*)&stAddrParam, (int)sizeof(stAddrParam));
		if (iReturnCodeLocal == 0 || errno != EINTR)
		{
			break;
		}
	}
	if (iReturnCodeLocal != 0 && errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK)
	{
		Fail("connect");
		return;
	}
	if (iReturnCodeLocal != 0)
	{
		iPhase = PHASE_CONNECT;
		Arm(::OI::Net::IO_EV_WRITE);
		return;
	}
	iPhase = PHASE_WRITE;
	PumpWrite();
}

Call::State::~State()
{
	DropChannel();
}

#ifdef _SINGLE_THREAD_
static bool DrainSt()
{
	return coro::StScheduler::Poll();
}

static bool PendingSt()
{
	return coro::StScheduler::Queued();
}

static struct StReg
{
	StReg()
	{
		::OI::Thread::ThreadPool::GetInstance().SetDrain(DrainSt);
		::OI::Thread::ThreadPool::GetInstance().SetPending(PendingSt);
	}
} g_st_reg;
#endif

Call::~Call()
{
	if (m_pState)
	{
		m_pState->Release();
	}
}

void Call::await_suspend(std::coroutine_handle<> stHandleParam) noexcept
{
	m_pState->stHandle = stHandleParam;
	::OI::Net::Poll::GetInstance().CallOnLoop(State::Start, m_pState);
}

Response Call::await_resume()
{
	Response stOutput;
	stOutput.iStatus = m_pState->iStatus;
	stOutput.sBody = std::move(m_pState->sBody);
	stOutput.bFail = m_pState->bFailed || m_pState->iStatus == 0;
	stOutput.sError = std::move(m_pState->sError);
	m_pState->Release();
	m_pState = nullptr;
	return stOutput;
}

Client::Client(const std::string& sHostParam, int32_t iPortParam, int32_t iTimeoutMillisecondsParam)
    : m_sHost(sHostParam), m_iPort(iPortParam), m_iTimeoutMilliseconds(iTimeoutMillisecondsParam)
{
}

static std::string HostHeader(const std::string& sHostParam, int32_t iPortParam)
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
	Call::State* pStateLocal = new Call::State();
	pStateLocal->sHost = m_sHost;
	pStateLocal->iPort = m_iPort;
	pStateLocal->iTimeoutMilliseconds = m_iTimeoutMilliseconds;
	pStateLocal->sRequest = "GET " + sTarget + " HTTP/1.1\r\n";
	pStateLocal->sRequest += HostHeader(m_sHost, m_iPort);
	pStateLocal->sRequest += "Connection: close\r\n\r\n";
	return Call(pStateLocal);
}

Call Client::Post(const std::string& sPathParam, const std::string& sBodyParam, const std::string& sContentTypeParam)
{
	std::string sTarget = sPathParam.empty() ? "/" : sPathParam;
	Call::State* pStateLocal = new Call::State();
	pStateLocal->sHost = m_sHost;
	pStateLocal->iPort = m_iPort;
	pStateLocal->iTimeoutMilliseconds = m_iTimeoutMilliseconds;
	pStateLocal->sRequest = "POST " + sTarget + " HTTP/1.1\r\n";
	pStateLocal->sRequest += HostHeader(m_sHost, m_iPort);
	pStateLocal->sRequest += "Content-Type: " + sContentTypeParam + "\r\n";
	pStateLocal->sRequest += "Content-Length: " + std::to_string(sBodyParam.size()) + "\r\n";
	pStateLocal->sRequest += "Connection: close\r\n\r\n";
	pStateLocal->sRequest.append(sBodyParam.data(), sBodyParam.size());
	return Call(pStateLocal);
}

}
}
