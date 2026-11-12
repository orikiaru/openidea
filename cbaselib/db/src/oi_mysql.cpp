#include "oi_mysql.h"
#include "oi_errno.h"
#include "oi_lock.h"
#include <mysql.h>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace OI::Mysql
{
namespace
{
struct Runtime
{
	bool bReady = mysql_library_init(0, nullptr, nullptr) == 0;
	OI::Thread::NativeMutex stMutex;
	std::vector<MYSQL*> vDeferredClose;

	void Drain()
	{
		std::vector<MYSQL*> vConnections;
		{
			OI::Thread::NativeMutexGuard stGuard(stMutex);
			vConnections.swap(vDeferredClose);
		}
		for (auto* pConnection : vConnections)
		{
			mysql_close(pConnection);
		}
	}
	void Defer(MYSQL* pConnection) noexcept
	{
		try
		{
			OI::Thread::NativeMutexGuard stGuard(stMutex);
			vDeferredClose.push_back(pConnection);
		}
		catch (...)
		{
			std::fputs("MySQL: could not retain connection for deferred close\n", stderr);
		}
	}
	~Runtime()
	{
		if (bReady)
		{
			if (!vDeferredClose.empty())
			{
				if (mysql_thread_init() == 0)
				{
					Drain();
					mysql_thread_end();
				}
				else
				{
					std::fputs("MySQL: thread initialization failed during deferred close\n", stderr);
				}
			}
			mysql_library_end();
		}
	}
};

struct ThreadState
{
	std::shared_ptr<Runtime> pRuntime;
	bool bReady = false;
	~ThreadState()
	{
		if (bReady)
		{
			pRuntime->Drain();
			mysql_thread_end();
		}
	}
};

ThreadState& GetThreadState(const std::shared_ptr<Runtime>& pRuntime)
{
	// Keep the client library alive until this thread has released its TLS.
	thread_local ThreadState stThread;
	if (!stThread.bReady && mysql_thread_init() == 0)
	{
		stThread.pRuntime = pRuntime;
		stThread.bReady = true;
	}
	return stThread;
}

using BindFlag = std::remove_pointer_t<decltype(MYSQL_BIND{}.is_null)>;

struct ResultBind
{
	unsigned long iLength = 0;
	BindFlag bNull = 0;
	BindFlag bError = 0;
	unsigned char iByte = 0;
};

void SetError(OI::Db::Error& stError, const char* pStage, unsigned int iCode, const char* pState)
{
	stError.sStage = pStage;
	stError.iNativeCode = iCode;
	stError.sSqlState = pState ? std::string(pState).substr(0, 5) : "";
	// Server and SDK error strings can contain SQL, values, host names or credentials.
	stError.sMessage = std::string("MySQL ") + pStage + " failed";
}

class MysqlConnection final : public OI::Db::Connection
{
	std::shared_ptr<Runtime> m_pRuntime;
	MYSQL* m_pMysql;
	Config m_stConfig;
	bool m_bHealthy = true;

  public:
	MysqlConnection(std::shared_ptr<Runtime> pRuntime, MYSQL* pMysql, Config stConfig)
	    : m_pRuntime(std::move(pRuntime)), m_pMysql(pMysql), m_stConfig(std::move(stConfig))
	{
	}
	~MysqlConnection() override
	{
		auto& stThread = GetThreadState(m_pRuntime);
		if (stThread.bReady)
		{
			m_pRuntime->Drain();
			mysql_close(m_pMysql);
		}
		else
		{
			std::fputs("MySQL: thread initialization failed; connection close deferred\n", stderr);
			m_pRuntime->Defer(m_pMysql);
		}
	}
	bool Healthy() const override
	{
		return m_bHealthy;
	}
	bool Reset(OI::Db::Error& stError) override
	{
		auto& stThread = GetThreadState(m_pRuntime);
		if (!stThread.bReady)
		{
			m_bHealthy = false;
			SetError(stError, "thread_init", 0, nullptr);
			return false;
		}
		m_pRuntime->Drain();
		if (mysql_reset_connection(m_pMysql) != 0 || mysql_set_character_set(m_pMysql, "utf8mb4") != 0 ||
		    (!m_stConfig.sDatabase.empty() && mysql_select_db(m_pMysql, m_stConfig.sDatabase.c_str()) != 0))
		{
			m_bHealthy = false;
			SetError(stError, "reset", mysql_errno(m_pMysql), mysql_sqlstate(m_pMysql));
			return false;
		}
		return true;
	}
	int32_t Execute(const OI::Db::Statement& stStatement, OI::Db::Result& stResult, OI::Db::Error& stError) override
	{
		auto& stThread = GetThreadState(m_pRuntime);
		if (!stThread.bReady)
		{
			m_bHealthy = false;
			SetError(stError, "thread_init", 0, nullptr);
			return OI_ERR_FAIL;
		}
		m_pRuntime->Drain();
		struct StatementGuard
		{
			MYSQL_STMT* pStatement;
			~StatementGuard()
			{
				if (pStatement)
				{
					mysql_stmt_close(pStatement);
				}
			}
		} stGuard{mysql_stmt_init(m_pMysql)};
		if (!stGuard.pStatement)
		{
			m_bHealthy = false;
			SetError(stError, "statement_init", mysql_errno(m_pMysql), mysql_sqlstate(m_pMysql));
			return OI_ERR_FAIL;
		}
		auto fnFail = [this, pStatement = stGuard.pStatement, pError = &stError](const char* pStage) -> int32_t
		{
			SetError(*pError, pStage, mysql_stmt_errno(pStatement), mysql_stmt_sqlstate(pStatement));
			// Discard on any SDK failure; never retry an operation with uncertain outcome.
			m_bHealthy = false;
			return OI_ERR_FAIL;
		};
		if (stStatement.sSql.size() > std::numeric_limits<unsigned long>::max())
		{
			SetError(stError, "prepare", 0, nullptr);
			return OI_ERR_INVALID_ARGUMENT;
		}
		if (mysql_stmt_prepare(stGuard.pStatement, stStatement.sSql.data(), static_cast<unsigned long>(stStatement.sSql.size())) != 0)
		{
			return fnFail("prepare");
		}
		if (mysql_stmt_param_count(stGuard.pStatement) != stStatement.vParameters.size())
		{
			SetError(stError, "bind", 0, nullptr);
			return OI_ERR_INVALID_ARGUMENT;
		}
		std::vector<MYSQL_BIND> vBind(stStatement.vParameters.size());
		std::vector<unsigned long> vLengths(stStatement.vParameters.size());
		unsigned char iEmptyByte = 0;
		for (size_t iIndex = 0; iIndex < vBind.size(); ++iIndex)
		{
			const auto& stValue = stStatement.vParameters[iIndex];
			auto& stBind = vBind[iIndex];
			if (std::holds_alternative<std::nullptr_t>(stValue))
			{
				stBind.buffer_type = MYSQL_TYPE_NULL;
			}
			else if (const auto* pValue = std::get_if<int64_t>(&stValue))
			{
				stBind.buffer_type = MYSQL_TYPE_LONGLONG;
				stBind.buffer = const_cast<int64_t*>(pValue);
			}
			else if (const auto* pValue = std::get_if<uint64_t>(&stValue))
			{
				stBind.buffer_type = MYSQL_TYPE_LONGLONG;
				stBind.is_unsigned = 1;
				stBind.buffer = const_cast<uint64_t*>(pValue);
			}
			else if (const auto* pValue = std::get_if<double>(&stValue))
			{
				stBind.buffer_type = MYSQL_TYPE_DOUBLE;
				stBind.buffer = const_cast<double*>(pValue);
			}
			else
			{
				const auto* pString = std::get_if<std::string>(&stValue);
				const auto* pBlob = std::get_if<OI::Db::Blob>(&stValue);
				const size_t iSize = pString ? pString->size() : pBlob->size();
				if (iSize > std::numeric_limits<unsigned long>::max())
				{
					SetError(stError, "bind", 0, nullptr);
					return OI_ERR_INVALID_ARGUMENT;
				}
				vLengths[iIndex] = static_cast<unsigned long>(iSize);
				stBind.buffer_type = pString ? MYSQL_TYPE_STRING : MYSQL_TYPE_BLOB;
				stBind.buffer = iSize == 0 ? static_cast<void*>(&iEmptyByte) : (pString ? static_cast<void*>(const_cast<char*>(pString->data())) : static_cast<void*>(const_cast<uint8_t*>(pBlob->data())));
				stBind.buffer_length = vLengths[iIndex];
				stBind.length = &vLengths[iIndex];
			}
		}
		if (!vBind.empty() && mysql_stmt_bind_param(stGuard.pStatement, vBind.data()) != 0)
		{
			return fnFail("bind");
		}
		if (mysql_stmt_execute(stGuard.pStatement) != 0)
		{
			return fnFail("execute");
		}
		OI::Db::Result stOutput;
		stOutput.iAffected = mysql_stmt_affected_rows(stGuard.pStatement);
		if (stOutput.iAffected == static_cast<uint64_t>(-1))
		{
			stOutput.iAffected = 0;
		}
		stOutput.iInsertId = mysql_stmt_insert_id(stGuard.pStatement);
		struct MetadataGuard
		{
			MYSQL_RES* pResult;
			~MetadataGuard()
			{
				if (pResult)
				{
					mysql_free_result(pResult);
				}
			}
		} stMetadata{mysql_stmt_result_metadata(stGuard.pStatement)};
		if (stMetadata.pResult)
		{
			const unsigned int iCount = mysql_num_fields(stMetadata.pResult);
			MYSQL_FIELD* pFields = mysql_fetch_fields(stMetadata.pResult);
			size_t iBytes = 0;
			auto fnCharge = [this, pBytes = &iBytes](size_t iSize) -> bool
			{
				if (iSize > m_stConfig.iMaxResultBytes - *pBytes)
				{
					return false;
				}
				*pBytes += iSize;
				return true;
			};
			if (iCount > m_stConfig.iMaxResultBytes / (sizeof(std::string) + sizeof(ResultBind) + sizeof(MYSQL_BIND)) ||
			    !fnCharge(static_cast<size_t>(iCount) * (sizeof(std::string) + sizeof(ResultBind) + sizeof(MYSQL_BIND))))
			{
				m_bHealthy = false;
				SetError(stError, "result_limit", 0, nullptr);
				return OI_ERR_FAIL;
			}
			std::vector<ResultBind> vFields(iCount);
			std::vector<MYSQL_BIND> vOutput(iCount);
			for (size_t iIndex = 0; iIndex < iCount; ++iIndex)
			{
				if (!fnCharge(pFields[iIndex].name_length))
				{
					m_bHealthy = false;
					SetError(stError, "result_limit", 0, nullptr);
					return OI_ERR_FAIL;
				}
				stOutput.vColumns.emplace_back(pFields[iIndex].name, pFields[iIndex].name_length);
				vOutput[iIndex].buffer_type = MYSQL_TYPE_BLOB;
				vOutput[iIndex].buffer = &vFields[iIndex].iByte;
				vOutput[iIndex].buffer_length = 1;
				vOutput[iIndex].length = &vFields[iIndex].iLength;
				vOutput[iIndex].is_null = &vFields[iIndex].bNull;
				vOutput[iIndex].error = &vFields[iIndex].bError;
			}
			if (mysql_stmt_bind_result(stGuard.pStatement, vOutput.data()) != 0)
			{
				return fnFail("bind_result");
			}
			for (;;)
			{
				const int iFetch = mysql_stmt_fetch(stGuard.pStatement);
				if (iFetch == MYSQL_NO_DATA)
				{
					break;
				}
				if (iFetch != 0 && iFetch != MYSQL_DATA_TRUNCATED)
				{
					return fnFail("fetch");
				}
				if (stOutput.vRows.size() >= m_stConfig.iMaxRows ||
				    iCount > m_stConfig.iMaxResultBytes / sizeof(std::optional<OI::Db::Blob>) ||
				    !fnCharge(sizeof(std::vector<std::optional<OI::Db::Blob>>)) ||
				    !fnCharge(static_cast<size_t>(iCount) * sizeof(std::optional<OI::Db::Blob>)))
				{
					m_bHealthy = false;
					SetError(stError, "result_limit", 0, nullptr);
					return OI_ERR_FAIL;
				}
				std::vector<std::optional<OI::Db::Blob>> vRow;
				vRow.reserve(iCount);
				for (size_t iIndex = 0; iIndex < iCount; ++iIndex)
				{
					if (vFields[iIndex].bNull)
					{
						vRow.emplace_back(std::nullopt);
						continue;
					}
					const size_t iLength = vFields[iIndex].iLength;
					if (!fnCharge(iLength))
					{
						m_bHealthy = false;
						SetError(stError, "result_limit", 0, nullptr);
						return OI_ERR_FAIL;
					}
					vRow.emplace_back(OI::Db::Blob(iLength));
					if (iLength > 0)
					{
						MYSQL_BIND stColumn{};
						stColumn.buffer_type = MYSQL_TYPE_BLOB;
						stColumn.buffer = vRow.back()->data();
						stColumn.buffer_length = static_cast<unsigned long>(iLength);
						if (mysql_stmt_fetch_column(stGuard.pStatement, &stColumn, static_cast<unsigned int>(iIndex), 0) != 0)
						{
							return fnFail("fetch_column");
						}
					}
				}
				stOutput.vRows.push_back(std::move(vRow));
			}
		}
		else if (mysql_stmt_field_count(stGuard.pStatement) != 0)
		{
			return fnFail("metadata");
		}
		const int iNext = mysql_stmt_next_result(stGuard.pStatement);
		if (iNext != -1)
		{
			m_bHealthy = false;
			SetError(stError, "multiple_results", mysql_stmt_errno(stGuard.pStatement), mysql_stmt_sqlstate(stGuard.pStatement));
			return OI_ERR_FAIL;
		}
		stResult = std::move(stOutput);
		return OI_OK;
	}
};

class MysqlDriver final : public OI::Db::Driver
{
	std::shared_ptr<Runtime> m_pRuntime;
	Config m_stConfig;

  public:
	MysqlDriver(std::shared_ptr<Runtime> pRuntime, Config stConfig)
	    : m_pRuntime(std::move(pRuntime)), m_stConfig(std::move(stConfig))
	{
	}
	std::unique_ptr<OI::Db::Connection> Connect(OI::Db::Error& stError) override
	{
		if (m_stConfig.iConnectTimeoutSeconds == 0 || m_stConfig.iReadTimeoutSeconds == 0 || m_stConfig.iWriteTimeoutSeconds == 0)
		{
			SetError(stError, "invalid_timeout", 0, nullptr);
			return nullptr;
		}
		if (!m_pRuntime->bReady)
		{
			SetError(stError, "init", 0, nullptr);
			return nullptr;
		}
		auto& stThread = GetThreadState(m_pRuntime);
		if (!stThread.bReady)
		{
			SetError(stError, "thread_init", 0, nullptr);
			return nullptr;
		}
		m_pRuntime->Drain();
		MYSQL* pMysql = mysql_init(nullptr);
		if (!pMysql)
		{
			SetError(stError, "init", 0, nullptr);
			return nullptr;
		}
		if (mysql_options(pMysql, MYSQL_OPT_CONNECT_TIMEOUT, &m_stConfig.iConnectTimeoutSeconds) != 0 ||
		    mysql_options(pMysql, MYSQL_OPT_READ_TIMEOUT, &m_stConfig.iReadTimeoutSeconds) != 0 ||
		    mysql_options(pMysql, MYSQL_OPT_WRITE_TIMEOUT, &m_stConfig.iWriteTimeoutSeconds) != 0 ||
		    mysql_options(pMysql, MYSQL_SET_CHARSET_NAME, "utf8mb4") != 0)
		{
			SetError(stError, "options", mysql_errno(pMysql), mysql_sqlstate(pMysql));
			mysql_close(pMysql);
			return nullptr;
		}
		if (!m_stConfig.sTlsCaFile.empty())
		{
			bool bOptionsFailed = mysql_options(pMysql, MYSQL_OPT_SSL_CA, m_stConfig.sTlsCaFile.c_str()) != 0;
#if defined(MARIADB_BASE_VERSION) || defined(MARIADB_VERSION_ID)
			BindFlag bEnable = 1;
			bOptionsFailed = bOptionsFailed || mysql_options(pMysql, MYSQL_OPT_SSL_ENFORCE, &bEnable) != 0 ||
			                 mysql_options(pMysql, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &bEnable) != 0;
#elif MYSQL_VERSION_ID >= 50711
			enum mysql_ssl_mode iMode = SSL_MODE_VERIFY_IDENTITY;
			bOptionsFailed = bOptionsFailed || mysql_options(pMysql, MYSQL_OPT_SSL_MODE, &iMode) != 0;
#else
			// Old clients cannot guarantee both encryption and identity verification.
			bOptionsFailed = true;
#endif
			if (bOptionsFailed)
			{
				SetError(stError, "tls_options", mysql_errno(pMysql), mysql_sqlstate(pMysql));
				mysql_close(pMysql);
				return nullptr;
			}
		}
		if (!mysql_real_connect(pMysql, m_stConfig.sHost.c_str(), m_stConfig.sUser.c_str(), m_stConfig.sPassword.c_str(), m_stConfig.sDatabase.c_str(), m_stConfig.iPort, nullptr, 0))
		{
			SetError(stError, "connect", mysql_errno(pMysql), mysql_sqlstate(pMysql));
			mysql_close(pMysql);
			return nullptr;
		}
		try
		{
			return std::make_unique<MysqlConnection>(m_pRuntime, pMysql, m_stConfig);
		}
		catch (...)
		{
			mysql_close(pMysql);
			throw;
		}
	}
};
}

std::shared_ptr<OI::Db::Driver> MakeDriver(Config stConfig)
{
	if (stConfig.sDatabase.empty() || stConfig.iPort == 0 || stConfig.iPort > 65535 ||
	    stConfig.iConnectTimeoutSeconds == 0 || stConfig.iReadTimeoutSeconds == 0 || stConfig.iWriteTimeoutSeconds == 0)
	{
		throw std::invalid_argument("MySQL requires a database, valid port and positive SDK timeouts");
	}
	static const auto pRuntime = std::make_shared<Runtime>();
	if (pRuntime->bReady)
	{
		// library_init also initializes TLS on its calling thread.
		(void)GetThreadState(pRuntime);
	}
	return std::make_shared<MysqlDriver>(pRuntime, std::move(stConfig));
}
}
