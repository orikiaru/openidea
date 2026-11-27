#include "oi_tls_session.h"
#include "oi_session.h"
#include "oi_net_os.h"
#include <algorithm>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <uv.h>

namespace OI::Net
{
namespace
{
class TlsError
{
  public:
	static std::string Describe(const char* pStage)
	{
		const char* pReason = ERR_reason_error_string(ERR_peek_last_error());
		std::string sMessage(pStage);
		if (pReason)
		{
			sMessage += ": ";
			sMessage += pReason;
		}
		return sMessage;
	}
};
}

class TlsSession::Connection
{
	SSL* m_pSsl = nullptr;
	std::string m_sError;
	int32_t Failed(const char* pStage);

  public:
	Connection() = default;
	~Connection();
	Connection(const Connection&) = delete;
	Connection& operator=(const Connection&) = delete;
	int32_t Init(const TlsClientContext& stContext, std::string sHost);
	int32_t Feed(std::string_view sWire);
	int32_t Handshake(TlsSession& stSession);
	bool Ready() const;
	bool ReceivedClose() const;
	bool HasPending() const;
	int32_t Flush(TlsSession& stSession);
	int32_t Read(TlsSession& stSession, std::string& sPlain, size_t iLimit);
	int32_t Write(TlsSession& stSession, std::string_view sPlain);
	void Shutdown(TlsSession& stSession);
	const std::string& Error() const
	{
		return m_sError;
	}
};

TlsClientContext::TlsClientContext(const std::string& sCaFile)
{
	ERR_clear_error();
	m_pContext = SSL_CTX_new(TLS_client_method());
	if (m_pContext && SSL_CTX_set_min_proto_version(m_pContext, TLS1_2_VERSION) == 1)
	{
		SSL_CTX_set_verify(m_pContext, SSL_VERIFY_PEER, nullptr);
		SSL_CTX_set_options(m_pContext, SSL_OP_NO_RENEGOTIATION);
		int iLoaded = 0;
		if (sCaFile.empty())
		{
#ifdef _WIN32
			iLoaded = SSL_CTX_load_verify_store(m_pContext, "org.openssl.winstore:");
#else
			iLoaded = SSL_CTX_set_default_verify_paths(m_pContext);
#endif
		}
		else if (sCaFile.find('\0') == sCaFile.npos)
		{
			iLoaded = SSL_CTX_load_verify_file(m_pContext, sCaFile.c_str());
		}
		if (iLoaded == 1)
		{
			return;
		}
	}
	m_sError = TlsError::Describe("TLS trust configuration");
	SSL_CTX_free(m_pContext);
	m_pContext = nullptr;
}

TlsClientContext::~TlsClientContext()
{
	SSL_CTX_free(m_pContext);
}

TlsSession::Connection::~Connection()
{
	SSL_free(m_pSsl);
}

int32_t TlsSession::Connection::Init(const TlsClientContext& stContext, std::string sHost)
{
	if (!stContext.Get())
	{
		m_sError = stContext.Error();
		return OI_ERR_CONNECT;
	}
	ERR_clear_error();
	m_pSsl = SSL_new(stContext.Get());
	if (!m_pSsl)
	{
		return Failed("TLS initialization");
	}
	BIO* pIn = BIO_new(BIO_s_mem());
	BIO* pOut = BIO_new(BIO_s_mem());
	if (!pIn || !pOut)
	{
		BIO_free(pIn);
		BIO_free(pOut);
		return Failed("TLS buffers");
	}
	SSL_set_bio(m_pSsl, pIn, pOut);
	SSL_set_connect_state(m_pSsl);
	if (sHost.starts_with('[') && sHost.ends_with(']'))
	{
		sHost = sHost.substr(1, sHost.size() - 2);
	}
	if (sHost.empty() || sHost.front() == '.' || sHost.find('\0') != sHost.npos || sHost.find('*') != sHost.npos)
	{
		return Failed("TLS invalid peer name");
	}
	sockaddr_in stIpv4{};
	sockaddr_in6 stIpv6{};
	if (uv_ip4_addr(sHost.c_str(), 0, &stIpv4) == 0 || uv_ip6_addr(sHost.c_str(), 0, &stIpv6) == 0)
	{
		if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(m_pSsl), sHost.c_str()) == 1)
		{
			return OI_OK;
		}
		return Failed("TLS peer IP");
	}
	SSL_set_hostflags(m_pSsl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS | X509_CHECK_FLAG_NEVER_CHECK_SUBJECT);
	if (SSL_set1_host(m_pSsl, sHost.c_str()) != 1 || SSL_set_tlsext_host_name(m_pSsl, sHost.c_str()) != 1)
	{
		return Failed("TLS peer name");
	}
	return OI_OK;
}

