#pragma once
#include "oi_coro.h"
#include "oi_errno.h"
#include "oi_log.h"
#include "oi_blocking_pool.h"
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <concepts>
#include <utility>
#include <vector>

namespace OI::Db
{
using Blob = std::vector<uint8_t>;
enum class ParameterType
{
	Null,
	Boolean,
	Signed,
	Unsigned,
	Real,
	Text,
	Binary
};

// 数字保留原生绑定类型。文本和二进制共用有长度的字节串。
struct Parameter
{
	ParameterType iType = ParameterType::Null;
	int64_t iSigned = 0;
	uint64_t iUnsigned = 0;
	double dReal = 0;
	std::string sBytes;

	Parameter() = default;
	Parameter(std::nullptr_t)
	{
	}
	Parameter(bool bValue)
	    : iType(ParameterType::Boolean), iSigned(bValue)
	{
	}
	template <std::signed_integral T>
	Parameter(T iValue)
	    : iType(ParameterType::Signed), iSigned(iValue)
	{
	}
	template <std::unsigned_integral T>
	Parameter(T iValue)
	    : iType(ParameterType::Unsigned), iUnsigned(iValue)
	{
	}
	Parameter(double dValue)
	    : iType(ParameterType::Real), dReal(dValue)
	{
	}
	Parameter(std::string sValue)
	    : iType(ParameterType::Text), sBytes(std::move(sValue))
	{
	}
	Parameter(const char* pText)
	{
		if (pText)
		{
			iType = ParameterType::Text;
			sBytes = pText;
		}
	}
	Parameter(const Blob& vValue)
	    : iType(ParameterType::Binary), sBytes(vValue.begin(), vValue.end())
	{
	}
	static Parameter Binary(std::string sBytes)
	{
		Parameter stValue(std::move(sBytes));
		stValue.iType = ParameterType::Binary;
		return stValue;
	}
	bool is_bool() const
	{
		return iType == ParameterType::Boolean;
	}
	bool is_null() const
	{
		return iType == ParameterType::Null;
	}
	bool is_signed() const
	{
		return iType == ParameterType::Signed;
	}
	bool is_unsigned() const
	{
		return iType == ParameterType::Unsigned;
	}
	bool is_real() const
	{
		return iType == ParameterType::Real;
	}
	bool is_text() const
	{
		return iType == ParameterType::Text;
	}
	bool is_binary() const
	{
		return iType == ParameterType::Binary;
	}
	bool operator==(const Parameter&) const = default;
};

using CellMap = std::map<std::string, std::optional<std::string>, std::less<>>;

struct TableSource
{
	std::string sDatabase;
	std::string sTable;
};

// 驱动为一份结果填写一次。键是 SELECT 返回的列名，可对应不同来源表。
struct QuerySource
{
	std::string sDatabase;
	std::map<std::string, TableSource, std::less<>> mColumns;
	std::string Describe(std::string_view sColumn) const;
};

class SqlError : public std::runtime_error
{
  public:
	const int32_t iCode;
	SqlError(int32_t iCodeLocal, const std::string& sMessage)
	    : std::runtime_error(sMessage), iCode(iCodeLocal)
	{
	}
};

class RowDecodeError : public SqlError
{
  public:
	RowDecodeError(const QuerySource* pSource, std::string_view sColumn, const char* pReason);
};

// 仅借用当前行，读取到的字符串引用与所属行同寿命。
class Cell
{
	const QuerySource* m_pSource;
	std::string_view m_sColumn;
	const std::optional<std::string>& m_stValue;
	template <typename T>
	T Number(const char* pExpected) const;
	template <typename T>
	bool CanReadNumber() const;

  public:
	Cell(const QuerySource* pSource, std::string_view sColumn, const std::optional<std::string>& stValue)
	    : m_pSource(pSource), m_sColumn(sColumn), m_stValue(stValue)
	{
	}
	bool is_null() const;
	bool is_string() const;
	const std::string& as_string() const;
	bool is_bool() const;
	bool as_bool() const;
	bool is_int8() const;
	int8_t as_int8() const;
	bool is_uint8() const;
	uint8_t as_uint8() const;
	bool is_int16() const;
	int16_t as_int16() const;
	bool is_uint16() const;
	uint16_t as_uint16() const;
	bool is_int32() const;
	int32_t as_int32() const;
	bool is_uint32() const;
	uint32_t as_uint32() const;
	bool is_int64() const;
	int64_t as_int64() const;
	bool is_uint64() const;
	uint64_t as_uint64() const;
	bool is_float() const;
	float as_float() const;
	bool is_double() const;
	double as_double() const;
};

struct Row
{
	CellMap mCells;
	std::shared_ptr<const QuerySource> pSource{};
	std::optional<Cell> find(std::string_view sColumn) const&;
	std::optional<Cell> find(std::string_view sColumn) const&& = delete;
	Cell at(std::string_view sColumn) const&;
	Cell at(std::string_view sColumn) const&& = delete;
	bool operator==(const Row& stOther) const
	{
		return mCells == stOther.mCells;
	}
};

struct BoundSql;
class Driver;

class Batch
{
	friend class Client;
	std::vector<BoundSql> m_vCommands;

