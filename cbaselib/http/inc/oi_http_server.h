#pragma once
#include "oi_coro.h"
#include "oi_http_message.h"
#include "oi_poll.h"
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
	// 半帧超时只留会话号和代次。定时器回来由管理器在自己的锁里核对，再关会话。
	void ArmIncomplete(sid_t iSessionId, uint64_t iGeneration);
	void ClearIncomplete(sid_t iSessionId);
	void CloseIncomplete(sid_t iSessionId, uint64_t iGeneration);
	// 锁内确认会话还在表里并加引用，锁外调用。回调可以再进 CloseSession。
	template <typename F>
	void UseSession(sid_t iSessionId, F fnFunction)
	{
		if (!Net::Poll::GetInstance().InLoopThread())
		{
			return;
		}
		Net::Session* pSession = nullptr;
		{
			::OI::Thread::MutexGuard stLock(stLocker);
			auto it = mSessionMap.find(iSessionId);
			if (it == mSessionMap.end())
			{
				return;
			}
			pSession = it->second;
			pSession->AddRef();
		}
		struct ReleaseGuard
		{
			Net::Session* pSession;
			~ReleaseGuard()
			{
				pSession->Release();
			}
		} stGuard{pSession};
		fnFunction(*pSession);
	}

  private:
	struct Route
	{
		Handler fnHandler;
		SinkFactory fnFactory;
	};
	std::map<std::pair<std::string, std::string>, Route> m_mRoutes;
	// 还没收齐的半帧。会话号到这一帧的代次。
	std::map<sid_t, uint64_t> m_mIncomplete;
	void OnSessionClosed(sid_t iSessionId) override;
	Net::Session* CloneSession() override;
	void OnAddSession(Net::Session*) override
	{
	}
	void OnDelSession(const Net::Session*) override
	{
	}
};
}
