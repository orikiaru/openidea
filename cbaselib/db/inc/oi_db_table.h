#pragma once
#include "oi_db_row.h"
#include <string>
#include <vector>

// 按标了 db 的 rpcdata 生成单表语句：建表、整行写入、按主键读、改、删。
// 只拼 Statement，不执行；JOIN、任意条件、跨表和 ALTER 仍由调用方写 SQL。
// 失败都返回 OI_ERR_INVALID_ARGUMENT，stStatement 不变。
namespace OI::Db
{
// 一列的建表和写入信息，按 DbFields 的顺序。
struct ColumnSpec
{
	std::string sName;
	std::string sSqlType;
	bool bKey = false;
	Parameter stValue;
};

template <typename C, size_t... iIndex>
void DescribeRowAt(const C& stRow, std::vector<ColumnSpec>& vColumns, std::index_sequence<iIndex...>)
{
	const auto& stFields = C::DbFields();
	(vColumns.push_back(ColumnSpec{std::get<iIndex>(stFields).pColumn, std::get<iIndex>(stFields).pSqlType, std::get<iIndex>(stFields).bKey,
		 std::get<iIndex>(stFields).fnEncode(stRow.*(std::get<iIndex>(stFields).pMember))}),
		...);
}

template <typename C>
std::vector<ColumnSpec> DescribeRow(const C& stRow)
{
	std::vector<ColumnSpec> vColumns;
	vColumns.reserve(FieldCount<C>());
	DescribeRowAt(stRow, vColumns, std::make_index_sequence<FieldCount<C>()>{});
	return vColumns;
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
	const Field<C, M>* pField = FindField(pMember);
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
int32_t BuildCreateTable(const std::string& sTable, Statement& stStatement)
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
		sSql += QuoteName(stColumn.sName) + " " + stColumn.sSqlType + " NOT NULL, ";
		if (stColumn.bKey)
		{
			sKeys += (sKeys.empty() ? "" : ",") + QuoteName(stColumn.sName);
		}
	}
	if (sKeys.empty())
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	sSql += "PRIMARY KEY (" + sKeys + ")) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
	stStatement = Statement{std::move(sSql), {}};
	return OI_OK;
}

// INSERT 整行，主键冲突时由数据库报错。
template <typename C>
int32_t BuildInsert(const std::string& sTable, const C& stRow, Statement& stStatement)
{
	if (!ValidTableName(sTable))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	std::vector<ColumnSpec> vColumns = DescribeRow(stRow);
	std::string sNames;
	std::string sMarks;
	std::vector<Parameter> vParameters;
	for (ColumnSpec& stColumn : vColumns)
	{
		sNames += (sNames.empty() ? "" : ",") + QuoteName(stColumn.sName);
		sMarks += sMarks.empty() ? "?" : ",?";
		vParameters.push_back(std::move(stColumn.stValue));
	}
	stStatement = Statement{"INSERT INTO " + QuoteName(sTable) + " (" + sNames + ") VALUES (" + sMarks + ")", std::move(vParameters)};
	return OI_OK;
}

// 整行写入，主键已存在时改写其余列。需要 [dbkey]。
template <typename C>
int32_t BuildUpsert(const std::string& sTable, const C& stRow, Statement& stStatement)
{
	if (!ValidTableName(sTable))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	const std::vector<ColumnSpec> vColumns = DescribeRow(stRow);
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
		vParameters.push_back(stColumn.stValue);
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
	stStatement = Statement{"INSERT INTO " + QuoteName(sTable) + " (" + sNames + ") VALUES (" + sMarks + ") ON DUPLICATE KEY UPDATE " + sUpdates,
		std::move(vParameters)};
	return OI_OK;
}

// 按 stRow 的主键读。不传成员读整行，传了只读这几列，读回用 RowCells + FromDbMap。
template <typename C, typename... M>
int32_t BuildSelectByKey(const std::string& sTable, const C& stRow, Statement& stStatement, M C::*... pMembers)
{
	if (!ValidTableName(sTable))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	const std::vector<ColumnSpec> vColumns = DescribeRow(stRow);
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
	stStatement = Statement{"SELECT " + sNames + " FROM " + QuoteName(sTable) + " WHERE " + sCondition, std::move(vParameters)};
	return OI_OK;
}

// 按 stRow 的主键改。不传成员改全部非主键列，传了只改这几列；主键列不能改。
template <typename C, typename... M>
int32_t BuildUpdateByKey(const std::string& sTable, const C& stRow, Statement& stStatement, M C::*... pMembers)
{
	if (!ValidTableName(sTable))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	const std::vector<ColumnSpec> vColumns = DescribeRow(stRow);
	std::string sCondition;
	std::vector<Parameter> vKeys;
	std::vector<std::string> vNames;
	if (!KeyCondition(vColumns, sCondition, vKeys) || !PickedNames<C>(vNames, pMembers...))
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
	stStatement = Statement{"UPDATE " + QuoteName(sTable) + " SET " + sAssignments + " WHERE " + sCondition, std::move(vParameters)};
	return OI_OK;
}

// 按 stRow 的主键删。
template <typename C>
int32_t BuildDeleteByKey(const std::string& sTable, const C& stRow, Statement& stStatement)
{
	if (!ValidTableName(sTable))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	const std::vector<ColumnSpec> vColumns = DescribeRow(stRow);
	std::string sCondition;
	std::vector<Parameter> vParameters;
	if (!KeyCondition(vColumns, sCondition, vParameters))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	stStatement = Statement{"DELETE FROM " + QuoteName(sTable) + " WHERE " + sCondition, std::move(vParameters)};
	return OI_OK;
}
}
