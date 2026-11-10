#include "oi_blocking_work_session.h"
#include "oi_blocking_work_channel.h"
#include "oi_thread.h"
#include "oi_bytes_stream.h"
#include "work_protocol/oi_rpcdata_workcompleted.h"
#include "work_protocol/rpc_enums.h"
#include <stdexcept>

namespace OI
{
namespace Net
{
class BlockingWorkSessionManager::BlockingWorkSession : public Session
{
	BlockingWorkSessionManager& m_stManager;
	BlockingWorkChannel m_stChannel;
	void Close(int iError) override
	{
		iErrorCode.store(iError);
		iStatus.store(IO_ST_CLOSED);
	}

  public:
	explicit BlockingWorkSession(BlockingWorkSessionManager& stManager)
	    : Session(&stManager), m_stManager(stManager), m_stChannel(this)
	{
		SetChannel(&m_stChannel);
	}
	~BlockingWorkSession() override
	{
		DetachChannel();
	}
	void Submit(uint64_t iOperation, std::function<void()> fnWork)
	{
		OI::RpcData::WorkCompleted stMessage;
		stMessage.iOperation = iOperation;
		OI::Base::BytesStream stPacket;
		stPacket.WritePb(OI::RpcData::WORK_PROTOCOL_COMPLETED, stMessage);
		m_stChannel.Submit(iOperation, std::move(fnWork), std::move(stPacket));
	}
	void OnDataIn() override
	{
		const auto& stCompletion = m_stChannel.Completion();
		OI::Base::BytesStream stPacket;
		stPacket.Swap(stInputBuffer);
		try
		{
			uint32_t iProtocol = 0;
			uint32_t iSize = 0;
			if (stPacket.PreparePbHeader(iProtocol, iSize) != OI::Base::PB_HEADER_OK || stPacket.RemainSize() != iSize)
			{
				throw OI::Base::ParserException();
			}
			// 协议 ID 就是通知类型。新增协议只加生成结构与对应分发，不改通道。
			switch (iProtocol)
			{
			case OI::RpcData::WORK_PROTOCOL_COMPLETED:
			{
				OI::RpcData::WorkCompleted stMessage;
				stPacket >> stMessage;
				if (stMessage.iOperation != stCompletion.iOperation)
				{
					throw OI::Base::ParserException();
				}
				m_stManager.Complete(stMessage.iOperation, stCompletion.stError);
				break;
			}
			default:
				throw std::runtime_error("unknown work protocol id=" + std::to_string(iProtocol));
			}
		}
		catch (...)
		{
			// 内部通知损坏也必须结束对应等待，不能留下永久挂起的任务。
			m_stManager.Complete(stCompletion.iOperation, std::current_exception());
			return;
		}
	}
	void OnDataOut() override
	{
	}
	void ActiveClose() override
	{
		Close(0);
	}
	void PassiveClose(int iError) override
	{
		Close(iError);
	}
};

struct BlockingWorkSessionManager::Submission
{
	std::function<void()> fnWork;
	std::function<void(int32_t, std::exception_ptr)> fnDone;
	void Finish(int32_t iStatus, std::exception_ptr stError) noexcept
	{
		try
		{
			fnDone(iStatus, stError);
		}
		catch (...)
		{
			OI::Thread::ReportTaskError(std::current_exception());
		}
	}
};

BlockingWorkSessionManager::BlockingWorkSessionManager() = default;

BlockingWorkSessionManager::~BlockingWorkSessionManager()
{
	// 管理器比 poll 晚构造，先析构；必须先排空后台访问再释放会话。
	Poll::GetInstance().StopAndJoin();
	CloseAllSessions();
}

Session* BlockingWorkSessionManager::CloneSession()
{
	return new BlockingWorkSession(*this);
}

void BlockingWorkSessionManager::OnAddSession(Session*)
{
}

void BlockingWorkSessionManager::OnDelSession(const Session*)
{
}

void BlockingWorkSessionManager::Submit(std::function<void()> fnWork, std::function<void(int32_t, std::exception_ptr)> fnDone)
{
	auto pSubmission = std::make_unique<Submission>(Submission{std::move(fnWork), std::move(fnDone)});
	auto* pSubmitted = pSubmission.release();
	try
	{
		Poll::GetInstance().CallOnLoop([](void* pArgument)
		{
			std::unique_ptr<Submission> pOwned(static_cast<Submission*>(pArgument));
			GetInstance().SubmitOnLoop(*pOwned);
		}, pSubmitted);
	}
	catch (...)
	{
		delete pSubmitted;
		throw;
	}
}

void BlockingWorkSessionManager::SubmitOnLoop(Submission& stSubmission) noexcept
{
	uint64_t iOperation = 0;
	// 与 RPC 一样，常规拒绝通过返回码交回；C++ 异常仍在等待处重抛。
	const int32_t iAdmission = m_bClosed || OI::Thread::ThreadPool::GetInstance().StopRequested()
	                               ? OI_ERR_CONNECT
	                               : (m_mPending.size() >= 1024 ? OI_ERR_BUSY : OI_OK);
	try
	{
		if (iAdmission != OI_OK)
		{
			stSubmission.Finish(iAdmission, nullptr);
			return;
		}
		if (!m_pSession)
		{
			std::unique_ptr<BlockingWorkSession> pSessionLocal(static_cast<BlockingWorkSession*>(CloneSession()));
			AddSession(pSessionLocal.get(), "local-work", 0, 64 * 1024);
			m_pSession = std::move(pSessionLocal);
		}
		if (m_pSession->IsClosed())
		{
			stSubmission.Finish(OI_ERR_CONNECT, nullptr);
			return;
		}
		// 操作号只在 poll 上分配，0 留给登记失败；回绕时不覆盖仍在途的项。
		do
		{
			iOperation = ++m_iNextOperation;
		} while (iOperation == 0 || m_mPending.contains(iOperation));
		m_mPending.emplace(iOperation, stSubmission.fnDone);
		m_pSession->Submit(iOperation, std::move(stSubmission.fnWork));
	}
	catch (...)
	{
		m_mPending.erase(iOperation);
		stSubmission.Finish(OI_OK, std::current_exception());
	}
}

void BlockingWorkSessionManager::Complete(uint64_t iOperation, std::exception_ptr stError)
{
	assert(Poll::GetInstance().InLoopThread());
	auto stNode = m_mPending.extract(iOperation);
	if (stNode.empty())
	{
		return;
	}
	try
	{
		stNode.mapped()(OI_OK, stError);
	}
	catch (...)
	{
		OI::Thread::ReportTaskError(std::current_exception());
	}
	CheckDrained();
}

void BlockingWorkSessionManager::CheckDrained()
{
	if (m_mPending.empty() && m_fnDrained)
	{
		auto fnDone = std::exchange(m_fnDrained, nullptr);
		fnDone(m_pDrainedArgument);
	}
}

void BlockingWorkSessionManager::Drain(void (*fnDone)(void*), void* pArgument)
{
	assert(Poll::GetInstance().InLoopThread());
	m_bClosed = true;
	m_fnDrained = fnDone;
	m_pDrainedArgument = pArgument;
	CheckDrained();
}
}
}
