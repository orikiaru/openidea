#include "binary_mgr.h"
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
#include "oi_rpcdata_alltypesdata.h"
#include "oi_rpcdata_allechores.h"
#include "oi_rpcdata_holdreq.h"
#include "oi_rpcdata_holdres.h"
#include "oi_db_row.h"
#include <map>
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

int BinaryMgr::CheckBinaryBool()
{
	int iBad = 0;
	for (int iMode = 0; iMode < 2; ++iMode)
	{
		for (unsigned iByte = 0; iByte < 256; ++iByte)
		{
			const unsigned char iRaw = static_cast<unsigned char>(iByte);
			OI::Base::BytesStream stInput(reinterpret_cast<const char*>(&iRaw), 1);
			bool bValue = true;
			bool bRejected = false;
			try
			{
				if (iMode == 0)
				{
					stInput >> bValue;
				}
				else
				{
					stInput.ReadPb(1, bValue);
				}
			}
			catch (const OI::Base::ParserException&)
			{
				bRejected = true;
			}
			if (iByte <= 1)
			{
				if (bRejected || bValue != (iByte != 0) || stInput.GetCurPos() != 1)
				{
					++iBad;
				}
			}
			else if (!bRejected || !bValue || stInput.GetCurPos() != 0)
			{
				++iBad;
			}
		}
	}
	LOG_TRACE("hold binary bool bad=%d", iBad);
	return iBad;
}

