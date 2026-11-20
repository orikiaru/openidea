#pragma once
#include "oi_blocking_pool.h"
#include "oi_fifo_channel.h"
#include "oi_lock.h"
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

// 一个本地 socketpair。后台只写非阻塞通知字节，读端由 IO 线程监听。
// 后台线程不访问 Session；通知回到 poll 后统一经 DataIn -> Session::OnDataIn。
class BlockingWorkChannel : public FifoChannel
{
	struct Request;
	const WorkCompletion* m_pCompletion = nullptr;
	::OI::Thread::NativeMutex m_stReadyLock;
	std::shared_ptr<Request> m_pReadyHead;
	Request* m_pReadyTail = nullptr;
	size_t m_iInFlight = 0;
	bool m_bAttached = false;

	int32_t Open();
	void Publish(const std::shared_ptr<Request>& pRequest);

  public:
	explicit BlockingWorkChannel(Session* pSession);
	// 仅 poll 调用；失败抛异常，成功后通过 Session 完成恰好一次。
	int32_t Submit(OI::Thread::BlockingType iType, uint64_t iOperation, std::function<void()> fnWork, OI::Base::Bytes stPacket);
	// 只在 Session::OnDataIn 调用期间有效，不转交给后台线程。
	const WorkCompletion& Completion() const;
	void DataIn() override;
};
}
}
