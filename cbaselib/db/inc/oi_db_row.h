#pragma once
#include "oi_bytes_stream.h"
#include "oi_db.h"
#include "oi_errno.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <system_error>
#include <type_traits>
#include <utility>

namespace OI::Db
{
// 数值列按 MySQL 以文本取回的十进制写回。空文本、空白、正号和尾随字符都拒绝。
template <typename T>
bool FromDbInteger(const std::string& sBytes, T& stOutput)
{
	static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
	if (sBytes.empty())
	{
		return false;
	}
	const char* pBegin = sBytes.data();
	const char* pEnd = pBegin + sBytes.size();
	if constexpr (std::is_signed_v<T>)
	{
		int64_t iValue = 0;
		const std::from_chars_result stParsed = std::from_chars(pBegin, pEnd, iValue);
		if (stParsed.ec != std::errc() || stParsed.ptr != pEnd)
		{
			return false;
		}
		if constexpr (!std::is_same_v<T, int64_t>)
		{
			if (iValue < (std::numeric_limits<T>::min)() || iValue > (std::numeric_limits<T>::max)())
			{
				return false;
			}
		}
		stOutput = static_cast<T>(iValue);
		return true;
	}
	else
	{
		uint64_t iValue = 0;
		const std::from_chars_result stParsed = std::from_chars(pBegin, pEnd, iValue);
		if (stParsed.ec != std::errc() || stParsed.ptr != pEnd)
		{
			return false;
		}
		if constexpr (!std::is_same_v<T, uint64_t>)
		{
			if (iValue > static_cast<uint64_t>((std::numeric_limits<T>::max)()))
			{
				return false;
			}
		}
		stOutput = static_cast<T>(iValue);
		return true;
	}
}

inline bool FromDbBool(const std::string& sBytes, bool& bOutput)
{
	if (sBytes.size() == 1 && sBytes[0] == static_cast<uint8_t>('0'))
	{
		bOutput = false;
		return true;
	}
	if (sBytes.size() == 1 && sBytes[0] == static_cast<uint8_t>('1'))
	{
		bOutput = true;
		return true;
	}
	return false;
}

inline bool ParseDbDouble(const std::string& sBytes, double& dOutput)
{
	if (sBytes.empty())
	{
		return false;
	}
	const char* pBegin = sBytes.data();
	const char* pEnd = pBegin + sBytes.size();
	double dValue = 0;
	const std::from_chars_result stParsed = std::from_chars(pBegin, pEnd, dValue, std::chars_format::general);
	if (stParsed.ec != std::errc() || stParsed.ptr != pEnd || !std::isfinite(dValue))
	{
		return false;
	}
	dOutput = dValue;
	return true;
}

template <typename T>
bool FromDbReal(const std::string& sBytes, T& stOutput)
{
	static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>);
	double dValue = 0;
	if (!ParseDbDouble(sBytes, dValue))
	{
		return false;
	}
	if constexpr (std::is_same_v<T, float>)
	{
		const float fValue = static_cast<float>(dValue);
		if (!std::isfinite(fValue))
		{
			return false;
		}
		stOutput = fValue;
		return true;
	}
	else
	{
		stOutput = dValue;
		return true;
	}
}

inline bool FromDbText(const std::string& sBytes, std::string& sOutput)
{
	sOutput = sBytes;
	return true;
}

inline bool FromDbText(const std::string& sBytes, ::OI::Base::Bytes& stOutput)
{
	stOutput = ::OI::Base::Bytes(sBytes.empty() ? nullptr : sBytes.data(), sBytes.size());
	return true;
}

// 头一层的 vector、map、rpcdata 不拆列。blob 是这条值自己的 PB：序号固定为 0。
// 所有类型均为一条完整字段，空容器也带数量头。
template <typename T>
std::string ToDbBlob(const T& stValue)
{
	::OI::Base::BytesStream stStream;
	stStream.WritePb(0, stValue);
	if (stStream.Size() == 0)
	{
		return {};
	}
	return std::string(stStream.Data(), stStream.Size());
}

template <typename T>
bool FromDbBlob(const std::string& sBytes, T& stOutput)
{
	try
	{
		::OI::Base::BytesStream stStream(sBytes.empty() ? nullptr : sBytes.data(), sBytes.size());
		T stDecoded{};
		uint32_t iSequence = 0;
		uint32_t iSize = 0;
		stStream.ReadPbHeader(iSequence, iSize);
		if (iSequence != 0)
		{
			return false;
		}
		stStream.ReadPb(iSize, stDecoded);
		if (stStream.RemainSize() != 0)
		{
			return false;
		}
		stOutput = std::move(stDecoded);
		return true;
	}
	catch (const ::OI::Base::ParserException&)
	{
		return false;
	}
}