int32_t TlsSession::Connection::Failed(const char* pStage)
{
	const long iVerify = m_pSsl ? SSL_get_verify_result(m_pSsl) : X509_V_OK;
	if (iVerify != X509_V_OK)
	{
		m_sError = std::string(pStage) + ": " + X509_verify_cert_error_string(iVerify);
	}
	else
	{
		m_sError = TlsError::Describe(pStage);
	}
	return OI_ERR_CONNECT;
}

bool TlsSession::Connection::Ready() const
{
	return m_pSsl && SSL_is_init_finished(m_pSsl);
}

bool TlsSession::Connection::ReceivedClose() const
{
	return m_pSsl && (SSL_get_shutdown(m_pSsl) & SSL_RECEIVED_SHUTDOWN);
}

int32_t TlsSession::Connection::Feed(std::string_view sWire)
{
	if (sWire.empty())
	{
		return OI_OK;
	}
	size_t iWritten = 0;
	if (!BIO_write_ex(SSL_get_rbio(m_pSsl), sWire.data(), sWire.size(), &iWritten) || iWritten != sWire.size())
	{
		return Failed("TLS input");
	}
	return OI_OK;
}

bool TlsSession::Connection::HasPending() const
{
	return m_pSsl && BIO_ctrl_pending(SSL_get_wbio(m_pSsl)) != 0;
}

int32_t TlsSession::Connection::Flush(TlsSession& stSession)
{
	auto pBio = SSL_get_wbio(m_pSsl);
	while (HasPending())
	{
		char* pData = nullptr;
		const long iSize = BIO_get_mem_data(pBio, &pData);
		const size_t iTake = std::min({static_cast<size_t>(iSize), stSession.SendCapacity(), size_t{16384}});
		if (!iTake)
		{
			return OI_OK;
		}
		const auto iCode = stSession.Session::SendResult(Base::Bytes(pData, iTake));
		if (iCode == OI_ERR_BUSY)
		{
			return OI_OK;
		}
		if (iCode != OI_OK)
		{
			m_sError = "TLS transport send";
			return iCode;
		}
		char vDiscard[16384];
		if (BIO_read(pBio, vDiscard, static_cast<int>(iTake)) != static_cast<int>(iTake))
		{
			return Failed("TLS output buffer");
		}
	}
	return OI_OK;
}

int32_t TlsSession::Connection::Handshake(TlsSession& stSession)
{
	ERR_clear_error();
	const int iResult = SSL_do_handshake(m_pSsl);
	if (iResult != 1 && SSL_get_error(m_pSsl, iResult) != SSL_ERROR_WANT_READ)
	{
		return Failed("TLS handshake");
	}
	return Flush(stSession);
}

int32_t TlsSession::Connection::Write(TlsSession& stSession, std::string_view sPlain)
{
	ERR_clear_error();
	size_t iWritten = 0;
	if (SSL_write_ex(m_pSsl, sPlain.data(), sPlain.size(), &iWritten) != 1 || iWritten != sPlain.size())
	{
		return Failed("TLS write");
	}
	return Flush(stSession);
}

