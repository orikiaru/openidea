#pragma once
#include "oi_net_session.h"
#include "oi_poll.h"
#include <string_view>

struct ssl_ctx_st;

namespace OI::Net
{
// Reuse immutable trust configuration across outbound sessions.
class TlsClientContext
{
	friend class TlsSession;
	ssl_ctx_st* m_pContext = nullptr;
	std::string m_sError;
	ssl_ctx_st* Get() const
	{
		return m_pContext;
	}
	const std::string& Error() const
	{
		return m_sError;
	}

  public:
	explicit TlsClientContext(const std::string& sCaFile = {});
	~TlsClientContext();
	TlsClientContext(const TlsClientContext&) = delete;
	TlsClientContext& operator=(const TlsClientContext&) = delete;
};

// An outbound byte stream. Network callbacks and TLS writes run on the poll thread.
// A null context selects plain TCP. OnPlaintext borrows bytes only for the callback.
class TlsSession : public NetSession
{
	class Connection;
	std::unique_ptr<Connection> m_pTls;
	std::string m_sPending;
	bool m_bStarted = false;
	bool m_bEnded = false;
	bool m_bFlushPosted = false;
	Poll::QueuedCall m_stFlushCall;
	void PostDrain();
	void DrainOutbound();
	void OnBytesSend(size_t iBytes) override;
	int32_t AdvanceHandshake();
	void OnDataIn() final override;
	void OnClose() override;

  protected:
	TlsSession(SessionManager* pManager, const TlsClientContext* pContext, const std::string& sHost);
	virtual void OnPlaintext(std::string_view sData) = 0;
	virtual void OnTransportEnd(int32_t iCode, const char* pReason) = 0;
	bool TransportReady() const;
	bool PeerEnded() const;
	void ShutdownTransport();

  public:
	~TlsSession() override;
	// For client protocols which read first. Sending starts the handshake automatically.
	int32_t StartTransport();
	int32_t SendResult(const Base::Bytes& stBytes) final override;
	const char* TransportError() const;
	// Notify logical closure from SessionManager::OnSessionClosed, before physical teardown.
	void EndTransport();
};
}