  public:
	Batch();
	~Batch();
	Batch(Batch&&) noexcept;
	Batch& operator=(Batch&&) noexcept;
	Batch(const Batch&);
	Batch& operator=(const Batch&);
	void Add(std::string sSql, std::vector<Parameter> vParameters = {});
};

struct CleanupError
{
	bool bFailed = false;
	bool bException = false;
	std::string sStage;
	uint32_t iNativeCode = 0;
	std::string sSqlState;
	std::string sMessage;
};

struct Error
{
	std::string sDatabase;
	std::string sStage;
	uint32_t iNativeCode = 0;
	std::string sSqlState;
	std::string sMessage;
	CleanupError stCleanup;
	void ThrowIfFailed(int32_t iCode) const;
};

struct Result
{
	std::vector<Row> vRows;
	uint64_t iAffected = 0;
	uint64_t iInsertId = 0;

	// 解码回调逐行返回一个对象。全部成功才替换输出，字段错误在这里统一处理。
	template <typename C, typename F>
	int32_t DecodeRows(std::vector<C>& vOutput, Error& stError, F fnDecode) const
	{
		stError.sStage.clear();
		stError.iNativeCode = 0;
		stError.sSqlState.clear();
		stError.sMessage.clear();
		std::vector<C> vDecoded;
		vDecoded.reserve(vRows.size());
		try
		{
			for (const Row& stRow : vRows)
			{
				vDecoded.push_back(fnDecode(stRow));
			}
		}
		catch (const RowDecodeError& stException)
		{
			stError.sStage = "decode_row";
			stError.sMessage = stException.what();
			return OI_ERR_DECODE;
		}
		vOutput = std::move(vDecoded);
		return OI_OK;
	}
};

class Client
{
	struct State;
	std::shared_ptr<State> m_pState;

	template <typename C, typename F, typename... M>
	OI::Coro::TaskResult<int32_t> WriteTable(const std::string& sTable, const C& stRow, Result* pResult, Error& stError, F fnBuild, M... pMembers) const;
	template <typename C, typename F, typename... M>
	OI::Coro::TaskResult<int32_t> WriteTable(const std::string& sTable, const std::vector<C>& vRows, Error& stError, F fnBuild, M... pMembers) const;
	template <typename C>
	static int32_t ReadByKey(const Result& stResult, C& stRow);
	template <typename C>
	static OI::Coro::TaskResult<int32_t> SelectAllRows(Client stClient, std::string sTable, std::vector<C>& vRows, Error& stError);
	template <typename C, typename... M>
	static OI::Coro::TaskResult<int32_t> SelectByKey(Client stClient, std::string sTable, C& stRow, Error& stError, M C::*... pMembers);
	template <typename C, typename... M>
	static OI::Coro::TaskResult<int32_t> SelectByKey(Client stClient, std::string sTable, std::vector<C>& vRows, Error& stError, M C::*... pMembers);
	template <typename T>
	static OI::Coro::TaskResult<int32_t> ReadScalar(Client stClient, std::string sSql, std::vector<Parameter> vParameters, T& stValue, Error& stError);
	static OI::Coro::TaskResult<int32_t> BuildStatus(int32_t iCode, Error& stError);
	static OI::Coro::TaskResult<int32_t> ExecuteWrite(Client stClient, BoundSql stBoundSql, Error& stError);
	static OI::Coro::TaskResult<int32_t> ExecuteTableBatch(Client stClient, Batch stBatch, Error& stError);

