#pragma once
#include "oi_bytes.h"
#include "oi_channel.h"
#include "oi_header.h"
#include <deque>
namespace OI
{
namespace Net
{

class SessionManager;
// 绑定口负责读。每个 peer 通过 dup 取得自己的描述符和通道，在本会话的 fd 上发送整包。
class UDP_Channel : public Channel
{
	friend class SessionManager;
	friend class Session;
	SessionManager* m_pSessionManager;
	uint16_t m_iLocalPort;
	::OI::Base::Bytes m_stInput;
	// 每项是一个发往本 peer 的完整数据报。相邻协议帧可合包；裸包独占一项。
	std::deque<::OI::Base::Bytes> m_vPackets;
	bool m_bTailFramed = false;
	int32_t QueuePacket(bool bFramed);
	bool Flush();
	UDP_Channel(NetSocket stSocketLocal, SessionManager* pMgr, uint16_t iLocalPort, Session* pPeer);

  public:
	static int32_t StartBind(SessionManager* pMgr);
	uint16_t LocalPort() const
	{
		return m_iLocalPort;
	}
	int32_t AllowSend() override;
	void DataIn() override;
	void DataOut() override;
	bool TryClose() override
	{
		return false;
	}
};
}
}
