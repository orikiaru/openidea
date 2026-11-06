#pragma once
#include "oi_channel.h"
#include <memory>
namespace OI
{
namespace Net
{
class SessionManager;
// 一次出向连接。可写表示连接有结果了。
class ConnectChannel : public Channel
{
	// 已经决定放弃这次连接。
	bool m_bClose;
	// 属于哪一代。旧连接完成不能改新一代的状态。
	uint64_t m_iGeneration;
	// 成功后把套接字交给它。
	SessionManager* m_pSessionManager;

  public:
	ConnectChannel(NetSocket stText, SessionManager* pMgr, uint64_t iGeneration);
	// 连接被拒或出错时收口。libuv 这时不一定再报可写。
	virtual void DataIn();
	// 可写后看连接结果。成功把套接字交给管理器，失败作废这一代。
	virtual void DataOut();
	// 已经放弃后摘掉事件并删除自己。还在连接中返回 false。
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
	// 用套接字错误码看连接结果。
	bool CheckTcpConnect();

  public:
	// 发起非阻塞连接并挂上 poll。失败返回 false。
	static bool StartConnect(SessionManager* pMgr, bool bAsync);

  private:
	static bool StartTcpConnect(SessionManager* pMgr, bool bAsync);
};
}
}
