#include "oi_tcp_channel.h"
#include "oi_net_session.h"
#include "oi_net_os.h"
#include <uv.h>
#include "test_binary.h"
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
#include "framework_protocol/oi_rpcdata_regpullres.h"
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
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

int TestBinary::CheckBinaryBool()
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

namespace
{
class ReceiveCapacityProbe : public OI::Net::NetSession
{
  public:
	std::vector<size_t> vCapacities;
	std::vector<size_t> vSizes;
	std::vector<std::string> vReceived;
	ReceiveCapacityProbe()
	    : NetSession(nullptr)
	{
	}
	void OnDataIn() override
	{
		vCapacities.push_back(stInputBuffer.Capacity());
		vSizes.push_back(stInputBuffer.Size());
		vReceived.emplace_back(stInputBuffer.Data(), stInputBuffer.Size());
		stInputBuffer.Clear();
	}
	void OnDataOut() override
	{
	}
};
}

// clang-format off
int TestBinary::CheckBinaryContainers()
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
		uv_os_sock_t vSockets[2];
		const int iPairCode = uv_socketpair(SOCK_STREAM, 0, vSockets, 0, 0);
		fnCheck(iPairCode == 0, "tcp capacity socket pair");
		if (iPairCode == 0)
		{
			ReceiveCapacityProbe stProbe;
			OI::Net::NetSocket stSocket;
			stSocket.Attach(vSockets[0]);
			{
				OI::Net::TCP_Channel stChannel(stSocket, &stProbe);
				const char iFirst = 'x';
				const int iFirstSent = NetOs_Send(vSockets[1], &iFirst, 1);
				stChannel.DataIn();
				const std::string sLarger(8192, 'y');
				const int iLargerSent = NetOs_Send(vSockets[1], sLarger.data(), sLarger.size());
				stChannel.DataIn();
				fnCheck(iFirstSent == 1 && iLargerSent == static_cast<int>(sLarger.size())
					&& stProbe.vSizes.size() == 2 && stProbe.vSizes[0] == 1 && stProbe.vSizes[1] == sLarger.size()
					&& stProbe.vReceived[0] == "x" && stProbe.vReceived[1] == sLarger
					&& stProbe.vCapacities[0] == 4096 && stProbe.vCapacities[1] >= sLarger.size()
					&& stProbe.vCapacities[1] <= 1024 * 1024, "tcp first read capacity and growth");
			}
			NetOs_Close(vSockets[1]);
		}
		uv_os_sock_t vLimitedSockets[2];
		const int iLimitedPairCode = uv_socketpair(SOCK_STREAM, 0, vLimitedSockets, 0, 0);
		fnCheck(iLimitedPairCode == 0, "tcp limited socket pair");
		if (iLimitedPairCode == 0)
		{
			ReceiveCapacityProbe stLimitedProbe;
			stLimitedProbe.SetReceiveQueueLimit(6000);
			OI::Net::NetSocket stLimitedSocket;
			stLimitedSocket.Attach(vLimitedSockets[0]);
			{
				OI::Net::TCP_Channel stLimitedChannel(stLimitedSocket, &stLimitedProbe);
				const std::string sLimited(5000, 'z');
				const int iSent = NetOs_Send(vLimitedSockets[1], sLimited.data(), sLimited.size());
				stLimitedChannel.DataIn();
				fnCheck(iSent == static_cast<int>(sLimited.size()) && stLimitedProbe.vReceived.size() == 1
					&& stLimitedProbe.vReceived[0] == sLimited && stLimitedProbe.vCapacities[0] >= sLimited.size()
					&& stLimitedProbe.vCapacities[0] <= 6000, "tcp non-power-of-two capacity limit");
			}
			NetOs_Close(vLimitedSockets[1]);
		}
		{
			constexpr size_t iCapacityLimit = 6000;
			constexpr size_t iErasedBytes = 1000;
			std::string sOriginal(iCapacityLimit, '\0');
			for (size_t iIndex = 0; iIndex < sOriginal.size(); ++iIndex)
			{
				sOriginal[iIndex] = static_cast<char>('a' + iIndex % 26);
			}
			const std::string sSuffix(iErasedBytes, '!');
			const std::string sExpected = sOriginal.substr(iErasedBytes) + sSuffix;
			OI::Base::Bytes stForward("abcdef", 6);
			const size_t iForward = stForward.Replace(2, stForward.Data(), 4);
			fnCheck(iForward == 4 && stForward.Size() == 6 && std::memcmp(stForward.Data(), "ababcd", 6) == 0,
				"bytes replace overlapping source towards end");
			OI::Base::Bytes stBackward("abcdef", 6);
			const size_t iBackward = stBackward.Replace(0, stBackward.Data() + 2, 4);
			fnCheck(iBackward == 4 && stBackward.Size() == 6 && std::memcmp(stBackward.Data(), "cdefef", 6) == 0,
				"bytes replace overlapping source towards beginning");
			OI::Base::Bytes stLimitedBytes;
			stLimitedBytes.Reserve(iCapacityLimit, iCapacityLimit);
			stLimitedBytes.Append(sOriginal.data(), sOriginal.size());
			fnCheck(stLimitedBytes.Size() == iCapacityLimit && stLimitedBytes.Capacity() == iCapacityLimit,
				"bytes limited reserve reaches non-power-of-two limit");
			const size_t iErased = stLimitedBytes.Erase(size_t{0}, iErasedBytes);
			fnCheck(iErased == iErasedBytes && stLimitedBytes.Size() == iCapacityLimit - iErasedBytes
				&& stLimitedBytes.Capacity() == iCapacityLimit - iErasedBytes,
				"bytes prefix erase leaves bounded tail capacity");
			// The erased prefix is smaller than the live suffix: the limit must force compaction, not growth.
			const size_t iReserved = stLimitedBytes.Reserve(iCapacityLimit, iCapacityLimit);
			stLimitedBytes.Append(sSuffix.data(), sSuffix.size());
			fnCheck(iReserved == iCapacityLimit && stLimitedBytes.Capacity() == iCapacityLimit
				&& stLimitedBytes.Size() == sExpected.size()
				&& std::memcmp(stLimitedBytes.Data(), sExpected.data(), sExpected.size()) == 0,
				"bytes limited reserve compacts prefix and preserves appended content");
			bool bCapacityRejected = false;
			try
			{
				stLimitedBytes.Reserve(iCapacityLimit + 1, iCapacityLimit);
			}
			catch (const std::length_error&)
			{
				bCapacityRejected = true;
			}
			fnCheck(bCapacityRejected && stLimitedBytes.Capacity() == iCapacityLimit
				&& stLimitedBytes.Size() == sExpected.size()
				&& std::memcmp(stLimitedBytes.Data(), sExpected.data(), sExpected.size()) == 0,
				"bytes reserve one byte over limit preserves capacity and content");
		}
		for (int iLegacy = 0; iLegacy < 3; ++iLegacy)
		{
			OI::Base::BytesStream stLegacy;
			stLegacy.WritePb(0, std::vector<OI::RpcData::RegInstance>{});
			if (iLegacy != 0)
			{
				stLegacy.WritePb(1, iLegacy == 2);
			}
			OI::RpcData::RegPullRes stLegacyResponse;
			stLegacyResponse.deserialize(stLegacy);
			fnCheck(!stLegacyResponse.bStatic && stLegacyResponse.vList.empty() && stLegacy.RemainSize() == 0,
				"registry legacy and old report field default to reporting");
		}
		OI::RpcData::RegPullRes stStaticResponse;
		stStaticResponse.bStatic = true;
		OI::Base::BytesStream stStaticWire;
		stStaticResponse.serialize(stStaticWire);
		uint32_t iStaticSequence = 0;
		uint32_t iStaticSize = 0;
		stStaticWire.ReadPbHeader(iStaticSequence, iStaticSize);
		stStaticWire.SkipPb(iStaticSize);
		stStaticWire.ReadPbHeader(iStaticSequence, iStaticSize);
		bool bStatic = false;
		stStaticWire.ReadPb(iStaticSize, bStatic);
		fnCheck(iStaticSequence == 2 && bStatic && stStaticWire.RemainSize() == 0, "registry static field 2 wire");
		OI::Base::BytesStream stStaticInput;
		stStaticInput.WritePb(2, true);
		OI::RpcData::RegPullRes stStaticDecoded;
		stStaticDecoded.deserialize(stStaticInput);
		fnCheck(stStaticDecoded.bStatic && stStaticInput.RemainSize() == 0, "registry static decode");

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

		const unsigned char vNestedGolden[] = {
			11,0,0,0, 21,0,0,0, 0,0,0,0, 4,0,0,0, 7,0,0,0, 1,0,0,0, 1,0,0,0, 'x',
			12,0,0,0, 29,0,0,0, 1,0,0,0, 21,0,0,0, 0,0,0,0, 4,0,0,0, 7,0,0,0, 1,0,0,0, 1,0,0,0, 'x',
			14,0,0,0, 34,0,0,0, 1,0,0,0, 1,0,0,0, 'k', 21,0,0,0, 0,0,0,0, 4,0,0,0, 7,0,0,0, 1,0,0,0, 1,0,0,0, 'x'};
		OI::Base::BytesStream stGolden(reinterpret_cast<const char*>(vNestedGolden), sizeof(vNestedGolden));
		OI::RpcData::AllTypesData stGoldenData;
		stGoldenData.deserialize(stGolden);
		fnCheck(stGolden.RemainSize() == 0 && stGoldenData.stChild.iNumber == 7 && stGoldenData.stChild.sText == "x"
			&& stGoldenData.vChildren.size() == 1 && stGoldenData.vChildren[0].iNumber == 7
			&& stGoldenData.vChildren[0].sText == "x" && stGoldenData.mNamed.size() == 1
			&& stGoldenData.mNamed.at("k").iNumber == 7 && stGoldenData.mNamed.at("k").sText == "x",
			"nested object vector map golden decode");

		OI::Base::BytesStream stBrokenNested;
		stBrokenNested << uint32_t{0} << uint32_t{4} << uint8_t{1};
		OI::Base::BytesStream stBrokenBody;
		stBrokenBody.WritePb(11, static_cast<const OI::Base::Bytes&>(stBrokenNested));
		OI::Base::BytesStream stFrames;
		stFrames.WritePb(1, static_cast<const OI::Base::Bytes&>(stBrokenBody));
		stFrames.WritePb(2, int32_t{42});
		stFrames.ReadPbHeader(iSequence, iSize);
		bool bNestedRejected = false;
		{
			OI::Base::BytesStream::ReadLimit stFrameLimit(stFrames, iSize, true);
			try
			{
				OI::RpcData::AllTypesData stBrokenData;
				stBrokenData.deserialize(stFrames);
			}
			catch (const OI::Base::ParserException&)
			{
				bNestedRejected = true;
			}
			fnCheck(stFrames.RemainSize() == stBrokenNested.Size(), "nested exception restores outer frame limit");
		}
		stFrames.ReadPbHeader(iSequence, iSize);
		stFrames.ReadPb(iSize, iSibling);
		fnCheck(bNestedRejected && iSequence == 2 && iSibling == 42 && stFrames.RemainSize() == 0,
			"nested exception skips to next frame");

		OI::Base::BytesStream stPartialFrames;
		stPartialFrames.WritePb(1, uint64_t{123});
		stPartialFrames.WritePb(2, int32_t{42});
		stPartialFrames.ReadPbHeader(iSequence, iSize);
		{
			OI::Base::BytesStream::ReadLimit stFrameLimit(stPartialFrames, iSize, true);
			uint32_t iPart = 0;
			stPartialFrames >> iPart;
			fnCheck(iPart == 123 && stPartialFrames.RemainSize() == 4, "partial frame read");
		}
		stPartialFrames.ReadPbHeader(iSequence, iSize);
		stPartialFrames.ReadPb(iSize, iSibling);
		fnCheck(iSequence == 2 && iSibling == 42 && stPartialFrames.RemainSize() == 0,
			"partial frame handler skips to next frame");
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

		// Budget checks use valid, small payloads; only the explicit budget makes them fail.
		OI::Base::BytesStream stBudgetWire;
		stBudgetWire << std::vector<int32_t>{1, 2};
		OI::Base::BytesStream stBudgetLow(stBudgetWire, sizeof(int32_t));
		std::vector<int32_t> vBudgetOutput = {77};
		bool bBudgetRejected = false;
		try
		{
			stBudgetLow >> vBudgetOutput;
		}
		catch (const OI::Base::ParserException&)
		{
			bBudgetRejected = true;
		}
		fnCheck(bBudgetRejected && vBudgetOutput == std::vector<int32_t>{77}, "vector budget preserves output");

		OI::Base::BytesStream stBudgetMapWire;
		stBudgetMapWire << std::map<int32_t, int32_t>{{1, 2}};
		OI::Base::BytesStream stBudgetMap(stBudgetMapWire, sizeof(std::map<int32_t, int32_t>::value_type));
		std::map<int32_t, int32_t> mBudgetOutput = {{7, 8}};
		bBudgetRejected = false;
		try
		{
			stBudgetMap >> mBudgetOutput;
		}
		catch (const OI::Base::ParserException&)
		{
			bBudgetRejected = true;
		}
		fnCheck(bBudgetRejected && mBudgetOutput == std::map<int32_t, int32_t>{{7, 8}}, "map budget preserves output");

		OI::Base::BytesStream stBudgetNestedWire;
		stBudgetNestedWire << std::vector<std::vector<int32_t>>{{1, 2}};
		OI::Base::BytesStream stBudgetNested(stBudgetNestedWire, sizeof(std::vector<int32_t>) + sizeof(int32_t));
		std::vector<std::vector<int32_t>> vBudgetNested = {{77}};
		bBudgetRejected = false;
		try
		{
			stBudgetNested >> vBudgetNested;
		}
		catch (const OI::Base::ParserException&)
		{
			bBudgetRejected = true;
		}
		fnCheck(bBudgetRejected && vBudgetNested == std::vector<std::vector<int32_t>>{{77}}, "nested budget shared and output preserved");

		OI::Base::BytesStream stBudgetStringsWire;
		stBudgetStringsWire << std::vector<std::string>{"ab", "cd"};
		OI::Base::BytesStream stBudgetStrings(stBudgetStringsWire, 2 * sizeof(std::string) + 3);
		std::vector<std::string> vBudgetStrings = {"old"};
		bBudgetRejected = false;
		try
		{
			stBudgetStrings >> vBudgetStrings;
		}
		catch (const OI::Base::ParserException&)
		{
			bBudgetRejected = true;
		}
		fnCheck(bBudgetRejected && vBudgetStrings == std::vector<std::string>{"old"}, "string content shares budget");

		OI::Base::BytesStream stBudgetFieldsWire;
		stBudgetFieldsWire.WritePb(1, std::vector<int32_t>{1, 2});
		stBudgetFieldsWire.WritePb(2, std::vector<int32_t>{3, 4});
		OI::Base::BytesStream stBudgetFields(stBudgetFieldsWire, 3 * sizeof(int32_t));
		std::vector<int32_t> vBudgetFirst;
		std::vector<int32_t> vBudgetSecond = {77};
		stBudgetFields.ReadPbHeader(iSequence, iSize);
		stBudgetFields.ReadPb(iSize, vBudgetFirst);
		stBudgetFields.ReadPbHeader(iSequence, iSize);
		bBudgetRejected = false;
		try
		{
			stBudgetFields.ReadPb(iSize, vBudgetSecond);
		}
		catch (const OI::Base::ParserException&)
		{
			bBudgetRejected = true;
		}
		fnCheck(vBudgetFirst == std::vector<int32_t>({1, 2}) && bBudgetRejected
			&& vBudgetSecond == std::vector<int32_t>{77}, "sibling fields share budget");

		OI::Base::BytesStream stBudgetRetry(stBudgetWire, 2 * sizeof(int32_t));
		stBudgetRetry.StartTransaction();
		stBudgetRetry >> vBudgetOutput;
		stBudgetRetry.RollBack();
		vBudgetOutput = {77};
		stBudgetRetry >> vBudgetOutput;
		fnCheck(vBudgetOutput == std::vector<int32_t>({1, 2}) && stBudgetRetry.RemainSize() == 0,
			"rollback restores budget for retry");

		OI::Base::BytesStream stBudgetFramesWire;
		stBudgetFramesWire << std::vector<int32_t>{1, 2} << std::vector<int32_t>{3, 4};
		OI::Base::BytesStream stBudgetFrames(stBudgetFramesWire, 2 * sizeof(int32_t));
		stBudgetFrames.StartReadFrame();
		stBudgetFrames >> vBudgetFirst;
		stBudgetFrames.StartReadFrame();
		stBudgetFrames >> vBudgetSecond;
		fnCheck(vBudgetFirst == std::vector<int32_t>({1, 2}) && vBudgetSecond == std::vector<int32_t>({3, 4})
			&& stBudgetFrames.RemainSize() == 0, "independent frames reset budget");
		stBudgetFrames.Clear();
		stBudgetFrames << std::vector<int32_t>{5, 6};
		stBudgetFrames >> vBudgetOutput;
		fnCheck(vBudgetOutput == std::vector<int32_t>({5, 6}), "clear resets budget");
		stBudgetFrames.Clear();
		stBudgetFrames << std::vector<int32_t>{5, 6, 7};
		vBudgetOutput = {77};
		bBudgetRejected = false;
		try
		{
			stBudgetFrames >> vBudgetOutput;
		}
		catch (const OI::Base::ParserException&)
		{
			bBudgetRejected = true;
		}
		fnCheck(bBudgetRejected && vBudgetOutput == std::vector<int32_t>{77}, "clear retains configured budget");

		const std::vector<int32_t> vMillion(1000000, 42);
		OI::Base::BytesStream stMillionWire;
		stMillionWire << vMillion;
		OI::Base::BytesStream stMillionInput(stMillionWire);
		std::vector<int32_t> vMillionOutput;
		stMillionInput >> vMillionOutput;
		fnCheck(vMillionOutput == vMillion && stMillionInput.RemainSize() == 0, "million int32 default budget");

		{
			constexpr size_t iDefaultBudgetBytes = 64 * 1024 * 1024;
			constexpr uint32_t iBudgetObjectCount = static_cast<uint32_t>(iDefaultBudgetBytes / sizeof(OI::RpcData::AllTypesData));
			constexpr size_t iAmplifiedWireBytes = 16 * 1024 * 1024;
			constexpr uint32_t iAmplifiedObjectCount = static_cast<uint32_t>(iAmplifiedWireBytes / sizeof(uint32_t) - 1);
			fnCheck(iBudgetObjectCount > 0 && iAmplifiedObjectCount > iBudgetObjectCount
				&& static_cast<size_t>(iBudgetObjectCount) * sizeof(OI::RpcData::AllTypesData) <= iDefaultBudgetBytes
				&& (static_cast<size_t>(iBudgetObjectCount) + 1) * sizeof(OI::RpcData::AllTypesData) > iDefaultBudgetBytes,
				"rpcdata default budget boundary calculation");
			auto fnEmptyObjectBudget = [fnCheck](OI::Base::BytesStream& stInput, uint32_t iCount, bool bExpectRejected, const char* pLabel)
			{
				std::vector<OI::RpcData::AllTypesData> vOutput(1);
				vOutput[0].iGap = -77;
				vOutput[0].sMessage = "old";
				vOutput[0].vNumbers = {77};
				bool bRejected = false;
				try
				{
					stInput >> vOutput;
				}
				catch (const OI::Base::ParserException&)
				{
					bRejected = true;
				}
				if (bExpectRejected)
				{
					fnCheck(bRejected && stInput.GetCurPos() == sizeof(uint32_t)
						&& stInput.RemainSize() == static_cast<size_t>(iCount) * sizeof(uint32_t)
						&& vOutput.size() == 1 && vOutput[0].iGap == -77 && vOutput[0].sMessage == "old"
						&& vOutput[0].vNumbers == std::vector<int32_t>{77}, pLabel);
				}
				else
				{
					bool bDefaultObjects = true;
					for (const auto& stObject : vOutput)
					{
						bDefaultObjects = bDefaultObjects && stObject.iGap == 0 && stObject.sMessage.empty()
							&& stObject.vNumbers.empty() && stObject.stRawData.Size() == 0;
					}
					fnCheck(!bRejected && vOutput.size() == iCount && bDefaultObjects && stInput.RemainSize() == 0, pLabel);
				}
			};
			// The same generated type accepts empty bodies at its exact logical object cost, but not one byte less.
			constexpr uint32_t iSmallObjectCount = 2;
			char vSmallWire[(iSmallObjectCount + 1) * sizeof(uint32_t)]{};
			OI::Base::StoreLittleEndian(vSmallWire, iSmallObjectCount);
			constexpr size_t iSmallBudgetBytes = iSmallObjectCount * sizeof(OI::RpcData::AllTypesData);
			OI::Base::BytesStream stExactBudget(vSmallWire, sizeof(vSmallWire), iSmallBudgetBytes);
			fnEmptyObjectBudget(stExactBudget, iSmallObjectCount, false, "rpcdata empty bodies exact object budget decode");
			OI::Base::BytesStream stShortBudget(vSmallWire, sizeof(vSmallWire), iSmallBudgetBytes - 1);
			fnEmptyObjectBudget(stShortBudget, iSmallObjectCount, true, "rpcdata empty bodies one byte short preserves output");

			// Count plus exactly count zero body lengths: no illegal length or truncated body can cause this rejection.
			// Keep the default constructor budget; never enlarge it to create gigabytes of generated objects.
			std::string sEmptyBodies(iAmplifiedWireBytes, '\0');
			OI::Base::StoreLittleEndian(sEmptyBodies.data(), iAmplifiedObjectCount);
			OI::Base::BytesStream stAmplifiedInput(sEmptyBodies.data(), sEmptyBodies.size());
			fnEmptyObjectBudget(stAmplifiedInput, iAmplifiedObjectCount, true,
				"rpcdata 16 MiB empty bodies reject default amplification and preserve output");
		}

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
int TestBinary::Run()
{
	return CheckBinaryBool() + CheckBinaryContainers();
}