// 头一层字段按类型绑定：整数、bool 绑成 int64_t / uint64_t，浮点绑成 double，
// string 和 Bytes 原样，vector、map、rpcdata 收成 PB blob。
template <typename M>
Parameter EncodeDbValue(const M& stValue)
{
	if constexpr (std::is_same_v<M, bool>)
	{
		return stValue;
	}
	else if constexpr (std::is_integral_v<M> && std::is_signed_v<M>)
	{
		return static_cast<int64_t>(stValue);
	}
	else if constexpr (std::is_integral_v<M>)
	{
		return static_cast<uint64_t>(stValue);
	}
	else if constexpr (std::is_floating_point_v<M>)
	{
		return static_cast<double>(stValue);
	}
	else if constexpr (std::is_same_v<M, std::string>)
	{
		return stValue;
	}
	else if constexpr (std::is_same_v<M, ::OI::Base::Bytes>)
	{
		return Parameter::Binary(std::string(stValue.Data(), stValue.Size()));
	}
	else
	{
		return Parameter::Binary(ToDbBlob(stValue));
	}
}

template <typename M>
bool DecodeDbValue(const std::string& sBytes, M& stOutput)
{
	if constexpr (std::is_same_v<M, bool>)
	{
		return FromDbBool(sBytes, stOutput);
	}
	else if constexpr (std::is_integral_v<M>)
	{
		return FromDbInteger(sBytes, stOutput);
	}
	else if constexpr (std::is_floating_point_v<M>)
	{
		return FromDbReal(sBytes, stOutput);
	}
	else if constexpr (std::is_same_v<M, std::string> || std::is_same_v<M, ::OI::Base::Bytes>)
	{
		return FromDbText(sBytes, stOutput);
	}
	else
	{
		return FromDbBlob(sBytes, stOutput);
	}
}

// 建表时的默认列类型，和 EncodeDbValue 的绑定对应。string、Bytes 按字节存，
// vector、map、rpcdata 的 PB blob 也是 LONGBLOB；要别的类型由协议写 [dbtype]。
template <typename M>
constexpr const char* DefaultSqlType()
{
	if constexpr (std::is_same_v<M, bool>)
	{
		return "TINYINT";
	}
	else if constexpr (std::is_integral_v<M>)
	{
		constexpr bool bSigned = std::is_signed_v<M>;
		if constexpr (sizeof(M) == 1)
		{
			return bSigned ? "TINYINT" : "TINYINT UNSIGNED";
		}
		else if constexpr (sizeof(M) == 2)
		{
			return bSigned ? "SMALLINT" : "SMALLINT UNSIGNED";
		}
		else if constexpr (sizeof(M) == 4)
		{
			return bSigned ? "INT" : "INT UNSIGNED";
		}
		else
		{
			static_assert(sizeof(M) == 8);
			return bSigned ? "BIGINT" : "BIGINT UNSIGNED";
		}
	}
	else if constexpr (std::is_floating_point_v<M>)
	{
		return "DOUBLE";
	}
	else
	{
		return "LONGBLOB";
	}
}

// 同一行的字段使用统一描述，遍历不再展开异构 tuple。
template <typename C>
struct Field
{
	const char* pColumn;
	Parameter (*fnEncode)(const C&);
	bool (*fnDecode)(const std::string&, C&);
	const char* pSqlType;
	bool bKey;
	bool bAutoIncrement = false;
	const char* pSqlDefault = "";
};

template <typename C>
constexpr size_t FieldCount()
{
	return C::DbFieldCount;
}

// Insert 和 Upsert 把自增字段的零值绑成 SQL NULL，显式 ID 保留原始整型精度。
inline Parameter InsertParameter(Parameter stValue, bool bAutoIncrement)
{
	if (bAutoIncrement && ((stValue.is_signed() && stValue.iSigned == 0) || (stValue.is_unsigned() && stValue.iUnsigned == 0)))
	{
		return nullptr;
	}
	return stValue;
}

template <typename C>
void AppendRowParameters(const C& stRow, std::vector<Parameter>& vParameters)
{
	for (const auto& stField : C::DbFields())
	{
		vParameters.push_back(InsertParameter(stField.fnEncode(stRow), stField.bAutoIncrement));
	}
}

// 未知列跳过；已知列重复、NULL 或解码失败时保持原对象。
template <typename C>
bool FromRow(C& stRow, const std::vector<std::string>& vColumns, const std::vector<std::optional<std::string>>& vRow)
{
	if (vColumns.size() != vRow.size())
	{
		return false;
	}
	C stDecoded(stRow);
	std::vector<uint8_t> vSeen(C::DbFields().size(), 0);
	for (size_t iColumn = 0; iColumn < vColumns.size(); ++iColumn)
	{
		size_t iField = 0;
		for (const auto& stField : C::DbFields())
		{
			if (vColumns[iColumn] == stField.pColumn)
			{
				if (vSeen[iField] || !vRow[iColumn] || !stField.fnDecode(*vRow[iColumn], stDecoded))
				{
					return false;
				}
				vSeen[iField] = 1;
				break;
			}
			++iField;
		}
	}
	stRow = std::move(stDecoded);
	return true;
}

