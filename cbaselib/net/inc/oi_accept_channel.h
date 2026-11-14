#pragma once
#include "oi_channel.h"
namespace OI
{
namespace Net
{

class SessionManager;
// 监听套接字。可读时 accept，不承载业务数据。
class AcceptChannel : public Channel
{
	// 新连接交给它建会话。
	SessionManager* m_pSessionManager;

  public:
	AcceptChannel(NetSocket stText, SessionManager* pMgr)
	    : Channel(stText, 0), m_pSessionManager(pMgr)
	{
	}
	// 有新连接时 accept，并交给管理器建会话。
	void DataIn() override;
	// 被报成可写时同样去 accept。监听套接字不写业务数据。
	void DataOut() override;
	// 监听通道不从这里回收，始终返回 false。
	bool TryClose() override;

  public:
	// 创建监听套接字并挂上 poll。失败返回 false。
	static int32_t StartAccept(SessionManager* pMgr, bool bAsync);

  private:
	static int32_t StartTcpAccept(SessionManager* pMgr, bool bAsync);
};
}
}
