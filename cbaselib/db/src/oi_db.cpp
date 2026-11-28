#include "oi_db_row.h"
#include "oi_db_driver.h"
#include "oi_errno.h"
#include "oi_lock.h"
#include <span>
#include <set>
#include <stdexcept>
#include <utility>

namespace OI::Db
{
namespace
{
std::string IdentifierLabel(std::string_view sName)
{
	if (sName.empty() || sName.size() > 256)
	{
		return "<unknown>";
	}
	for (const unsigned char iByte : sName)
	{
		if (iByte < 128 && !(iByte >= 'a' && iByte <= 'z') && !(iByte >= 'A' && iByte <= 'Z') &&
		    !(iByte >= '0' && iByte <= '9') && iByte != '_' && iByte != '-' && iByte != '$')
		{
			return "<unknown>";
		}
	}
	return std::string(sName);
}
}

std::string QuerySource::Describe(std::string_view sColumn) const
{
	const auto it = mColumns.find(sColumn);
	if (it != mColumns.end())
	{
		const auto& stTable = it->second;
		const auto& sSchema = stTable.sDatabase.empty() ? sDatabase : stTable.sDatabase;
		return "database=" + IdentifierLabel(sSchema) + " table=" + IdentifierLabel(stTable.sTable);
	}
	// 缺列没有唯一来源。列出查询结果中实际出现的表，便于排查联表别名。
	std::set<std::string> vTables;
	for (const auto& [sName, stTable] : mColumns)
	{
		if (!stTable.sTable.empty())
		{
			std::string sOrigin = IdentifierLabel(stTable.sTable);
			if (!stTable.sDatabase.empty() && stTable.sDatabase != sDatabase)
			{
				sOrigin = IdentifierLabel(stTable.sDatabase) + "." + sOrigin;
			}
			vTables.insert(std::move(sOrigin));
		}
	}
	std::string sTables;
	for (const auto& sName : vTables)
	{
		if (!sTables.empty())
		{
			sTables += ",";
		}
		sTables += sName;
	}
	if (sTables.empty())
	{
		sTables = "<unknown>";
	}
	return "database=" + IdentifierLabel(sDatabase) + " table=" + sTables;
}

RowDecodeError::RowDecodeError(const QuerySource* pSource, std::string_view sColumn, const char* pReason)
    : SqlError(OI_ERR_DECODE, (pSource ? pSource->Describe(sColumn) : "database=<unknown> table=<unknown>") + " column=" + IdentifierLabel(sColumn) + " " + pReason)
{
}

std::optional<Cell> Row::find(std::string_view sColumn) const&
{
	const auto it = mCells.find(sColumn);
	if (it == mCells.end())
	{
		return std::nullopt;
	}
	return Cell(pSource.get(), it->first, it->second);
}

Cell Row::at(std::string_view sColumn) const&
{
	const auto stCell = find(sColumn);
	if (!stCell)
	{
		throw RowDecodeError(pSource.get(), sColumn, "missing");
	}
	return *stCell;
}

bool Cell::is_null() const
{
	return !m_stValue.has_value();
}

bool Cell::is_string() const
{
	return m_stValue.has_value();
}

const std::string& Cell::as_string() const
{
	if (!m_stValue)
	{
		throw RowDecodeError(m_pSource, m_sColumn, "is NULL");
	}
	return *m_stValue;
}

template <typename T>
T Cell::Number(const char* pExpected) const
{
	T stValue{};
	if (!DecodeDbValue(as_string(), stValue))
	{
		throw RowDecodeError(m_pSource, m_sColumn, pExpected);
	}
	return stValue;
}

template <typename T>
bool Cell::CanReadNumber() const
{
	T stValue{};
	return m_stValue && DecodeDbValue(*m_stValue, stValue);
}

bool Cell::is_bool() const
{
	return CanReadNumber<bool>();
}

bool Cell::as_bool() const
{
	return Number<bool>("invalid bool");
}

bool Cell::is_int8() const
{
	return CanReadNumber<int8_t>();
}

int8_t Cell::as_int8() const
{
	return Number<int8_t>("invalid int8");
}

bool Cell::is_uint8() const
{
	return CanReadNumber<uint8_t>();
}

uint8_t Cell::as_uint8() const
{
	return Number<uint8_t>("invalid uint8");
}

bool Cell::is_int16() const
{
	return CanReadNumber<int16_t>();
}

int16_t Cell::as_int16() const
{
	return Number<int16_t>("invalid int16");
}

bool Cell::is_uint16() const
{
	return CanReadNumber<uint16_t>();
}

uint16_t Cell::as_uint16() const
{
	return Number<uint16_t>("invalid uint16");
}

bool Cell::is_int32() const
{
	return CanReadNumber<int32_t>();
}

int32_t Cell::as_int32() const
{
	return Number<int32_t>("invalid int32");
}

bool Cell::is_uint32() const
{
	return CanReadNumber<uint32_t>();
}

uint32_t Cell::as_uint32() const
{
	return Number<uint32_t>("invalid uint32");
}

bool Cell::is_int64() const
{
	return CanReadNumber<int64_t>();
}

int64_t Cell::as_int64() const
{
	return Number<int64_t>("invalid int64");
}

bool Cell::is_uint64() const
{
	return CanReadNumber<uint64_t>();
}

uint64_t Cell::as_uint64() const
{
	return Number<uint64_t>("invalid uint64");
}

bool Cell::is_float() const
{
	return CanReadNumber<float>();
}

float Cell::as_float() const
{
	return Number<float>("invalid float");
}

bool Cell::is_double() const
{
	return CanReadNumber<double>();
}

double Cell::as_double() const
{
	return Number<double>("invalid double");
}

void Error::ThrowIfFailed(int32_t iCode) const
{
	if (iCode == OI_OK)
	{
		return;
	}
	std::string sDescription = "database=" + IdentifierLabel(sDatabase) + " stage=" + IdentifierLabel(sStage) + " native=" + std::to_string(iNativeCode) + " sqlstate=" + IdentifierLabel(sSqlState);
	if (sStage == "decode_row" || sStage == "affected_rows")
	{
		sDescription += " " + sMessage;
	}
	if (stCleanup.bFailed)
	{
		sDescription += " cleanup=" + IdentifierLabel(stCleanup.sStage) + " native=" + std::to_string(stCleanup.iNativeCode);
	}
	throw SqlError(iCode, sDescription);
}

Batch::Batch() = default;
Batch::~Batch() = default;
Batch::Batch(Batch&&) noexcept = default;
Batch& Batch::operator=(Batch&&) noexcept = default;
Batch::Batch(const Batch&) = default;
Batch& Batch::operator=(const Batch&) = default;

void Batch::Add(std::string sSql, std::vector<Parameter> vParameters)
{
	m_vCommands.push_back({std::move(sSql), std::move(vParameters)});
}

void Batch::AddExpectAffected(uint64_t iExpected, std::string sSql, std::vector<Parameter> vParameters)
{
	m_vCommands.push_back({std::move(sSql), std::move(vParameters), iExpected});
}

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

