#pragma once
#include "oi_session.h"
namespace OI
{
namespace Net
{
// 已经连上的 TCP 会话。关闭分主动和被动两条路径，避免管理器重入。
class NetSession : public Session
{
  protected:
	// poll 线程内部关闭。调用方已经在合适的上下文里，这里不再加会话锁。
	void Close(int iError) override;
  public:
	NetSession(SessionManager* pMgr)
	    : Session(pMgr)
	{
	}
	// 管理器或 poll 线程主动关。持锁，不再回调管理器的 CloseSession。
	void ActiveClose() override;
	// poll 线程发现对端或错误时关，并回调管理器的 CloseSession。
	void PassiveClose(int iError) override;
	// 关闭收尾。子类在这里释放自己的协议状态。
	void OnClose() override;
};
}
}
