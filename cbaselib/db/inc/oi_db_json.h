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
bool DecodeDbJson(const std::string& sBytes, M& stOutput)
{
	return ::OI::Rpc::FromJsonText(sBytes, stOutput);
}

}