int32_t TlsSession::Connection::Read(TlsSession& stSession, std::string& sPlain, size_t iLimit)
{
	char vData[16384];
	for (;;)
	{
		size_t iRead = 0;
		ERR_clear_error();
		const int iResult = SSL_read_ex(m_pSsl, vData, sizeof(vData), &iRead);
		if (iResult != 1)
		{
			const int iError = SSL_get_error(m_pSsl, iResult);
			if (iError != SSL_ERROR_WANT_READ && iError != SSL_ERROR_ZERO_RETURN)
			{
				return Failed("TLS read");
			}
			return Flush(stSession);
		}
		if (sPlain.size() > iLimit || iRead > iLimit - sPlain.size())
		{
			m_sError = "TLS response size";
			return OI_ERR_PROTOCOL;
		}
		sPlain.append(vData, iRead);
	}
}

void TlsSession::Connection::Shutdown(TlsSession& stSession)
{
	if (Ready() && !(SSL_get_shutdown(m_pSsl) & SSL_SENT_SHUTDOWN))
	{
		ERR_clear_error();
		SSL_shutdown(m_pSsl);
		Flush(stSession);
	}
}

TlsSession::TlsSession(SessionManager* pManager, const TlsClientContext* pContext, const std::string& sHost)
    : NetSession(pManager)
{
	if (pContext)
	{
		m_pTls = std::make_unique<Connection>();
		m_pTls->Init(*pContext, sHost);
	}
}

TlsSession::~TlsSession() = default;

void TlsSession::PostDrain()
{
	if (m_bFlushPosted || m_bEnded || IsClosed() || !m_pTls)
	{
		return;
	}
	if (!m_pTls->HasPending() && m_sPending.empty())
	{
		return;
	}
	m_bFlushPosted = true;
	AddRef();
	m_stFlushCall.fnCall = [](void* pArgument)
	{
		auto* pSession = static_cast<TlsSession*>(pArgument);
		struct ReleaseGuard
		{
			TlsSession* pSession;
			~ReleaseGuard()
			{
				pSession->Release();
			}
		} stGuard{pSession};
		pSession->m_bFlushPosted = false;
		pSession->DrainOutbound();
	};
	m_stFlushCall.pArgument = this;
	Poll::GetInstance().PostOnLoop(m_stFlushCall);
}

void TlsSession::DrainOutbound()
{
	if (IsClosed() || m_bEnded || !m_pTls || !m_pTls->Error().empty())
	{
		return;
	}
	int32_t iCode = m_pTls->Flush(*this);
	if (iCode != OI_OK)
	{
		m_bEnded = true;
		OnTransportEnd(iCode, TransportError());
		return;
	}
	// 密文还占着 BIO 时不能再塞明文，等下一次写出后续上。
	if (m_pTls->HasPending() || !TransportReady() || m_sPending.empty())
	{
		return;
	}
	std::string sPending = std::move(m_sPending);
	iCode = m_pTls->Write(*this, sPending);
	if (iCode != OI_OK)
	{
		m_bEnded = true;
		OnTransportEnd(iCode, TransportError());
	}
}

void TlsSession::OnBytesSend(size_t iBytes)
{
	Session::OnBytesSend(iBytes);
	PostDrain();
}

bool TlsSession::TransportReady() const
{
	return !m_pTls || m_pTls->Ready();
}

bool TlsSession::PeerEnded() const
{
	return m_bEnded || (m_pTls && m_pTls->ReceivedClose());
}

const char* TlsSession::TransportError() const
{
	if (m_pTls && !m_pTls->Error().empty())
	{
		return m_pTls->Error().c_str();
	}
	return "transport send";
}

