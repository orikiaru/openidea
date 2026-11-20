#pragma once
#include "oi_channel.h"

namespace OI
{
namespace Net
{
// 本地非阻塞 socketpair。读端挂 poll；持有者串行化写入和关闭写端。
class FifoChannel : public Channel
{
	net_socket_t m_iWriter = INVALID_SOCKET_ID;

  protected:
	explicit FifoChannel(Session* pSessionLocal);
	int32_t OpenPair();
	bool ReadWakeup();
	void CloseWriter();

  public:
	~FifoChannel() override;
	void Wakeup();
	void DataOut() override;
	bool TryClose() override;
	int32_t AllowSend() override;
};
}
}
