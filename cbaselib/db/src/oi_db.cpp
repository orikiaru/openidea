#include "oi_db.h"
#include "oi_errno.h"
#include "oi_lock.h"
#include <stdexcept>
#include <utility>

namespace OI::Db
{
struct Client::State
{
	OI::Thread::NativeMutex stMutex;
	std::shared_ptr<Driver> pDriver;
	std::vector<std::unique_ptr<Connection>> vIdle;
	size_t iLimit;
	size_t iCount = 0;
	OI::Thread::BlockingType iType;
	bool bClosed = false;

	State(std::shared_ptr<Driver> pDriverLocal, size_t iLimitLocal, OI::Thread::BlockingType iTypeLocal)
	    : pDriver(std::move(pDriverLocal)), iLimit(iLimitLocal), iType(iTypeLocal)
	{
	}
	~State()
	{
		if (vIdle.empty())
		{
			return;
		}
		try
		{
			auto pConnections = std::make_shared<std::vector<std::unique_ptr<Connection>>>();
			pConnections->swap(vIdle);
			const int32_t iStatus = OI::Thread::SubmitBlockingWork(iType, [pConnections, pDriverLocal = pDriver]()
			{
				(void)pDriverLocal;
				pConnections->clear();
			});
			if (iStatus != OI_OK)
			{
				pConnections->clear();
			}
		}
		catch (...)
		{
			// A failed allocation or submission leaves cleanup on this thread.
		}
	}

	void StopAccepting()
	{
		OI::Thread::NativeMutexGuard stGuard(stMutex);
		bClosed = true;
	}

	int32_t CloseIdle()
	{
		std::vector<std::unique_ptr<Connection>> vConnections;
		{
			OI::Thread::NativeMutexGuard stGuard(stMutex);
			vConnections.swap(vIdle);
			iCount -= vConnections.size();
		}
		vConnections.clear();
		return OI_OK;
	}