// clang-format off
int BinaryMgr::CheckBinaryContainers()
{
	int iBad = 0;
	auto fnCheck = [pBad = &iBad](bool bPassed, const char* pLabel)
	{
		if (!bPassed)
		{
			LOG_TRACE("hold binary container failed: %s", pLabel);
			++*pBad;
		}
	};
	try
	{
		const std::vector<int32_t> vNumbers = {1, -2};
		OI::Base::BytesStream stWire;
		stWire.WritePb(7, vNumbers);
		stWire.WritePb(8, int32_t{42});
		const unsigned char vExpected[] = {7, 0, 0, 0, 12, 0, 0, 0, 2, 0, 0, 0,
			1, 0, 0, 0, 254, 255, 255, 255, 8, 0, 0, 0, 4, 0, 0, 0, 42, 0, 0, 0};
		fnCheck(stWire.Size() == sizeof(vExpected) && std::memcmp(stWire.Data(), vExpected, sizeof(vExpected)) == 0, "vector golden field and sibling");
		uint32_t iSequence = 0;
		uint32_t iSize = 0;
		std::vector<int32_t> vDecoded = {99};
		stWire.ReadPbHeader(iSequence, iSize);
		stWire.ReadPb(iSize, vDecoded);
		int32_t iSibling = 0;
		stWire.ReadPbHeader(iSequence, iSize);
		stWire.ReadPb(iSize, iSibling);
		fnCheck(vDecoded == vNumbers && iSequence == 8 && iSibling == 42 && stWire.RemainSize() == 0, "vector decode and sibling");

		OI::Base::BytesStream stEmpty;
		stEmpty.WritePb(1, std::vector<int32_t>{});
		stEmpty.WritePb(2, std::map<std::string, int32_t>{});
		const unsigned char vEmptyExpected[] = {1, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0,
			2, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0};
		fnCheck(stEmpty.Size() == sizeof(vEmptyExpected)
			&& std::memcmp(stEmpty.Data(), vEmptyExpected, sizeof(vEmptyExpected)) == 0, "empty vector map golden");
		std::map<std::string, int32_t> mEmpty = {{"old", 1}};
		stEmpty.ReadPbHeader(iSequence, iSize);
		stEmpty.ReadPb(iSize, vDecoded);
		stEmpty.ReadPbHeader(iSequence, iSize);
		stEmpty.ReadPb(iSize, mEmpty);
		fnCheck(vDecoded.empty() && mEmpty.empty() && stEmpty.RemainSize() == 0, "empty containers replace");

		const std::map<std::string, int32_t> mNumbers = {{"a", 5}, {"b", 6}};
		OI::Base::BytesStream stMap;
		stMap.WritePb(3, mNumbers);
		const unsigned char vMapExpected[] = {3, 0, 0, 0, 22, 0, 0, 0, 2, 0, 0, 0,
			1, 0, 0, 0, 'a', 5, 0, 0, 0, 1, 0, 0, 0, 'b', 6, 0, 0, 0};
		fnCheck(stMap.Size() == sizeof(vMapExpected) && std::memcmp(stMap.Data(), vMapExpected, sizeof(vMapExpected)) == 0, "map golden");
		std::map<std::string, int32_t> mDecoded = {{"old", 7}};
		stMap.ReadPbHeader(iSequence, iSize);
		stMap.ReadPb(iSize, mDecoded);
		fnCheck(mDecoded == mNumbers && stMap.RemainSize() == 0, "map replacement");

		OI::RpcData::HoldReq stFirst;
		stFirst.iMark = 17;
		stFirst.iDelayMilliseconds = 3;
		OI::RpcData::HoldReq stSecond;
		stSecond.iMark = 29;
		const std::vector<OI::RpcData::HoldReq> vObjects = {stFirst, stSecond};
		const std::vector<std::vector<int32_t>> vNested = {{1, 2}, {}, {3}};
		const std::map<std::string, std::vector<OI::RpcData::HoldReq>> mNested = {{"a", vObjects}, {"empty", {}}};
		OI::Base::BytesStream stNested;
		stNested.WritePb(4, vObjects);
		stNested.WritePb(5, vNested);
		stNested.WritePb(6, mNested);
		stNested.WritePb(7, int32_t{71});
		std::vector<OI::RpcData::HoldReq> vObjectOut;
		std::vector<std::vector<int32_t>> vNestedOut;
		std::map<std::string, std::vector<OI::RpcData::HoldReq>> mNestedOut;
		stNested.ReadPbHeader(iSequence, iSize);
		stNested.ReadPb(iSize, vObjectOut);
		stNested.ReadPbHeader(iSequence, iSize);
		stNested.ReadPb(iSize, vNestedOut);
		stNested.ReadPbHeader(iSequence, iSize);
		stNested.ReadPb(iSize, mNestedOut);
		stNested.ReadPbHeader(iSequence, iSize);
		stNested.ReadPb(iSize, iSibling);
		fnCheck(vObjectOut.size() == 2 && vObjectOut[0].iMark == 17 && vObjectOut[1].iMark == 29
			&& vNestedOut == vNested && mNestedOut.size() == 2 && mNestedOut.at("a").size() == 2
			&& mNestedOut.at("a")[1].iMark == 29 && mNestedOut.at("empty").empty()
			&& iSequence == 7 && iSibling == 71 && stNested.RemainSize() == 0, "nested containers and sibling");
		OI::Base::BytesStream stObjects;
		stObjects << stFirst << stSecond;
		OI::RpcData::HoldReq stFirstOut;
		OI::RpcData::HoldReq stSecondOut;
		stObjects >> stFirstOut >> stSecondOut;
		fnCheck(stFirstOut.iMark == 17 && stFirstOut.iDelayMilliseconds == 3
			&& stSecondOut.iMark == 29 && stObjects.RemainSize() == 0, "consecutive streamed objects");

		auto fnBadVector = [fnCheck](const unsigned char* pData, size_t iLength, const char* pLabel)
		{
			OI::Base::BytesStream stInput(reinterpret_cast<const char*>(pData), iLength);
			std::vector<int32_t> vOutput = {77};
			bool bRejected = false;
			try
			{
				stInput.ReadPb(static_cast<uint32_t>(iLength), vOutput);
			}
			catch (const OI::Base::ParserException&)
			{
				bRejected = true;
			}
			fnCheck(bRejected && vOutput == std::vector<int32_t>{77}, pLabel);
		};
		const unsigned char vHugeCount[] = {255, 255, 255, 255};
		const unsigned char vTruncated[] = {2, 0, 0, 0, 1, 0, 0, 0};
		const unsigned char vTrailing[] = {1, 0, 0, 0, 1, 0, 0, 0, 9};
		fnBadVector(vHugeCount, sizeof(vHugeCount), "huge count preserves vector");
		fnBadVector(vTruncated, sizeof(vTruncated), "truncated vector preserves output");
		fnBadVector(vTrailing, sizeof(vTrailing), "trailing vector bytes preserve output");
		auto fnBadMap = [fnCheck](const unsigned char* pData, size_t iLength, const char* pLabel)
		{
			OI::Base::BytesStream stInput(reinterpret_cast<const char*>(pData), iLength);
			std::map<std::string, int32_t> mOutput = {{"old", 7}};
			bool bRejected = false;
			try
			{
				stInput.ReadPb(static_cast<uint32_t>(iLength), mOutput);
			}
			catch (const OI::Base::ParserException&)
			{
				bRejected = true;
			}
			fnCheck(bRejected && mOutput == std::map<std::string, int32_t>{{"old", 7}}, pLabel);
		};
		const unsigned char vMapCut[] = {1, 0, 0, 0, 2, 0, 0, 0, 'a'};
		fnBadMap(vHugeCount, sizeof(vHugeCount), "huge map count preserves output");
		fnBadMap(vMapCut, sizeof(vMapCut), "truncated map preserves output");
		OI::Base::BytesStream stObjectPayload;
		stObjectPayload << uint32_t{1} << uint32_t{20} << uint32_t{0};
		std::vector<OI::RpcData::HoldReq> vPreserved = {stFirst};
		bool bObjectRejected = false;
		try
		{
			stObjectPayload.ReadPb(static_cast<uint32_t>(stObjectPayload.Size()), vPreserved);
		}
		catch (const OI::Base::ParserException&)
		{
			bObjectRejected = true;
		}
		fnCheck(bObjectRejected && vPreserved.size() == 1 && vPreserved[0].iMark == 17,
			"truncated nested object preserves vector");
		OI::RpcData::AllTypesData stMissingFields;
		stMissingFields.vNumbers = {77};
		stMissingFields.mTable = {{3, "old"}};
		OI::Base::BytesStream stPartial;
		stPartial.WritePb(20, int32_t{44});
		stMissingFields.deserialize(stPartial);
		fnCheck(stMissingFields.vNumbers == std::vector<int32_t>{77}
			&& stMissingFields.mTable == std::map<int32_t, std::string>{{3, "old"}}
			&& stMissingFields.iGap == 44, "missing generated fields preserve containers");
		OI::Base::BytesStream stOutOfOrder;
		stOutOfOrder.WritePb(20, int32_t{44});
		stOutOfOrder.WritePb(99, int32_t{123});
		stOutOfOrder.WritePb(9, std::vector<int32_t>{4, 5});
		OI::RpcData::AllTypesData stOutOfOrderData;
		stOutOfOrderData.deserialize(stOutOfOrder);
		fnCheck(stOutOfOrderData.iGap == 44 && stOutOfOrderData.vNumbers == std::vector<int32_t>({4, 5})
			&& stOutOfOrder.RemainSize() == 0, "out of order and unknown fields");
		auto fnDuplicateField = [fnCheck](uint32_t iField, const char* pLabel)
		{
			OI::Base::BytesStream stInput;
			if (iField == 9)
			{
				stInput.WritePb(iField, std::vector<int32_t>{1});
				stInput.WritePb(iField, std::vector<int32_t>{2});
			}
			else
			{
				stInput.WritePb(iField, int32_t{1});
				stInput.WritePb(iField, int32_t{2});
			}
			OI::RpcData::AllTypesData stOutput;
			bool bRejected = false;
			try
			{
				stOutput.deserialize(stInput);
			}
			catch (const OI::Base::ParserException&)
			{
				bRejected = true;
			}
			fnCheck(bRejected, pLabel);
		};
		fnDuplicateField(9, "duplicate vector field rejected");
		fnDuplicateField(20, "duplicate scalar field rejected");
		const unsigned char vDuplicate[] = {2, 0, 0, 0, 1, 0, 0, 0, 'a', 1, 0, 0, 0, 1, 0, 0, 0, 'a', 2, 0, 0, 0};
		OI::Base::BytesStream stDuplicate(reinterpret_cast<const char*>(vDuplicate), sizeof(vDuplicate));
		mDecoded = {{"old", 7}};
		bool bDuplicateRejected = false;
		try
		{
			stDuplicate.ReadPb(sizeof(vDuplicate), mDecoded);
		}
		catch (const OI::Base::ParserException&)
		{
			bDuplicateRejected = true;
		}
		fnCheck(bDuplicateRejected && mDecoded == std::map<std::string, int32_t>{{"old", 7}}, "duplicate map key preserves output");

		const auto stBlob = OI::Db::ToDbBlob(vNumbers);
		const unsigned char vBlobExpected[] = {0, 0, 0, 0, 12, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 254, 255, 255, 255};
		fnCheck(stBlob.size() == sizeof(vBlobExpected) && std::memcmp(stBlob.data(), vBlobExpected, sizeof(vBlobExpected)) == 0, "database blob golden");
		vDecoded = {77};
		fnCheck(OI::Db::FromDbBlob(stBlob, vDecoded) && vDecoded == vNumbers, "database blob roundtrip");
		auto stTrailingBlob = stBlob;
		stTrailingBlob.push_back(0);
		vDecoded = {77};
		fnCheck(!OI::Db::FromDbBlob(stTrailingBlob, vDecoded) && vDecoded == std::vector<int32_t>{77}, "database trailing blob preserves output");
		vDecoded = {77};
		fnCheck(!OI::Db::FromDbBlob(OI::Db::Blob{}, vDecoded) && vDecoded == std::vector<int32_t>{77}, "database empty blob rejected");
		auto stMultipleBlob = stBlob;
		stMultipleBlob.insert(stMultipleBlob.end(), stBlob.begin(), stBlob.end());
		vDecoded = {77};
		fnCheck(!OI::Db::FromDbBlob(stMultipleBlob, vDecoded) && vDecoded == std::vector<int32_t>{77}, "database multiple fields rejected");
		const auto stEmptyBlob = OI::Db::ToDbBlob(std::vector<int32_t>{});
		const unsigned char vEmptyBlobExpected[] = {0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0};
		fnCheck(stEmptyBlob.size() == sizeof(vEmptyBlobExpected)
			&& std::memcmp(stEmptyBlob.data(), vEmptyBlobExpected, sizeof(vEmptyBlobExpected)) == 0,
			"database empty vector golden");
	}
	catch (const std::exception& stException)
	{
		LOG_TRACE("hold binary container exception: %s", stException.what());
		++iBad;
	}
	LOG_TRACE("hold binary container bad=%d", iBad);
	return iBad;
}

// clang-format on
// 布尔和容器的二进制格式检查。
int BinaryMgr::Run()
{
	return CheckBinaryBool() + CheckBinaryContainers();
}
