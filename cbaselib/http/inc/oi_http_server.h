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
	// 在监听前注册。请求和回复由框架保活到 handler 完成。
	void Register(std::string sMethod, std::string sPath, Handler fnHandler);
	Handler Find(const Request& stRequest) const;

  private:
	std::map<std::pair<std::string, std::string>, Handler> m_mRoutes;
	Net::Session* CloneSession() override;
	void OnAddSession(Net::Session*) override
	{
	}
	void OnDelSession(const Net::Session*) override
	{
	}
};
}
