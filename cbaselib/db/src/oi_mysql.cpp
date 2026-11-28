#include "oi_mysql.h"
#include "oi_db_driver.h"
#include "oi_errno.h"
#include "oi_lock.h"
#include <mysql.h>
#include <errmsg.h>
#include <mysqld_error.h>
#include <charconv>
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
	float fValue = 0;
	char vRealText[64] = {};
};

void SetError(OI::Db::Error& stError, const char* pStage, unsigned int iCode, const char* pState)
{
	stError.sStage = pStage;
	stError.iNativeCode = iCode;
	stError.sSqlState = pState ? std::string(pState).substr(0, 5) : "";
	// Server and SDK error strings can contain SQL, values, host names or credentials.
	stError.sMessage = std::string("MySQL ") + pStage + " failed";
}

int32_t SqlStatus(const OI::Db::Error& stError)
{
	switch (stError.iNativeCode)
	{
	case ER_DUP_ENTRY:
		return OI_ERR_EXIST;
	case ER_LOCK_WAIT_TIMEOUT:
		return OI_ERR_TIMEOUT;
	case ER_LOCK_DEADLOCK:
		return OI_ERR_BUSY;
	case CR_SERVER_GONE_ERROR:
	case CR_SERVER_LOST:
		return OI_ERR_CONNECT;
	default:
		if (stError.sSqlState.starts_with("08"))
		{
			return OI_ERR_CONNECT;
		}
		return OI_ERR_DB;
	}
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
		stError.sDatabase = m_stConfig.sDatabase;
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
	int32_t Execute(const OI::Db::BoundSql& stBoundSql, OI::Db::Result& stResult, OI::Db::Error& stError) override
	{
		stError.sDatabase = m_stConfig.sDatabase;
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
			return SqlStatus(*pError);
		};
		if (stBoundSql.sSql.size() > std::numeric_limits<unsigned long>::max())
		{
			SetError(stError, "prepare", 0, nullptr);
			return OI_ERR_INVALID_ARGUMENT;
		}
		if (mysql_stmt_prepare(stGuard.pStatement, stBoundSql.sSql.data(), static_cast<unsigned long>(stBoundSql.sSql.size())) != 0)
		{
			return fnFail("prepare");
		}
		if (mysql_stmt_param_count(stGuard.pStatement) != stBoundSql.vParameters.size())
		{
			SetError(stError, "bind", 0, nullptr);
			return OI_ERR_INVALID_ARGUMENT;
		}
		std::vector<MYSQL_BIND> vBind(stBoundSql.vParameters.size());
		std::vector<unsigned long> vLengths(stBoundSql.vParameters.size());
		unsigned char iEmptyByte = 0;
		for (size_t iIndex = 0; iIndex < vBind.size(); ++iIndex)
		{
			const auto& stValue = stBoundSql.vParameters[iIndex];
			auto& stBind = vBind[iIndex];
			if (stValue.is_null())
			{
				stBind.buffer_type = MYSQL_TYPE_NULL;
			}
			else if (stValue.is_signed() || stValue.is_bool())
			{
				stBind.buffer_type = MYSQL_TYPE_LONGLONG;
				stBind.buffer = const_cast<int64_t*>(&stValue.iSigned);
			}
			else if (stValue.is_unsigned())
			{
				stBind.buffer_type = MYSQL_TYPE_LONGLONG;
				stBind.is_unsigned = 1;
				stBind.buffer = const_cast<uint64_t*>(&stValue.iUnsigned);
			}
			else if (stValue.is_real())
			{
				stBind.buffer_type = MYSQL_TYPE_DOUBLE;
				stBind.buffer = const_cast<double*>(&stValue.dReal);
			}
			else
			{
				const size_t iSize = stValue.sBytes.size();
				if (iSize > std::numeric_limits<unsigned long>::max())
				{
					SetError(stError, "bind", 0, nullptr);
					return OI_ERR_INVALID_ARGUMENT;
				}
				vLengths[iIndex] = static_cast<unsigned long>(iSize);
				stBind.buffer_type = stValue.is_text() ? MYSQL_TYPE_STRING : MYSQL_TYPE_BLOB;
				if (iSize == 0)
				{
					stBind.buffer = static_cast<void*>(&iEmptyByte);
				}
				else
				{
					stBind.buffer = const_cast<char*>(stValue.sBytes.data());
				}
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
			// 库表来源只存一份；字段元数据和每行的 map 节点都计入结果预算。
			constexpr size_t iNodeOverhead = 4 * sizeof(void*);
			constexpr size_t iColumnBytes = sizeof(ResultBind) + sizeof(MYSQL_BIND) + sizeof(decltype(OI::Db::QuerySource::mColumns)::value_type) + iNodeOverhead;
			if (!fnCharge(sizeof(OI::Db::QuerySource)) || !fnCharge(m_stConfig.sDatabase.size()) ||
			    iCount > m_stConfig.iMaxResultBytes / iColumnBytes || !fnCharge(static_cast<size_t>(iCount) * iColumnBytes))
			{
				m_bHealthy = false;
				SetError(stError, "result_limit", 0, nullptr);
				return OI_ERR_FAIL;
			}
			auto pSource = std::make_shared<OI::Db::QuerySource>();
			pSource->sDatabase = m_stConfig.sDatabase;
			std::vector<ResultBind> vFields(iCount);
			std::vector<MYSQL_BIND> vOutput(iCount);
			for (size_t iIndex = 0; iIndex < iCount; ++iIndex)
			{
				const MYSQL_FIELD& stField = pFields[iIndex];
				if (!fnCharge(stField.name_length) || !fnCharge(stField.db_length) || !fnCharge(stField.org_table_length))
				{
					m_bHealthy = false;
					SetError(stError, "result_limit", 0, nullptr);
					return OI_ERR_FAIL;
				}
				OI::Db::TableSource stOrigin;
				if (stField.db_length)
				{
					stOrigin.sDatabase.assign(stField.db, stField.db_length);
				}
				if (stField.org_table_length)
				{
					stOrigin.sTable.assign(stField.org_table, stField.org_table_length);
				}
				if (!pSource->mColumns.emplace(std::string(stField.name, stField.name_length), std::move(stOrigin)).second)
				{
					SetError(stError, "duplicate_column", 0, nullptr);
					m_bHealthy = false;
					return OI_ERR_DECODE;
				}
				auto& stBind = vOutput[iIndex];
				auto& stValue = vFields[iIndex];
				stBind.buffer_type = MYSQL_TYPE_BLOB;
				if (stField.type == MYSQL_TYPE_FLOAT)
				{
					stBind.buffer_type = MYSQL_TYPE_FLOAT;
					stBind.buffer = &stValue.fValue;
					stBind.buffer_length = sizeof(stValue.fValue);
				}
				else if (stField.type == MYSQL_TYPE_DOUBLE)
				{
					// SDK 转文本时按缓冲长度决定位数，DOUBLE 直接一次取完。
					stBind.buffer = stValue.vRealText;
					stBind.buffer_length = sizeof(stValue.vRealText);
				}
				else
				{
					stBind.buffer = &stValue.iByte;
					stBind.buffer_length = sizeof(stValue.iByte);
				}
				stBind.length = &stValue.iLength;
				stBind.is_null = &stValue.bNull;
				stBind.error = &stValue.bError;
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
				    !fnCharge(sizeof(OI::Db::Row)))
				{
					m_bHealthy = false;
					SetError(stError, "result_limit", 0, nullptr);
					return OI_ERR_FAIL;
				}
				OI::Db::CellMap mRow;
				for (size_t iIndex = 0; iIndex < iCount; ++iIndex)
				{
					if (!fnCharge(sizeof(OI::Db::CellMap::value_type) + iNodeOverhead) || !fnCharge(pFields[iIndex].name_length))
					{
						m_bHealthy = false;
						SetError(stError, "result_limit", 0, nullptr);
						return OI_ERR_FAIL;
					}
					auto& stCell = mRow[std::string(pFields[iIndex].name, pFields[iIndex].name_length)];
					auto& stValue = vFields[iIndex];
					if (stValue.bNull)
					{
						continue;
					}
					const auto iType = pFields[iIndex].type;
					size_t iLength = stValue.iLength;
					if (iType == MYSQL_TYPE_FLOAT)
					{
						// 按精确提升后的值转文本，float 和 double 两种读取都能得到列的原值。
						char* pBegin = stValue.vRealText;
						const auto stText = std::to_chars(pBegin, pBegin + sizeof(stValue.vRealText), static_cast<double>(stValue.fValue), std::chars_format::general);
						if (stValue.bError || stText.ec != std::errc())
						{
							return fnFail("fetch_real");
						}
						iLength = static_cast<size_t>(stText.ptr - pBegin);
					}
					if (!fnCharge(iLength))
					{
						m_bHealthy = false;
						SetError(stError, "result_limit", 0, nullptr);
						return OI_ERR_FAIL;
					}
					if (iType == MYSQL_TYPE_FLOAT || iType == MYSQL_TYPE_DOUBLE)
					{
						if (iLength > sizeof(stValue.vRealText))
						{
							return fnFail("fetch_real");
						}
						stCell.emplace(stValue.vRealText, iLength);
						continue;
					}
					stCell.emplace(iLength, '\0');
					if (iLength > 0)
					{
						MYSQL_BIND stColumn{};
						stColumn.buffer_type = MYSQL_TYPE_BLOB;
						stColumn.buffer = stCell->data();
						stColumn.buffer_length = static_cast<unsigned long>(iLength);
						if (mysql_stmt_fetch_column(stGuard.pStatement, &stColumn, static_cast<unsigned int>(iIndex), 0) != 0)
						{
							return fnFail("fetch_column");
						}
					}
				}
				stOutput.vRows.push_back({std::move(mRow), pSource});
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
	// MySQL 不支持用预处理协议发 BEGIN（原生错误 1295，8.0 和 8.4 实测），事务控制统一走文本协议。
	int32_t ExecuteControl(std::string_view sSql, OI::Db::Error& stError) override
	{
		stError.sDatabase = m_stConfig.sDatabase;
		auto& stThread = GetThreadState(m_pRuntime);
		if (!stThread.bReady)
		{
			m_bHealthy = false;
			SetError(stError, "thread_init", 0, nullptr);
			return OI_ERR_FAIL;
		}
		m_pRuntime->Drain();
		auto fnFail = [this, pError = &stError](const char* pStage) -> int32_t
		{
			SetError(*pError, pStage, mysql_errno(m_pMysql), mysql_sqlstate(m_pMysql));
			// Discard on any SDK failure; never retry an operation with uncertain outcome.
			m_bHealthy = false;
			return SqlStatus(*pError);
		};
		if (sSql.empty() || sSql.size() > std::numeric_limits<unsigned long>::max())
		{
			SetError(stError, "control", 0, nullptr);
			return OI_ERR_INVALID_ARGUMENT;
		}
		if (mysql_real_query(m_pMysql, sSql.data(), static_cast<unsigned long>(sSql.size())) != 0)
		{
			return fnFail("control");
		}
		// 事务控制只回 OK 包。文本协议下要把结果取干净，否则这条连接的下一条语句会报命令不同步。
		for (;;)
		{
			MYSQL_RES* pResult = mysql_store_result(m_pMysql);
			if (pResult)
			{
				// 事务控制不该带结果集；带了说明这条语句不是我们以为的那种，丢弃连接。
				mysql_free_result(pResult);
				m_bHealthy = false;
				SetError(stError, "control_result", 0, nullptr);
				return OI_ERR_FAIL;
			}
			else if (mysql_errno(m_pMysql) != 0 || mysql_field_count(m_pMysql) != 0)
			{
				return fnFail("control_result");
			}
			const int iNext = mysql_next_result(m_pMysql);
			if (iNext == -1)
			{
				break;
			}
			if (iNext != 0)
			{
				return fnFail("control_result");
			}
		}
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
		stError.sDatabase = m_stConfig.sDatabase;
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
