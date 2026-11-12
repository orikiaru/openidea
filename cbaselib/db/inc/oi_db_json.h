#pragma once
#include "oi_db_row.h"
#include "oi_json.h"

namespace OI::Db
{
// [dbjson] 字段：vector、map、rpcdata 存成 JSON 文本，不走 PB blob。
template <typename M>
Parameter EncodeDbJson(const M& stValue)
{
	return ::OI::Rpc::ToJsonText(stValue);
}

template <typename M>
bool DecodeDbJson(const Blob& stBlob, M& stOutput)
{
	return ::OI::Rpc::FromJsonText(std::string(reinterpret_cast<const char*>(stBlob.data()), stBlob.size()), stOutput);
}

// JSON 字段不能做主键。列类型默认 LONGTEXT，[dbtype] 可换成别的文本类型。
template <typename C, typename M>
constexpr Field<C, M> JsonField(const char* pColumn, M C::*pMember, const char* pSqlType = nullptr)
{
	return Field<C, M>{pColumn, pMember, &EncodeDbJson<M>, &DecodeDbJson<M>, pSqlType ? pSqlType : "LONGTEXT", false};
}
}
