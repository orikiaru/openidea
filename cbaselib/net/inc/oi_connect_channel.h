#pragma once
#include "oi_channel.h"
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

  public:
	ConnectChannel(NetSocket stTextParam, SessionManager* pMgrParam, uint64_t iGenerationParam)
	    : Channel(stTextParam, 0), m_bClose(false), m_iGeneration(iGenerationParam), m_pSessionManager(pMgrParam)
	{
	}
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
