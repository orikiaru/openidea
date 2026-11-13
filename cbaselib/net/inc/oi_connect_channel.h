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
	// POLLERR 收口。libuv 已停掉 poll，返回还在连也要作废这一代。
	virtual void DataIn();
	// 可写后看连接结果。还在连就留着；成功把套接字交给管理器，失败作废这一代。
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
	// 读 SO_ERROR。false 还在连，非零是失败，0 是连上。
	bool CheckTcpConnect();

  public:
	// 发起非阻塞连接并挂上 poll。失败返回 false。
	static int32_t StartConnect(SessionManager* pMgr, bool bAsync);

  private:
	static int32_t StartTcpConnect(SessionManager* pMgr, bool bAsync);
};
}
}
