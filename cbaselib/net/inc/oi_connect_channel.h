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

  public:
	ConnectChannel(NetSocket stText, SessionManager* pMgr, uint64_t iGeneration);
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
	static bool StartConnect(SessionManager* pMgr, bool bAsync);

  private:
	static bool StartTcpConnect(SessionManager* pMgr, bool bAsync);
};
}
}
