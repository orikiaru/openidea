#pragma once
#include "oi_coro.h"
#include "oi_http_message.h"
#include "oi_session_manager.h"
#include <map>
#include <string>

namespace OI::Http
{

class ServerManager : public Net::SessionManager
{
  public:
	using Handler = Coro::Task (*)(const Request&, Response&);
	using SinkFactory = std::shared_ptr<BodySink> (*)(const Request&);
	// 在监听前注册。请求和回复由框架保活到 handler 完成。
	void Register(std::string sMethod, std::string sPath, Handler fnHandler);
	void RegisterStream(std::string sMethod, std::string sPath, SinkFactory fnFactory, Handler fnHandler);
	SinkFactory FindSink(const Request& stRequest) const;
	Handler Find(const Request& stRequest) const;

  private:
	struct Route
	{
		Handler fnHandler;
		SinkFactory fnFactory;
	};
	std::map<std::pair<std::string, std::string>, Route> m_mRoutes;
	Net::Session* CloneSession() override;
	void OnAddSession(Net::Session*) override
	{
	}
	void OnDelSession(const Net::Session*) override
	{
	}
};
}
