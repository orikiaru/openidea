#pragma once
#include "oi_blocking_pool.h"
#include "oi_channel.h"
#include <exception>
#include <functional>

namespace OI
{
namespace Net
{
// 进程内完成通知，无网络报文，不对外监听端口。
struct WorkCompletion
{
	uint64_t iOperation;
	std::exception_ptr stError;
};

// 独立分类工作池的完成通知通道，唤醒通过 poll 的异步队列。
// 后台线程不访问 Session；通知回到 poll 后统一经 DataIn -> Session::OnDataIn。
class BlockingWorkChannel : public Channel
{
	struct Request;
	const WorkCompletion* m_pCompletion = nullptr;
	OI::Base::Bytes* m_pPacket = nullptr;

  public:
	explicit BlockingWorkChannel(Session* pSession);
	// 仅 poll 调用；失败抛异常，成功后通过 Session 完成恰好一次。
	int32_t Submit(OI::Thread::BlockingType iType, uint64_t iOperation, std::function<void()> fnWork, OI::Base::Bytes stPacket);
	// 只在 Session::OnDataIn 调用期间有效，不转交给后台线程。
	const WorkCompletion& Completion() const;
	void DataIn() override;
	void DataOut() override;
	bool TryClose() override;
	int32_t AllowSend() override;
};
}
}