// 整份结果按列名读成数组。任一行列数不对、重复列、NULL 或解码失败时返回 OI_ERR_DECODE，vRows 不变。
template <typename C>
int32_t FromRows(const Result& stResult, std::vector<C>& vRows)
{
	std::vector<C> vDecoded;
	vDecoded.reserve(stResult.vRows.size());
	for (const auto& stResultRow : stResult.vRows)
	{
		C stRow;
		if (!stRow.FromDbMap(stResultRow.mCells))
		{
			return OI_ERR_DECODE;
		}
		vDecoded.push_back(std::move(stRow));
	}
	vRows = std::move(vDecoded);
	return OI_OK;
}

using ParameterMap = std::map<std::string, Parameter>;

template <typename C>
ParameterMap ToParameterMap(const C& stRow)
{
	ParameterMap mParameters;
	for (const auto& stField : C::DbFields())
	{
		mParameters[stField.pColumn] = stField.fnEncode(stRow);
	}
	return mParameters;
}

// 只更新提供的数据库列，全部成功后再提交。
template <typename C>
bool FromCellMap(C& stRow, const CellMap& mCells)
{
	C stDecoded(stRow);
	for (const auto& stField : C::DbFields())
	{
		const auto it = mCells.find(stField.pColumn);
		if (it != mCells.end() && (!it->second || !stField.fnDecode(*it->second, stDecoded)))
		{
			return false;
		}
	}
	stRow = std::move(stDecoded);
	return true;
}

// 单行单列结果按数据库字段规则解码，失败时保留调用方原值。
template <typename T>
OI::Coro::TaskResult<int32_t> Client::ReadScalar(Client stClient, std::string sSql, std::vector<Parameter> vParameters, T& stValue, Error& stError)
{
	Result stResult;
	const int32_t iCode = co_await stClient.Call(std::move(sSql), std::move(vParameters), stResult, stError);
	if (iCode != OI_OK)
	{
		co_return iCode;
	}
	if (stResult.vRows.size() != 1 || stResult.vRows.front().mCells.size() != 1)
	{
		stError.sStage = "scalar_shape";
		co_return OI_ERR_DECODE;
	}
	const auto& stCell = stResult.vRows.front().mCells.begin()->second;
	T stDecoded{};
	if (!stCell || !DecodeDbValue(*stCell, stDecoded))
	{
		stError.sStage = "scalar_value";
		co_return OI_ERR_DECODE;
	}
	stValue = std::move(stDecoded);
	co_return OI_OK;
}

template <typename T>
OI::Coro::TaskResult<int32_t> Client::Scalar(std::string sSql, std::vector<Parameter> vParameters, T& stValue, Error& stError) const
{
	return ReadScalar(*this, std::move(sSql), std::move(vParameters), stValue, stError);
}

template <typename C, typename M>
const Field<C>* FindField(M C::*pMember)
{
	return C::DbFindField(pMember);
}

template <typename C, typename M>
const char* ColumnOf(M C::*pMember)
{
	const Field<C>* pField = FindField(pMember);
	return pField ? pField->pColumn : nullptr;
}

// 按成员指针挑出的几列，顺序就是传入顺序。表名和其余 SQL 仍由调用方写。
struct Picked
{
	std::vector<std::string> vColumns;
	std::string sColumns;
	std::string sMarks;
	std::string sAssignments;
	std::string sConditions;
	std::vector<Parameter> vParameters;
};

template <typename C, typename M>
int32_t PickOne(const C& stRow, Picked& stPicked, M C::*pMember)
{
	const Field<C>* pField = FindField(pMember);
	if (!pField)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	const std::string sColumn = pField->pColumn;
	if (std::find(stPicked.vColumns.begin(), stPicked.vColumns.end(), sColumn) != stPicked.vColumns.end())
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	const bool bFirst = stPicked.vColumns.empty();
	const std::string sQuoted = "`" + sColumn + "`";
	stPicked.sColumns += (bFirst ? "" : ",") + sQuoted;
	stPicked.sMarks += bFirst ? "?" : ",?";
	stPicked.sAssignments += (bFirst ? "" : ",") + sQuoted + "=?";
	stPicked.sConditions += (bFirst ? "" : " AND ") + sQuoted + "=?";
	stPicked.vColumns.push_back(sColumn);
	stPicked.vParameters.push_back(pField->fnEncode(stRow));
	return OI_OK;
}

// 例：Pick(stRow, stSet, &Row::sName) 和 Pick(stRow, stWhere, &Row::iId) 拼成
// "UPDATE t SET " + stSet.sAssignments + " WHERE " + stWhere.sConditions，参数先 stSet 后 stWhere。
// 任一成员不是数据库字段或重复时返回 OI_ERR_INVALID_ARGUMENT，stPicked 不变。
template <typename C, typename... M>
int32_t Pick(const C& stRow, Picked& stPicked, M C::*... pMembers)
{
	static_assert(sizeof...(M) > 0);
	Picked stResult;
	int32_t iCode = OI_OK;
	(void)(((iCode = PickOne(stRow, stResult, pMembers)) == OI_OK) && ...);
	if (iCode != OI_OK)
	{
		return iCode;
	}
	stPicked = std::move(stResult);
	return OI_OK;
}
}