	int32_t Run(std::span<const BoundSql> vStatements, bool bTransaction, std::vector<Result>& vResults, Error& stError)
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
		if (bTransaction)
		{
			const int32_t iStatus = pConnection->ExecuteControl("BEGIN", stError);
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
						stConnection.ExecuteControl("ROLLBACK", stIgnoredError);
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
				if (pConnectionLocal->ExecuteControl("ROLLBACK", stRollbackError) == OI_OK)
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
		for (const auto& stBoundSql : vStatements)
		{
			Result stResult;
			int32_t iStatus = pConnection->Execute(stBoundSql, stResult, stError);
			if (iStatus == OI_OK && stBoundSql.stExpectedAffected && stResult.iAffected != *stBoundSql.stExpectedAffected)
			{
				stError.sStage = "affected_rows";
				stError.sMessage = "statement=" + std::to_string(vResults.size() + 1) + " expected=" + std::to_string(*stBoundSql.stExpectedAffected) +
				                   " actual=" + std::to_string(stResult.iAffected);
				iStatus = OI_ERR_DB_AFFECTED_ROWS;
			}
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
			const int32_t iStatus = pConnection->ExecuteControl("COMMIT", stError);
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

OI::Coro::TaskResult<int32_t> Client::Call(std::string sSql, std::vector<Parameter> vParameters, Result& stResult, Error& stError) const
{
	stError = {};
	auto pState = m_pState;
	return OI::Coro::RunBlocking(pState->iType, [pState, stBoundSql = BoundSql{std::move(sSql), std::move(vParameters)}, pResult = &stResult, pError = &stError]() -> int32_t
	{
		std::vector<Result> vResults;
		const int32_t iStatus = pState->Run(std::span(&stBoundSql, 1), false, vResults, *pError);
		if (iStatus == OI_OK)
		{
			*pResult = std::move(vResults.front());
		}
		return iStatus;
	});
}

OI::Coro::TaskResult<int32_t> Client::Transaction(Batch stBatch, std::vector<Result>& vResults, Error& stError) const
{
	stError = {};
	auto pState = m_pState;
	return OI::Coro::RunBlocking(pState->iType, [pState, vStatements = std::move(stBatch.m_vCommands), pResults = &vResults, pError = &stError]() -> int32_t
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

OI::Coro::TaskResult<int32_t> Client::BuildStatus(int32_t iCode, Error& stError)
{
	stError = {};
	if (iCode != OI_OK)
	{
		stError.sStage = "build_statement";
	}
	co_return iCode;
}

OI::Coro::TaskResult<int32_t> Client::ExecuteWrite(Client stClient, BoundSql stBoundSql, Error& stError)
{
	Result stResult;
	co_return co_await stClient.Call(std::move(stBoundSql.sSql), std::move(stBoundSql.vParameters), stResult, stError);
}

OI::Coro::TaskResult<int32_t> Client::ExecuteTableBatch(Client stClient, Batch stBatch, Error& stError)
{
	std::vector<Result> vResults;
	co_return co_await stClient.Transaction(std::move(stBatch), vResults, stError);
}
}