int32_t TlsSession::StartTransport()
{
	if (!m_pTls)
	{
		return OI_OK;
	}
	if (!Poll::GetInstance().InLoopThread())
	{
		return OI_ERR_WRONG_THREAD;
	}
	if (IsClosed() || PeerEnded() || !m_pTls->Error().empty())
	{
		return OI_ERR_CONNECT;
	}
	if (m_bStarted)
	{
		return OI_OK;
	}
	m_bStarted = true;
	return AdvanceHandshake();
}

int32_t TlsSession::AdvanceHandshake()
{
	const int32_t iCode = m_pTls->Handshake(*this);
	if (iCode != OI_OK || !TransportReady() || m_sPending.empty())
	{
		return iCode;
	}
	std::string sPending = std::move(m_sPending);
	return m_pTls->Write(*this, sPending);
}

int32_t TlsSession::SendResult(const Base::Bytes& stBytes)
{
	if (!m_pTls)
	{
		return NetSession::SendResult(stBytes);
	}
	const int32_t iCode = StartTransport();
	if (iCode != OI_OK || !stBytes.Size())
	{
		return iCode;
	}
	{
		OI::Thread::MutexGuard stGuard(stLocker);
		if (iQueuedBytes > iMaxSendQueueBytes || m_sPending.size() > iMaxSendQueueBytes - iQueuedBytes ||
		    stBytes.Size() > iMaxSendQueueBytes - iQueuedBytes - m_sPending.size())
		{
			return OI_ERR_BUSY;
		}
	}
	if (!TransportReady() || m_pTls->HasPending())
	{
		// 明文已计入额度。握手未完成，或上一程密文还没入队时，先收下，避免调用方把 BUSY 当成失败拆连接。
		m_sPending.append(stBytes.Data(), stBytes.Size());
		if (m_pTls->HasPending())
		{
			PostDrain();
		}
		return OI_OK;
	}
	return m_pTls->Write(*this, {stBytes.Data(), stBytes.Size()});
}

void TlsSession::OnDataIn()
{
	if (IsClosed() || m_bEnded || !stInputBuffer.Size())
	{
		return;
	}
	Base::Bytes stWire;
	stWire.Swap(stInputBuffer);
	const std::string_view sWire(stWire.Data(), stWire.Size());
	if (!m_pTls)
	{
		OnPlaintext(sWire);
		return;
	}
	int32_t iCode = StartTransport();
	if (iCode == OI_OK)
	{
		iCode = m_pTls->Feed(sWire);
	}
	if (iCode == OI_OK && !TransportReady())
	{
		iCode = AdvanceHandshake();
	}
	std::string sPlain;
	if (iCode == OI_OK && TransportReady())
	{
		iCode = m_pTls->Read(*this, sPlain, iMaxReceiveBufferBytes);
	}
	if (iCode != OI_OK)
	{
		m_bEnded = true;
		OnTransportEnd(iCode, TransportError());
		return;
	}
	if (!sPlain.empty())
	{
		OnPlaintext(sPlain);
	}
	if (m_pTls->ReceivedClose())
	{
		EndTransport();
	}
}

void TlsSession::EndTransport()
{
	if (m_bEnded)
	{
		return;
	}
	m_bEnded = true;
	std::string().swap(m_sPending);
	const int32_t iError = GetError();
	if (!m_pTls && (iError != 0 || !bReadEnded))
	{
		OnTransportEnd(OI_ERR_CONNECT, iError > 0 ? NetOs_StrError(iError) : "transport closed");
		return;
	}
	if (m_pTls && !m_pTls->ReceivedClose())
	{
		OnTransportEnd(OI_ERR_CONNECT, TransportReady() ? "TLS connection closed without close_notify" : "TLS handshake closed");
		return;
	}
	OnTransportEnd(OI_OK, nullptr);
}

void TlsSession::ShutdownTransport()
{
	if (m_pTls)
	{
		m_pTls->Shutdown(*this);
	}
}

void TlsSession::OnClose()
{
	EndTransport();
	NetSession::OnClose();
}
}