	int32_t Run(const std::vector<Statement>& vStatements, bool bTransaction, std::vector<Result>& vResults, Error& stError)
	{
		stError = {};
		std::unique_ptr<Connection> pConnection;
		{
			OI::Thread::NativeMutexGuard stGuard(stMutex);
			if (bClosed)
			{
				stError.sStage = "pool";
				stError.sMessage = "connection pool closed";
				return OI_ERR_CONNECT;
			}
			if (!vIdle.empty())
			{
				pConnection = std::move(vIdle.back());
				vIdle.pop_back();
			}
			else if (iCount < iLimit && pDriver)
			{
				++iCount;
			}
			else
			{
				stError.sStage = "pool";
				stError.sMessage = "connection pool full";
				return OI_ERR_BUSY;
			}
		}
		struct Lease
		{
			State& stState;
			std::unique_ptr<Connection>& pConnection;
			Error& stError;
			bool bReusable = false;
			void Reset() noexcept
			{
				if (bReusable && pConnection)
				{
					try
					{
						if (!pConnection->Healthy())
						{
							bReusable = false;
							return;
						}
						stError.stCleanup.bFailed = true;
						Error stResetError;
						bReusable = pConnection->Reset(stResetError);
						if (bReusable)
						{
							stError.stCleanup.bFailed = false;
						}
						else
						{
							stError.stCleanup.sStage = std::move(stResetError.sStage);
							stError.stCleanup.iNativeCode = stResetError.iNativeCode;
							stError.stCleanup.sSqlState = std::move(stResetError.sSqlState);
							stError.stCleanup.sMessage = std::move(stResetError.sMessage);
						}
					}
					catch (...)
					{
						bReusable = false;
						stError.stCleanup.bFailed = true;
						stError.stCleanup.bException = true;
					}
				}
			}
			~Lease()
			{
				OI::Thread::NativeMutexGuard stGuard(stState.stMutex);
				if (bReusable && pConnection && !stState.bClosed)
				{
					try
					{
						stState.vIdle.push_back(std::move(pConnection));
						return;
					}
					catch (...)
					{
					}
				}
				--stState.iCount;
			}
		} stLease{*this, pConnection, stError};
		vResults.clear();
		if (!pConnection)
		{
			pConnection = pDriver->Connect(stError);
			if (!pConnection)
			{
				return OI_ERR_CONNECT;
			}
		}
		Result stControl;
		if (bTransaction)
		{
			const int32_t iStatus = pConnection->Execute({"BEGIN", {}}, stControl, stError);
			if (iStatus != OI_OK)
			{
				return iStatus;
			}
		}
		struct TransactionGuard
		{
			Connection& stConnection;
			bool bActive;
			~TransactionGuard()
			{
				if (bActive)
				{
					try
					{
						Result stIgnored;
						Error stIgnoredError;
						stConnection.Execute({"ROLLBACK", {}}, stIgnored, stIgnoredError);
					}
					catch (...)
					{
					}
				}
			}
		} stTransaction{*pConnection, bTransaction};
		auto fnRollback = [pConnectionLocal = pConnection.get(), pError = &stError]() noexcept -> bool
		{
			try
			{
				Result stIgnored;
				Error stRollbackError;
				if (pConnectionLocal->Execute({"ROLLBACK", {}}, stIgnored, stRollbackError) == OI_OK)
				{
					return true;
				}
				pError->stCleanup.bFailed = true;
				pError->stCleanup.sStage = "rollback";
				pError->stCleanup.iNativeCode = stRollbackError.iNativeCode;
				pError->stCleanup.sSqlState = std::move(stRollbackError.sSqlState);
				pError->stCleanup.sMessage = std::move(stRollbackError.sMessage);
			}
			catch (...)
			{
				pError->stCleanup.bFailed = true;
				pError->stCleanup.bException = true;
			}
			return false;
		};
		for (const auto& stStatement : vStatements)
		{
			Result stResult;
			const int32_t iStatus = pConnection->Execute(stStatement, stResult, stError);
			if (iStatus != OI_OK)
			{
				if (bTransaction)
				{
					stLease.bReusable = fnRollback();
					stTransaction.bActive = false;
				}
				else
				{
					stLease.bReusable = true;
				}
				stLease.Reset();
				return iStatus;
			}
			vResults.push_back(std::move(stResult));
		}
		if (bTransaction)
		{
			const int32_t iStatus = pConnection->Execute({"COMMIT", {}}, stControl, stError);
			if (iStatus != OI_OK)
			{
				(void)fnRollback();
				stTransaction.bActive = false;
				return iStatus;
			}
			stTransaction.bActive = false;
		}
		stLease.bReusable = true;
		stLease.Reset();
		return OI_OK;
	}
};

Client::Client(std::shared_ptr<Driver> pDriver, size_t iConnectionLimit, OI::Thread::BlockingType iType)
{
	if (!pDriver || iConnectionLimit == 0)
	{
		throw std::invalid_argument("database driver and positive connection limit are required");
	}
	OI::Thread::InitializeBlockingPools();
	m_pState = std::make_shared<State>(std::move(pDriver), iConnectionLimit, iType);
}

OI::Coro::TaskResult<int32_t> Client::Close() const
{
	auto pState = m_pState;
	pState->StopAccepting();
	return OI::Coro::RunBlocking(pState->iType, [pState]() -> int32_t
	{
		return pState->CloseIdle();
	});
}

OI::Coro::TaskResult<int32_t> Client::Call(Statement stStatement, Result& stResult, Error& stError) const
{
	auto pState = m_pState;
	return OI::Coro::RunBlocking(pState->iType, [pState, stStatement = std::move(stStatement), pResult = &stResult, pError = &stError]() -> int32_t
	{
		std::vector<Result> vResults;
		const int32_t iStatus = pState->Run({stStatement}, false, vResults, *pError);
		if (iStatus == OI_OK)
		{
			*pResult = std::move(vResults.front());
		}
		return iStatus;
	});
}

OI::Coro::TaskResult<int32_t> Client::Transaction(std::vector<Statement> vStatements, std::vector<Result>& vResults, Error& stError) const
{
	auto pState = m_pState;
	return OI::Coro::RunBlocking(pState->iType, [pState, vStatements = std::move(vStatements), pResults = &vResults, pError = &stError]() -> int32_t
	{
		std::vector<Result> vLocalResults;
		const int32_t iStatus = pState->Run(vStatements, true, vLocalResults, *pError);
		if (iStatus == OI_OK)
		{
			*pResults = std::move(vLocalResults);
		}
		return iStatus;
	});
}
}
