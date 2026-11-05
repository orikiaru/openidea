#pragma once
#include "oi_channel.h"
#include <memory>
namespace OI
{
namespace Net
{
class SessionManager;
class ConnectChannel : public Channel
{
	bool m_bClose;
	uint64_t m_iGeneration;
	SessionManager* m_pSessionManager;
	std::shared_ptr<SessionManager> m_pManagerOwner;

  public:
	ConnectChannel(NetSocket stTextParam, SessionManager* pMgrParam, uint64_t iGenerationParam);
	~ConnectChannel() override;
	// 只在 poll 线程、且不在本通道回调栈上调用。
	void Cancel();
	virtual void DataIn();
	virtual void DataOut();
	virtual bool TryClose();

  private:
	void Close()
	{
		m_bClose = true;
	}
	bool IsClosed()
	{
		return m_bClose;
	}
	bool CheckTcpConnect();

  public:
	static bool StartConnect(SessionManager* pMgrParam, bool bAsyncParam);

  private:
	static bool StartTcpConnect(SessionManager* pMgrParam, bool bAsyncParam);
};
}
}