  public:
	explicit Client(std::shared_ptr<Driver> pDriver, size_t iConnectionLimit,
	    OI::Thread::BlockingType iType = OI::Thread::BlockingType::Mysql);
	// Await all calls, then await Close before stopping the framework or blocking pool.
	// A rejected Close leaves the client closed; retry Close to release idle connections.
	// Without Close, destruction may fall back to synchronous connection cleanup.
	[[nodiscard]] OI::Coro::TaskResult<int32_t> Close() const;
	[[nodiscard]] OI::Coro::TaskResult<int32_t> Call(std::string sSql, std::vector<Parameter> vParameters, Result& stResult, Error& stError) const;
	template <typename C>
	[[nodiscard]] OI::Coro::TaskResult<int32_t> CreateTable(const std::string& sTable, Error& stError) const;
	template <typename C>
	[[nodiscard]] OI::Coro::TaskResult<int32_t> Insert(const std::string& sTable, const C& stRow, Error& stError) const;
	template <typename C>
	[[nodiscard]] OI::Coro::TaskResult<int32_t> Insert(const std::string& sTable, const C& stRow, Result& stResult, Error& stError) const;
	template <typename C>
	[[nodiscard]] OI::Coro::TaskResult<int32_t> Insert(const std::string& sTable, const std::vector<C>& vRows, Error& stError) const;
	template <typename C>
	[[nodiscard]] OI::Coro::TaskResult<int32_t> Upsert(const std::string& sTable, const C& stRow, Error& stError) const;
	template <typename C>
	[[nodiscard]] OI::Coro::TaskResult<int32_t> Upsert(const std::string& sTable, const C& stRow, Result& stResult, Error& stError) const;
	template <typename C>
	[[nodiscard]] OI::Coro::TaskResult<int32_t> Upsert(const std::string& sTable, const std::vector<C>& vRows, Error& stError) const;
	template <typename C, typename... M>
	[[nodiscard]] OI::Coro::TaskResult<int32_t> Update(const std::string& sTable, const C& stRow, Error& stError, M C::*... pMembers) const;
	template <typename C, typename... M>
	[[nodiscard]] OI::Coro::TaskResult<int32_t> Update(const std::string& sTable, const C& stRow, Result& stResult, Error& stError, M C::*... pMembers) const;
	template <typename C, typename... M>
	[[nodiscard]] OI::Coro::TaskResult<int32_t> Update(const std::string& sTable, const std::vector<C>& vRows, Error& stError, M C::*... pMembers) const;
	template <typename C>
	[[nodiscard]] OI::Coro::TaskResult<int32_t> Delete(const std::string& sTable, const C& stRow, Error& stError) const;
	template <typename C>
	[[nodiscard]] OI::Coro::TaskResult<int32_t> Delete(const std::string& sTable, const C& stRow, Result& stResult, Error& stError) const;
	template <typename C>
	[[nodiscard]] OI::Coro::TaskResult<int32_t> Delete(const std::string& sTable, const std::vector<C>& vRows, Error& stError) const;
	template <typename C, typename... M>
	[[nodiscard]] OI::Coro::TaskResult<int32_t> Select(const std::string& sTable, C& stRow, Error& stError, M C::*... pMembers) const;
	template <typename C, typename... M>
	[[nodiscard]] OI::Coro::TaskResult<int32_t> Select(const std::string& sTable, std::vector<C>& vRows, Error& stError, M C::*... pMembers) const;
	template <typename C>
	[[nodiscard]] OI::Coro::TaskResult<int32_t> SelectAll(const std::string& sTable, std::vector<C>& vRows, Error& stError) const;
	template <typename T>
	[[nodiscard]] OI::Coro::TaskResult<int32_t> Scalar(std::string sSql, std::vector<Parameter> vParameters, T& stValue, Error& stError) const;
	// Only transactional DML (and reads) on transactional tables is supported.
	// The caller must not submit DDL, transaction control, stored procedures, or
	// statements with implicit commits. MySQL cannot roll those effects back.
	[[nodiscard]] OI::Coro::TaskResult<int32_t> Transaction(Batch stBatch, std::vector<Result>& vResults, Error& stError) const;
};
}

// 用在返回框架状态码的数据库协程中。需要恢复或重试的业务可以自己 catch。
#define OI_DB_TRY try
#define OI_DB_CATCH                                                                            \
	catch (const ::OI::Db::SqlError& stDbException)                                            \
	{                                                                                          \
		const ::OI::Log::DiagnosticText stDiagnostic(stDbException.what());                    \
		LOG_ERROR("database failure code=%d %s", stDbException.iCode, stDiagnostic.GetText()); \
		co_return stDbException.iCode;                                                         \
	}                                                                                          \
	catch (...)                                                                                \
	{                                                                                          \
		LOG_ERROR("database failure code=%d unexpected exception", OI_ERR_FAIL);               \
		co_return OI_ERR_FAIL;                                                                 \
	}
