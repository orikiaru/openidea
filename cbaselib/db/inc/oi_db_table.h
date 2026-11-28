#pragma once
#include "oi_db_row.h"
#include "oi_db_driver.h"
#include <stdexcept>
#include <string>
#include <vector>

// 按标了 db 的 rpcdata 生成单表语句：建表、整行写入、按主键读、改、删。
// 客户端执行常规单表操作；JOIN、任意条件、跨表和 ALTER 可直接传 SQL。
// 失败都返回 OI_ERR_INVALID_ARGUMENT，stBoundSql 不变。
namespace OI::Db
{
// 一列的建表和写入信息，按 DbFields 的顺序。
struct ColumnSpec
{
	std::string sName;
	std::string sSqlType;
	bool bKey = false;
	Parameter stValue;
	bool bAutoIncrement = false;
	const char* pSqlDefault = "";
};

// 主键始终需要，部分更新只额外编码选中的列。
template <typename C>
std::vector<ColumnSpec> DescribeRowValues(const C& stRow, bool bAllValues, const std::vector<std::string>& vPicked)
{
	std::vector<ColumnSpec> vColumns;
	vColumns.reserve(C::DbFields().size());
	for (const auto& stField : C::DbFields())
	{
		ColumnSpec stColumn{stField.pColumn, stField.pSqlType, stField.bKey, {}, stField.bAutoIncrement, stField.pSqlDefault};
		if (bAllValues || stField.bKey || std::find(vPicked.begin(), vPicked.end(), stColumn.sName) != vPicked.end())
		{
			stColumn.stValue = stField.fnEncode(stRow);
		}
		vColumns.push_back(std::move(stColumn));
	}
	return vColumns;
}

template <typename C>
std::vector<ColumnSpec> DescribeRow(const C& stRow)
{
	return DescribeRowValues(stRow, true, {});
}

template <typename C>
bool DescribeStatementRow(const C& stRow, std::vector<ColumnSpec>& vColumns, bool bAllValues, const std::vector<std::string>& vPicked = {})
{
	try
	{
		vColumns = DescribeRowValues(stRow, bAllValues, vPicked);
	}
	catch (const std::invalid_argument&)
	{
		return false;
	}
	return true;
}

// 表名由调用方给，拼进 SQL 前只认字母、数字、下划线，最长 64。
inline bool ValidTableName(const std::string& sTable)
{
	if (sTable.empty() || sTable.size() > 64)
	{
		return false;
	}
	for (const char iChar : sTable)
	{
		const bool bWord = (iChar >= 'a' && iChar <= 'z') || (iChar >= 'A' && iChar <= 'Z') || (iChar >= '0' && iChar <= '9') || iChar == '_';
		if (!bWord)
		{
			return false;
		}
	}
	return true;
}

inline std::string QuoteName(const std::string& sName)
{
	return "`" + sName + "`";
}

// 主键条件 "`a`=? AND `b`=?" 和对应参数。没有 [dbkey] 字段返回 false。
inline bool KeyCondition(const std::vector<ColumnSpec>& vColumns, std::string& sCondition, std::vector<Parameter>& vParameters)
{
	std::string sResult;
	std::vector<Parameter> vResult;
	for (const ColumnSpec& stColumn : vColumns)
	{
		if (!stColumn.bKey)
		{
			continue;
		}
		sResult += (sResult.empty() ? "" : " AND ") + QuoteName(stColumn.sName) + "=?";
		vResult.push_back(stColumn.stValue);
	}
	if (vResult.empty())
	{
		return false;
	}
	sCondition = std::move(sResult);
	vParameters = std::move(vResult);
	return true;
}

inline const ColumnSpec* FindColumn(const std::vector<ColumnSpec>& vColumns, const std::string& sName)
{
	for (const ColumnSpec& stColumn : vColumns)
	{
		if (stColumn.sName == sName)
		{
			return &stColumn;
		}
	}
	return nullptr;
}

template <typename C, typename M>
bool AppendPickedName(std::vector<std::string>& vNames, M C::*pMember)
{
	const Field<C>* pField = FindField(pMember);
	if (!pField)
	{
		return false;
	}
	const std::string sName = pField->pColumn;
	if (std::find(vNames.begin(), vNames.end(), sName) != vNames.end())
	{
		return false;
	}
	vNames.push_back(sName);
	return true;
}

// 成员指针换成列名，按传入顺序。不是数据库字段或重复时返回 false。
template <typename C, typename... M>
bool PickedNames(std::vector<std::string>& vNames, M C::*... pMembers)
{
	return (AppendPickedName(vNames, pMembers) && ...);
}

// CREATE TABLE IF NOT EXISTS，列都是 NOT NULL，主键是全部 [dbkey] 字段。没有主键时失败。
template <typename C>
int32_t BuildCreateTable(const std::string& sTable, BoundSql& stBoundSql)
{
	if (!ValidTableName(sTable))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	const std::vector<ColumnSpec> vColumns = DescribeRow(C{});
	std::string sSql = "CREATE TABLE IF NOT EXISTS " + QuoteName(sTable) + " (";
	std::string sKeys;
	for (const ColumnSpec& stColumn : vColumns)
	{
		sSql += QuoteName(stColumn.sName) + " " + stColumn.sSqlType + " NOT NULL";
		if (stColumn.bAutoIncrement)
		{
			sSql += " AUTO_INCREMENT";
		}
		sSql += stColumn.pSqlDefault;
		sSql += ", ";
		if (stColumn.bKey)
		{
			sKeys += (sKeys.empty() ? "" : ",") + QuoteName(stColumn.sName);
		}
	}
	if (sKeys.empty())
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	sSql += "PRIMARY KEY (" + sKeys + ")";
	if constexpr (requires { C::DbIndexDefinitions(); })
	{
		sSql += ", ";
		sSql += C::DbIndexDefinitions();
	}
	sSql += ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
	stBoundSql = BoundSql{std::move(sSql), {}};
	return OI_OK;
}

// INSERT 整行，主键冲突时由数据库报错。
template <typename C>
int32_t BuildInsert(const std::string& sTable, const C& stRow, BoundSql& stBoundSql)
{
	if (!ValidTableName(sTable))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	std::vector<ColumnSpec> vColumns;
	if (!DescribeStatementRow(stRow, vColumns, true))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	std::string sNames;
	std::string sMarks;
	std::vector<Parameter> vParameters;
	for (ColumnSpec& stColumn : vColumns)
	{
		sNames += (sNames.empty() ? "" : ",") + QuoteName(stColumn.sName);
		sMarks += sMarks.empty() ? "?" : ",?";
		vParameters.push_back(InsertParameter(std::move(stColumn.stValue), stColumn.bAutoIncrement));
	}
	stBoundSql = BoundSql{"INSERT INTO " + QuoteName(sTable) + " (" + sNames + ") VALUES (" + sMarks + ")", std::move(vParameters)};
	return OI_OK;
}

// 整行写入，主键已存在时改写其余列。需要 [dbkey]。
template <typename C>
int32_t BuildUpsert(const std::string& sTable, const C& stRow, BoundSql& stBoundSql)
{
	if (!ValidTableName(sTable))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	std::vector<ColumnSpec> vColumns;
	if (!DescribeStatementRow(stRow, vColumns, true))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	std::string sNames;
	std::string sMarks;
	std::string sUpdates;
	std::string sFirstKey;
	std::vector<Parameter> vParameters;
	std::vector<Parameter> vUpdates;
	for (const ColumnSpec& stColumn : vColumns)
	{
		sNames += (sNames.empty() ? "" : ",") + QuoteName(stColumn.sName);
		sMarks += sMarks.empty() ? "?" : ",?";
		vParameters.push_back(InsertParameter(stColumn.stValue, stColumn.bAutoIncrement));
		if (stColumn.bKey)
		{
			if (sFirstKey.empty())
			{
				sFirstKey = QuoteName(stColumn.sName);
			}
			continue;
		}
		sUpdates += (sUpdates.empty() ? "" : ",") + QuoteName(stColumn.sName) + "=?";
		vUpdates.push_back(stColumn.stValue);
	}
	if (sFirstKey.empty())
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	if (sUpdates.empty())
	{
		sUpdates = sFirstKey + "=" + sFirstKey;
	}
	vParameters.insert(vParameters.end(), vUpdates.begin(), vUpdates.end());
	stBoundSql = BoundSql{"INSERT INTO " + QuoteName(sTable) + " (" + sNames + ") VALUES (" + sMarks + ") ON DUPLICATE KEY UPDATE " + sUpdates,
	    std::move(vParameters)};
	return OI_OK;
}

// 按 stRow 的主键读。不传成员读整行，传了只读这几列，读回按列名解码。
template <typename C, typename... M>
int32_t BuildSelectByKey(const std::string& sTable, const C& stRow, BoundSql& stBoundSql, M C::*... pMembers)
{
	if (!ValidTableName(sTable))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	std::vector<ColumnSpec> vColumns;
	if (!DescribeStatementRow(stRow, vColumns, false))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	std::string sCondition;
	std::vector<Parameter> vParameters;
	std::vector<std::string> vNames;
	if (!KeyCondition(vColumns, sCondition, vParameters) || !PickedNames<C>(vNames, pMembers...))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	if (vNames.empty())
	{
		for (const ColumnSpec& stColumn : vColumns)
		{
			vNames.push_back(stColumn.sName);
		}
	}
	std::string sNames;
	for (const std::string& sName : vNames)
	{
		sNames += (sNames.empty() ? "" : ",") + QuoteName(sName);
	}
	stBoundSql = BoundSql{"SELECT " + sNames + " FROM " + QuoteName(sTable) + " WHERE " + sCondition, std::move(vParameters)};
	return OI_OK;
}

// 按 stRow 的主键改。不传成员改全部非主键列，传了只改这几列；主键列不能改。
template <typename C, typename... M>
int32_t BuildUpdateByKey(const std::string& sTable, const C& stRow, BoundSql& stBoundSql, M C::*... pMembers)
{
	if (!ValidTableName(sTable))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	std::vector<std::string> vNames;
	std::vector<ColumnSpec> vColumns;
	std::string sCondition;
	std::vector<Parameter> vKeys;
	if (!PickedNames<C>(vNames, pMembers...) || !DescribeStatementRow(stRow, vColumns, vNames.empty(), vNames) || !KeyCondition(vColumns, sCondition, vKeys))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	if (vNames.empty())
	{
		for (const ColumnSpec& stColumn : vColumns)
		{
			if (!stColumn.bKey)
			{
				vNames.push_back(stColumn.sName);
			}
		}
	}
	if (vNames.empty())
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	std::string sAssignments;
	std::vector<Parameter> vParameters;
	for (const std::string& sName : vNames)
	{
		const ColumnSpec* pColumn = FindColumn(vColumns, sName);
		if (!pColumn || pColumn->bKey)
		{
			return OI_ERR_INVALID_ARGUMENT;
		}
		sAssignments += (sAssignments.empty() ? "" : ",") + QuoteName(sName) + "=?";
		vParameters.push_back(pColumn->stValue);
	}
	vParameters.insert(vParameters.end(), vKeys.begin(), vKeys.end());
	stBoundSql = BoundSql{"UPDATE " + QuoteName(sTable) + " SET " + sAssignments + " WHERE " + sCondition, std::move(vParameters)};
	return OI_OK;
}

// 按 stRow 的主键删。
template <typename C>
int32_t BuildDeleteByKey(const std::string& sTable, const C& stRow, BoundSql& stBoundSql)
{
	if (!ValidTableName(sTable))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	std::vector<ColumnSpec> vColumns;
	if (!DescribeStatementRow(stRow, vColumns, false))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	std::string sCondition;
	std::vector<Parameter> vParameters;
	if (!KeyCondition(vColumns, sCondition, vParameters))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	stBoundSql = BoundSql{"DELETE FROM " + QuoteName(sTable) + " WHERE " + sCondition, std::move(vParameters)};
	return OI_OK;
}

template <typename C, typename F, typename... M>
OI::Coro::TaskResult<int32_t> Client::WriteTable(const std::string& sTable, const C& stRow, Result* pResult, Error& stError, F fnBuild, M... pMembers) const
{
	BoundSql stBoundSql;
	const int32_t iCode = fnBuild(sTable, stRow, stBoundSql, pMembers...);
	if (iCode != OI_OK)
	{
		return BuildStatus(iCode, stError);
	}
	if (pResult)
	{
		return Call(std::move(stBoundSql.sSql), std::move(stBoundSql.vParameters), *pResult, stError);
	}
	return ExecuteWrite(*this, std::move(stBoundSql), stError);
}

template <typename C, typename F, typename... M>
OI::Coro::TaskResult<int32_t> Client::WriteTable(const std::string& sTable, const std::vector<C>& vRows, Error& stError, F fnBuild, M... pMembers) const
{
	Batch stBatch;
	for (const auto& stRow : vRows)
	{
		BoundSql stBoundSql;
		const int32_t iCode = fnBuild(sTable, stRow, stBoundSql, pMembers...);
		if (iCode != OI_OK)
		{
			return BuildStatus(iCode, stError);
		}
		stBatch.Add(std::move(stBoundSql.sSql), std::move(stBoundSql.vParameters));
	}
	if (vRows.empty())
	{
		return BuildStatus(OI_OK, stError);
	}
	return ExecuteTableBatch(*this, std::move(stBatch), stError);
}

template <typename C>
int32_t Client::ReadByKey(const Result& stResult, C& stRow)
{
	if (stResult.vRows.empty())
	{
		return OI_ERR_NOT_FOUND;
	}
	if (stResult.vRows.size() != 1 || !stRow.FromDbMap(stResult.vRows.front().mCells))
	{
		return OI_ERR_DECODE;
	}
	return OI_OK;
}

template <typename C>
OI::Coro::TaskResult<int32_t> Client::CreateTable(const std::string& sTable, Error& stError) const
{
	BoundSql stBoundSql;
	const int32_t iCode = BuildCreateTable<C>(sTable, stBoundSql);
	if (iCode != OI_OK)
	{
		return BuildStatus(iCode, stError);
	}
	return ExecuteWrite(*this, std::move(stBoundSql), stError);
}

template <typename C>
OI::Coro::TaskResult<int32_t> Client::Insert(const std::string& sTable, const C& stRow, Error& stError) const
{
	return WriteTable(sTable, stRow, nullptr, stError, BuildInsert<C>);
}

template <typename C>
OI::Coro::TaskResult<int32_t> Client::Insert(const std::string& sTable, const C& stRow, Result& stResult, Error& stError) const
{
	return WriteTable(sTable, stRow, &stResult, stError, BuildInsert<C>);
}

template <typename C>
OI::Coro::TaskResult<int32_t> Client::Insert(const std::string& sTable, const std::vector<C>& vRows, Error& stError) const
{
	return WriteTable(sTable, vRows, stError, BuildInsert<C>);
}

template <typename C>
OI::Coro::TaskResult<int32_t> Client::Upsert(const std::string& sTable, const C& stRow, Error& stError) const
{
	return WriteTable(sTable, stRow, nullptr, stError, BuildUpsert<C>);
}

template <typename C>
OI::Coro::TaskResult<int32_t> Client::Upsert(const std::string& sTable, const C& stRow, Result& stResult, Error& stError) const
{
	return WriteTable(sTable, stRow, &stResult, stError, BuildUpsert<C>);
}

template <typename C>
OI::Coro::TaskResult<int32_t> Client::Upsert(const std::string& sTable, const std::vector<C>& vRows, Error& stError) const
{
	return WriteTable(sTable, vRows, stError, BuildUpsert<C>);
}

template <typename C, typename... M>
OI::Coro::TaskResult<int32_t> Client::Update(const std::string& sTable, const C& stRow, Error& stError, M C::*... pMembers) const
{
	return WriteTable(sTable, stRow, nullptr, stError, BuildUpdateByKey<C, M...>, pMembers...);
}

template <typename C, typename... M>
OI::Coro::TaskResult<int32_t> Client::Update(const std::string& sTable, const C& stRow, Result& stResult, Error& stError, M C::*... pMembers) const
{
	return WriteTable(sTable, stRow, &stResult, stError, BuildUpdateByKey<C, M...>, pMembers...);
}

template <typename C, typename... M>
OI::Coro::TaskResult<int32_t> Client::Update(const std::string& sTable, const std::vector<C>& vRows, Error& stError, M C::*... pMembers) const
{
	return WriteTable(sTable, vRows, stError, BuildUpdateByKey<C, M...>, pMembers...);
}

template <typename C>
OI::Coro::TaskResult<int32_t> Client::Delete(const std::string& sTable, const C& stRow, Error& stError) const
{
	return WriteTable(sTable, stRow, nullptr, stError, BuildDeleteByKey<C>);
}

template <typename C>
OI::Coro::TaskResult<int32_t> Client::Delete(const std::string& sTable, const C& stRow, Result& stResult, Error& stError) const
{
	return WriteTable(sTable, stRow, &stResult, stError, BuildDeleteByKey<C>);
}

template <typename C>
OI::Coro::TaskResult<int32_t> Client::Delete(const std::string& sTable, const std::vector<C>& vRows, Error& stError) const
{
	return WriteTable(sTable, vRows, stError, BuildDeleteByKey<C>);
}

template <typename C>
OI::Coro::TaskResult<int32_t> Client::SelectAllRows(Client stClient, std::string sTable, std::vector<C>& vRows, Error& stError)
{
	if (!ValidTableName(sTable))
	{
		stError = {};
		stError.sStage = "build_statement";
		co_return OI_ERR_INVALID_ARGUMENT;
	}
	std::string sSql = std::string("SELECT ") + C::DbColumnList() + " FROM " + QuoteName(sTable);
	Result stResult;
	const int32_t iCode = co_await stClient.Call(std::move(sSql), {}, stResult, stError);
	if (iCode != OI_OK)
	{
		co_return iCode;
	}
	const int32_t iRead = FromRows(stResult, vRows);
	if (iRead != OI_OK)
	{
		stError.sStage = "decode_row";
	}
	co_return iRead;
}

template <typename C, typename... M>
OI::Coro::TaskResult<int32_t> Client::SelectByKey(Client stClient, std::string sTable, C& stRow, Error& stError, M C::*... pMembers)
{
	BoundSql stBoundSql;
	const int32_t iBuild = BuildSelectByKey(sTable, stRow, stBoundSql, pMembers...);
	if (iBuild != OI_OK)
	{
		stError = {};
		stError.sStage = "build_statement";
		co_return iBuild;
	}
	Result stResult;
	const int32_t iCode = co_await stClient.Call(std::move(stBoundSql.sSql), std::move(stBoundSql.vParameters), stResult, stError);
	if (iCode != OI_OK)
	{
		co_return iCode;
	}
	co_return ReadByKey(stResult, stRow);
}

template <typename C, typename... M>
OI::Coro::TaskResult<int32_t> Client::SelectByKey(Client stClient, std::string sTable, std::vector<C>& vRows, Error& stError, M C::*... pMembers)
{
	if (vRows.empty())
	{
		stError = {};
		co_return OI_OK;
	}
	Batch stBatch;
	auto vDecoded = vRows;
	for (const auto& stRow : vDecoded)
	{
		BoundSql stBoundSql;
		const int32_t iBuild = BuildSelectByKey(sTable, stRow, stBoundSql, pMembers...);
		if (iBuild != OI_OK)
		{
			stError = {};
			stError.sStage = "build_statement";
			co_return iBuild;
		}
		stBatch.Add(std::move(stBoundSql.sSql), std::move(stBoundSql.vParameters));
	}
	std::vector<Result> vResults;
	const int32_t iCode = co_await stClient.Transaction(std::move(stBatch), vResults, stError);
	if (iCode != OI_OK)
	{
		co_return iCode;
	}
	for (size_t iIndex = 0; iIndex < vResults.size(); ++iIndex)
	{
		const int32_t iRead = ReadByKey(vResults[iIndex], vDecoded[iIndex]);
		if (iRead != OI_OK)
		{
			co_return iRead;
		}
	}
	vRows = std::move(vDecoded);
	co_return OI_OK;
}

template <typename C, typename... M>
OI::Coro::TaskResult<int32_t> Client::Select(const std::string& sTable, C& stRow, Error& stError, M C::*... pMembers) const
{
	return SelectByKey(*this, sTable, stRow, stError, pMembers...);
}

template <typename C, typename... M>
OI::Coro::TaskResult<int32_t> Client::Select(const std::string& sTable, std::vector<C>& vRows, Error& stError, M C::*... pMembers) const
{
	return SelectByKey(*this, sTable, vRows, stError, pMembers...);
}
template <typename C>
OI::Coro::TaskResult<int32_t> Client::SelectAll(const std::string& sTable, std::vector<C>& vRows, Error& stError) const
{
	return SelectAllRows(*this, sTable, vRows, stError);
}
}
