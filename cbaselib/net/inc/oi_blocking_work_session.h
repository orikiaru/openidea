#pragma once
#include "oi_session_manager.h"
#include <exception>
#include <functional>

namespace OI
{
namespace Net
{
// 进程常驻的后台工作会话管理器。业务通过 Coro::RunBlocking 使用。
// Submit 从协程线程投递；工作函数在后台，完成通知经 Session 回到 poll。
class BlockingWorkSessionManager : public SessionManager
{
	DECLARE_SINGLETON(BlockingWorkSessionManager)
	class BlockingWorkSession;
	struct Submission;
	BlockingWorkSessionManager();
	std::unique_ptr<BlockingWorkSession> m_pSession;
	std::map<uint64_t, std::function<void(int32_t, std::exception_ptr)>> m_mPending;
	uint64_t m_iNextOperation = 0;
	bool m_bClosed = false;
	void (*m_fnDrained)(void*) = nullptr;
	void* m_pDrainedArgument = nullptr;
	void SubmitOnLoop(Submission& stSubmission) noexcept;
	void CheckDrained();
	Session* CloneSession() override;
	void OnAddSession(Session*) override;
	void OnDelSession(const Session*) override;

  public:
	~BlockingWorkSessionManager();
	// 闭包按值保活，工作异常交给完成回调；完成回调不得抛异常或阻塞。
	// 上限 1024 条；容量不足返回 BUSY，停止/会话关闭返回 CONNECT，函数异常单独交回。
	void Submit(std::function<void()> fnWork, std::function<void(int32_t, std::exception_ptr)> fnDone);
	// Session 在 poll 线程交回操作号；同一操作只消费一次。
	void Complete(uint64_t iOperation, std::exception_ptr stError);
	// 仅 poll 线程调用。关闭入口，已接纳工作全部返回后调用 fnDone。
	void Drain(void (*fnDone)(void*), void* pArgument);
};
}
}
