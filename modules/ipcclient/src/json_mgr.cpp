#include "json_mgr.h"
#include "hold_test.h"
#include "oi_rpc_coro.h"
#include "oi_http.h"
#include "oi_log_client.h"
#include "oi_log.h"
#include "oi_registry.h"
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include "oi_rpcdata_jsonall.h"
#include <limits>
#include "oi_config.h"
#include "oi_rpcdata_allechoreq.h"
#include "oi_rpcdata_allechores.h"
#include "oi_rpcdata_holdreq.h"
#include "oi_rpcdata_holdres.h"
#include "ipcclient.h"
#include "rpc_ids.h"
#include "oi_redis.h"
#include "oi_poll.h"
#include "oi_net_session.h"
#include "oi_timer_manager.h"
#include <uv.h>
#include "test_board.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>


// Run with the existing Test.hold switch in both thread modes.
template <typename T>
int JsonMgr::CheckJsonInteger()
{
	int iBad = 0;
	T stValue{};
	const Json::Value stMinimum = std::is_signed_v<T>
	                                  ? Json::Value(static_cast<Json::Int64>((std::numeric_limits<T>::min)()))
	                                  : Json::Value(Json::UInt64(0));
	const Json::Value stMaximum(static_cast<Json::UInt64>((std::numeric_limits<T>::max)()));
	if (!OI::Rpc::FromJsonValue(stMinimum, stValue) || stValue != (std::numeric_limits<T>::min)())
	{
		++iBad;
	}
	if (!OI::Rpc::FromJsonValue(stMaximum, stValue) || stValue != (std::numeric_limits<T>::max)())
	{
		++iBad;
	}
	std::vector<Json::Value> vInvalid{Json::Value(1.0), Json::Value(true), Json::Value("1"), Json::Value()};
	if constexpr (std::is_signed_v<T>)
	{
		vInvalid.emplace_back(static_cast<Json::UInt64>((std::numeric_limits<T>::max)()) + 1);
		if constexpr (sizeof(T) < sizeof(int64_t))
		{
			vInvalid.emplace_back(static_cast<Json::Int64>((std::numeric_limits<T>::min)()) - 1);
		}
	}
	else
	{
		vInvalid.emplace_back(-1);
		if constexpr (sizeof(T) < sizeof(uint64_t))
		{
			vInvalid.emplace_back(static_cast<Json::UInt64>((std::numeric_limits<T>::max)()) + 1);
		}
	}
	for (const auto& stInput : vInvalid)
	{
		stValue = 7;
		if (OI::Rpc::FromJsonValue(stInput, stValue) || stValue != 7)
		{
			++iBad;
		}
	}
	return iBad;
}

int JsonMgr::CheckJsonDecoding()
{
	int iBad = CheckJsonInteger<int8_t>()
		+ CheckJsonInteger<uint8_t>()
		+ CheckJsonInteger<int16_t>()
		+ CheckJsonInteger<uint16_t>()
		+ CheckJsonInteger<int32_t>()
		+ CheckJsonInteger<uint32_t>()
		+ CheckJsonInteger<int64_t>()
		+ CheckJsonInteger<uint64_t>();
	Json::Value stInput(Json::objectValue);
	stInput[std::string(40, '0') + "1"] = 1;
	std::map<int32_t, int32_t> mSigned;
	if (!OI::Rpc::FromJsonValue(stInput, mSigned) || mSigned != std::map<int32_t, int32_t>{{1, 1}})
	{
		++iBad;
	}
	const std::vector<std::string> vInvalidKeys{"", "2147483648", "-2147483649", "1x", " 1", "+1", std::string("1\0x", 3)};
	for (const auto& sKey : vInvalidKeys)
	{
		stInput = Json::Value(Json::objectValue);
		stInput[sKey] = 2;
		if (OI::Rpc::FromJsonValue(stInput, mSigned) || mSigned != std::map<int32_t, int32_t>{{1, 1}})
		{
			++iBad;
		}
	}
	std::map<uint64_t, int32_t> mUnsigned{{3, 7}};
	for (const auto& sKey : std::vector<std::string>{"-1", "18446744073709551616", "1x"})
	{
		stInput = Json::Value(Json::objectValue);
		stInput[sKey] = 2;
		if (OI::Rpc::FromJsonValue(stInput, mUnsigned) || mUnsigned != std::map<uint64_t, int32_t>{{3, 7}})
		{
			++iBad;
		}
	}
	stInput = Json::Value(Json::objectValue);
	stInput["18446744073709551615"] = 2;
	if (!OI::Rpc::FromJsonValue(stInput, mUnsigned) || mUnsigned.count(UINT64_MAX) != 1)
	{
		++iBad;
	}
	std::vector<int8_t> vValues{7};
	Json::Value stArray(Json::arrayValue);
	stArray.append(1);
	stArray.append(128);
	if (OI::Rpc::FromJsonValue(stArray, vValues) || vValues != std::vector<int8_t>{7})
	{
		++iBad;
	}
	std::map<std::string, int8_t> mValues{{"old", 7}};
	stInput = Json::Value(Json::objectValue);
	stInput["a"] = 1;
	stInput["b"] = 128;
	if (OI::Rpc::FromJsonValue(stInput, mValues) || mValues != std::map<std::string, int8_t>{{"old", 7}})
	{
		++iBad;
	}
	OI::RpcData::JsonAll stOriginal;
	stOriginal.iInt8 = 7;
	stOriginal.sMessage = "unchanged";
	stOriginal.vNumbers = {3, 4};
	const std::string sBefore = stOriginal.ToJson();
	stInput = Json::Value(Json::objectValue);
	stInput["c"] = 1;
	stInput["u8"] = 256;
	if (stOriginal.FromJson(stInput) || stOriginal.ToJson() != sBefore)
	{
		++iBad;
	}
	stInput = Json::Value(Json::objectValue);
	stInput["c"] = 2;
	if (!stOriginal.FromJson(stInput) || stOriginal.iInt8 != 2 || stOriginal.sMessage != "unchanged" || stOriginal.vNumbers != std::vector<int32_t>{3, 4})
	{
		++iBad;
	}
	LOG_TRACE("hold json validation bad=%d", iBad);
	return iBad;
}

// 各宽度整数的 JSON 上下界。非法键、越界元素和坏对象解码失败，并且不改已有值。出现的字段才更新。
int JsonMgr::Run()
{
	return CheckJsonDecoding();
}
