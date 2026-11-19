#pragma once
#include "oi_singleton.h"
#include "oi_coro.h"
#include "oi_http.h"

class WebClient
{
	DECLARE_SINGLETON(WebClient)
	WebClient() = default;
	static OI::Coro::Task Run();
	static OI::Coro::Task PoolGet(std::string sUrl, int32_t iTimeoutMilliseconds, bool bFresh, OI::Http::Response& stResponse);
	static OI::Coro::Task PoolRelease(std::string sControlUrl, OI::Http::Response& stResponse);
	static OI::Coro::Task PoolQueue(std::string sBase, std::string sControlUrl, OI::Http::Response& stQueued, OI::Http::Response& stQueuedSuccess, OI::Http::Response& stReady, OI::Http::Response& stRelease);
	static OI::Coro::Task RunPoolRegression(int32_t iPort, int32_t iControlPort);
	static OI::Coro::Task RunDisconnectRegression(int32_t iPort, int32_t iControlPort);
	static OI::Coro::Task DisconnectQueue(std::string sBase, std::string sControl, OI::Http::Response& stSecond, OI::Http::Response& stReady, OI::Http::Response& stProbe, OI::Http::Response& stRelease);
	static OI::Coro::Task DisconnectRelease(std::string sBase, std::string sControl, OI::Http::Response& stProbe, OI::Http::Response& stRelease);
	static OI::Coro::Task UrlGet(std::string sUrl, OI::Http::Response& stResponse);
	static OI::Coro::Task RunUrlRegression(int32_t iPort, int32_t iTlsPort, const std::string& sCaFile);
	static OI::Coro::Task TlsGet(OI::Http::Client& stClient, OI::Http::Response& stResponse);
	static OI::Coro::Task RunTlsRegression(const std::string& sHost, int32_t iPort, const std::string& sCaFile, const std::string& sCase, int32_t iStallPort);

  public:
	void Start();
};
