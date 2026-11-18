#pragma once
#include "oi_http_server.h"
#include "oi_singleton.h"

class WebServer : public OI::Http::ServerManager
{
	DECLARE_SINGLETON(WebServer)
	WebServer() = default;
	int32_t m_iDelayMilliseconds = 0;
	bool m_bRpc = false;
	bool m_bRedis = false;
	static OI::Coro::Task Query(const OI::Http::ServerRequest& stRequest, OI::Http::ServerReply& stReply);

  public:
	int32_t Init() override;
	const std::string GetIDString() override
	{
		return "WebServer";
	}
};
