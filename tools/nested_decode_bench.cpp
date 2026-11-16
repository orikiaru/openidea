// Nested/frame/flat decode benchmark: g++-13 -O2 -std=c++20 -DNDEBUG -Icbaselib/base/inc -Icbaselib/include tools/nested_decode_bench.cpp cbaselib/base/src/oi_bytes.cpp cbaselib/base/src/oi_bytes_stream.cpp -o nested_decode_bench (add -DOI_BENCH_READ_LIMIT for the new reader; -DOI_BENCH_SWITCH for switch dispatch).
#include "oi_rpc.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <string>
#include <vector>

using OI::Base::BytesStream;

struct Leaf : OI::Rpc::RpcData
{
	int32_t iA{};
	int64_t iB{};
	std::string sName;

	BytesStream& serialize(BytesStream& stOutput) const override
	{
		stOutput.WritePb(0, iA);
		stOutput.WritePb(1, iB);
		stOutput.WritePb(2, sName);
		return stOutput;
	}
	BytesStream& deserialize(BytesStream& stInput) override
	{
#ifdef OI_BENCH_SWITCH
		std::array<uint8_t, 3> vSeen{};
		while (stInput.RemainSize() > 0)
		{
			uint32_t iSequence = 0;
			uint32_t iSize = 0;
			stInput.ReadPbHeader(iSequence, iSize);
			switch (iSequence)
			{
			case 0:
				if (vSeen[0] != 0)
				{
					throw OI::Base::ParserException();
				}
				vSeen[0] = 1;
				stInput.ReadPb(iSize, iA);
				break;
			case 1:
				if (vSeen[1] != 0)
				{
					throw OI::Base::ParserException();
				}
				vSeen[1] = 1;
				stInput.ReadPb(iSize, iB);
				break;
			case 2:
				if (vSeen[2] != 0)
				{
					throw OI::Base::ParserException();
				}
				vSeen[2] = 1;
				stInput.ReadPb(iSize, sName);
				break;
			default:
				stInput.SkipPb(iSize);
				break;
			}
		}
		return stInput;
#else
		static constexpr std::array<OI::Base::PbField, 3> vFields = {{
		    OI_PB_FIELD(Leaf, 0, iA),
		    OI_PB_FIELD(Leaf, 1, iB),
		    OI_PB_FIELD(Leaf, 2, sName),
		}};
		std::array<uint8_t, 3> vSeen{};
		return stInput.ReadFields(this, vFields, vSeen);
#endif
	}
};

struct Mid : OI::Rpc::RpcData
{
	int32_t iId{};
	Leaf stLeaf;
	std::vector<Leaf> vLeaves;

	BytesStream& serialize(BytesStream& stOutput) const override
	{
		stOutput.WritePb(0, iId);
		stOutput.WritePb(1, stLeaf);
		stOutput.WritePb(2, vLeaves);
		return stOutput;
	}
	BytesStream& deserialize(BytesStream& stInput) override
	{
#ifdef OI_BENCH_SWITCH
		std::array<uint8_t, 3> vSeen{};
		while (stInput.RemainSize() > 0)
		{
			uint32_t iSequence = 0;
			uint32_t iSize = 0;
			stInput.ReadPbHeader(iSequence, iSize);
			switch (iSequence)
			{
			case 0:
				if (vSeen[0] != 0)
				{
					throw OI::Base::ParserException();
				}
				vSeen[0] = 1;
				stInput.ReadPb(iSize, iId);
				break;
			case 1:
				if (vSeen[1] != 0)
				{
					throw OI::Base::ParserException();
				}
				vSeen[1] = 1;
				stInput.ReadPb(iSize, stLeaf);
				break;
			case 2:
				if (vSeen[2] != 0)
				{
					throw OI::Base::ParserException();
				}
				vSeen[2] = 1;
				stInput.ReadPb(iSize, vLeaves);
				break;
			default:
				stInput.SkipPb(iSize);
				break;
			}
		}
		return stInput;
#else
		static constexpr std::array<OI::Base::PbField, 3> vFields = {{
		    OI_PB_FIELD(Mid, 0, iId),
		    OI_PB_FIELD(Mid, 1, stLeaf),
		    OI_PB_FIELD(Mid, 2, vLeaves),
		}};
		std::array<uint8_t, 3> vSeen{};
		return stInput.ReadFields(this, vFields, vSeen);
#endif
	}
};

struct Top : OI::Rpc::RpcData
{
	int32_t iId{};
	Mid stMid;
	std::vector<Mid> vMids;
	std::map<std::string, Leaf> mLeaves;
	OI::Base::Bytes stBlob;

	BytesStream& serialize(BytesStream& stOutput) const override
	{
		stOutput.WritePb(0, iId);
		stOutput.WritePb(1, stMid);
		stOutput.WritePb(2, vMids);
		stOutput.WritePb(3, mLeaves);
		stOutput.WritePb(4, stBlob);
		return stOutput;
	}
	BytesStream& deserialize(BytesStream& stInput) override
	{
#ifdef OI_BENCH_SWITCH
		std::array<uint8_t, 5> vSeen{};
		while (stInput.RemainSize() > 0)
		{
			uint32_t iSequence = 0;
			uint32_t iSize = 0;
			stInput.ReadPbHeader(iSequence, iSize);
			switch (iSequence)
			{
			case 0:
				if (vSeen[0] != 0)
				{
					throw OI::Base::ParserException();
				}
				vSeen[0] = 1;
				stInput.ReadPb(iSize, iId);
				break;
			case 1:
				if (vSeen[1] != 0)
				{
					throw OI::Base::ParserException();
				}
				vSeen[1] = 1;
				stInput.ReadPb(iSize, stMid);
				break;
			case 2:
				if (vSeen[2] != 0)
				{
					throw OI::Base::ParserException();
				}
				vSeen[2] = 1;
				stInput.ReadPb(iSize, vMids);
				break;
			case 3:
				if (vSeen[3] != 0)
				{
					throw OI::Base::ParserException();
				}
				vSeen[3] = 1;
				stInput.ReadPb(iSize, mLeaves);
				break;
			case 4:
				if (vSeen[4] != 0)
				{
					throw OI::Base::ParserException();
				}
				vSeen[4] = 1;
				stInput.ReadPb(iSize, stBlob);
				break;
			default:
				stInput.SkipPb(iSize);
				break;
			}
		}
		return stInput;
#else
		static constexpr std::array<OI::Base::PbField, 5> vFields = {{
		    OI_PB_FIELD(Top, 0, iId),
		    OI_PB_FIELD(Top, 1, stMid),
		    OI_PB_FIELD(Top, 2, vMids),
		    OI_PB_FIELD(Top, 3, mLeaves),
		    OI_PB_FIELD(Top, 4, stBlob),
		}};
		std::array<uint8_t, 5> vSeen{};
		return stInput.ReadFields(this, vFields, vSeen);
#endif
	}
};

#define FLAT_FIELDS(X) \
	X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) \
	X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) \
	X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) \
	X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31) \
	X(32) X(33) X(34) X(35) X(36) X(37) X(38) X(39) \
	X(40) X(41) X(42) X(43) X(44) X(45) X(46) X(47) \
	X(48) X(49) X(50) X(51) X(52) X(53) X(54) X(55) \
	X(56) X(57) X(58) X(59) X(60) X(61) X(62) X(63) \
	X(64) X(65) X(66) X(67) X(68) X(69) X(70) X(71) \
	X(72) X(73) X(74) X(75) X(76) X(77) X(78) X(79) \
	X(80) X(81) X(82) X(83) X(84) X(85) X(86) X(87) \
	X(88) X(89) X(90) X(91) X(92) X(93) X(94) X(95) \
	X(96) X(97) X(98) X(99) X(100) X(101) X(102) X(103) \
	X(104) X(105) X(106) X(107) X(108) X(109) X(110) X(111) \
	X(112) X(113) X(114) X(115) X(116) X(117) X(118) X(119) \
	X(120) X(121) X(122) X(123) X(124) X(125) X(126) X(127)
#define FLAT_DECLARE(N) int32_t iValue##N{};
#define FLAT_ENCODE(N) stOutput.WritePb(N, iValue##N);
#define FLAT_DECODE(N) OI_PB_FIELD(Flat, N, iValue##N),
#define FLAT_SWITCH(N) \
	case N: \
		if (vSeen[N] != 0) { throw OI::Base::ParserException(); } \
		vSeen[N] = 1; \
		stInput.ReadPb(iSize, iValue##N); \
		break;
#define FLAT_FILL(N) iValue##N = N + 1;
#define FLAT_COMPARE(N) if (iValue##N != stOther.iValue##N) return false;
struct Flat : OI::Rpc::RpcData
{
	FLAT_FIELDS(FLAT_DECLARE)

	BytesStream& serialize(BytesStream& stOutput) const override
	{
		FLAT_FIELDS(FLAT_ENCODE)
		return stOutput;
	}
	BytesStream& deserialize(BytesStream& stInput) override
	{
#ifdef OI_BENCH_SWITCH
		std::array<uint8_t, 128> vSeen{};
		while (stInput.RemainSize() > 0)
		{
			uint32_t iSequence = 0;
			uint32_t iSize = 0;
			stInput.ReadPbHeader(iSequence, iSize);
			switch (iSequence)
			{
				FLAT_FIELDS(FLAT_SWITCH)
			default:
				stInput.SkipPb(iSize);
				break;
			}
		}
		return stInput;
#else
		static constexpr std::array<OI::Base::PbField, 128> vFields = {{
		    FLAT_FIELDS(FLAT_DECODE)
		}};
		std::array<uint8_t, 128> vSeen{};
		return stInput.ReadFields(this, vFields, vSeen);
#endif
	}
	void Fill()
	{
		FLAT_FIELDS(FLAT_FILL)
	}
	bool Equals(const Flat& stOther) const
	{
		FLAT_FIELDS(FLAT_COMPARE)
		return true;
	}
};
#undef FLAT_COMPARE
#undef FLAT_FILL
#undef FLAT_DECODE
#undef FLAT_SWITCH
#undef FLAT_ENCODE
#undef FLAT_DECLARE
#undef FLAT_FIELDS

Leaf MakeLeaf(int32_t iSeed)
{
	Leaf stValue;
	stValue.iA = 100 + iSeed;
	stValue.iB = 10000000000LL + iSeed;
	stValue.sName = std::string(16, static_cast<char>('a' + iSeed % 26));
	return stValue;
}

Mid MakeMid(int32_t iSeed)
{
	Mid stValue;
	stValue.iId = 200 + iSeed;
	stValue.stLeaf = MakeLeaf(iSeed);
	for (int32_t iIndex = 0; iIndex < 8; ++iIndex)
	{
		stValue.vLeaves.push_back(MakeLeaf(iSeed * 8 + iIndex));
	}
	return stValue;
}

Top MakeTop()
{
	Top stValue;
	stValue.iId = 123;
	stValue.stMid = MakeMid(0);
	for (int32_t iIndex = 0; iIndex < 16; ++iIndex)
	{
		stValue.vMids.push_back(MakeMid(iIndex + 1));
		char vKey[4];
		std::snprintf(vKey, sizeof(vKey), "k%02d", iIndex);
		stValue.mLeaves.emplace(vKey, MakeLeaf(iIndex + 200));
	}
	const std::string sBlob(256, 'z');
	stValue.stBlob.Append(sBlob.data(), sBlob.size());
	return stValue;
}

bool EqualLeaf(const Leaf& stLeft, const Leaf& stRight)
{
	return stLeft.iA == stRight.iA && stLeft.iB == stRight.iB && stLeft.sName == stRight.sName;
}

bool EqualMid(const Mid& stLeft, const Mid& stRight)
{
	if (stLeft.iId != stRight.iId || !EqualLeaf(stLeft.stLeaf, stRight.stLeaf)
	    || stLeft.vLeaves.size() != stRight.vLeaves.size())
	{
		return false;
	}
	for (size_t iIndex = 0; iIndex < stLeft.vLeaves.size(); ++iIndex)
	{
		if (!EqualLeaf(stLeft.vLeaves[iIndex], stRight.vLeaves[iIndex]))
		{
			return false;
		}
	}
	return true;
}

bool EqualTop(const Top& stLeft, const Top& stRight)
{
	if (stLeft.iId != stRight.iId || !EqualMid(stLeft.stMid, stRight.stMid)
	    || stLeft.vMids.size() != stRight.vMids.size() || stLeft.mLeaves.size() != stRight.mLeaves.size()
	    || stLeft.stBlob.Size() != stRight.stBlob.Size()
	    || std::memcmp(stLeft.stBlob.Data(), stRight.stBlob.Data(), stLeft.stBlob.Size()) != 0)
	{
		return false;
	}
	for (size_t iIndex = 0; iIndex < stLeft.vMids.size(); ++iIndex)
	{
		if (!EqualMid(stLeft.vMids[iIndex], stRight.vMids[iIndex]))
		{
			return false;
		}
	}
	for (const auto& [sKey, stLeaf] : stLeft.mLeaves)
	{
		auto it = stRight.mLeaves.find(sKey);
		if (it == stRight.mLeaves.end() || !EqualLeaf(stLeaf, it->second))
		{
			return false;
		}
	}
	return true;
}

using Clock = std::chrono::steady_clock;

bool BenchNested()
{
	constexpr int iIterations = 240000;
	const Top stExpected = MakeTop();
	BytesStream stPacket;
	stExpected.serialize(stPacket);
	uint64_t iSum = 0;
	const auto stStart = Clock::now();
	for (int iIndex = 0; iIndex < iIterations; ++iIndex)
	{
		stPacket.StartTransaction();
		Top stDecoded;
		stDecoded.deserialize(stPacket);
		iSum += static_cast<uint64_t>(stDecoded.iId + stDecoded.stMid.iId + stDecoded.stMid.stLeaf.iA);
		stPacket.RollBack();
	}
	const auto stEnd = Clock::now();
	Top stCheck;
	stCheck.deserialize(stPacket);
	const uint64_t iExpectedSum = static_cast<uint64_t>(stExpected.iId + stExpected.stMid.iId + stExpected.stMid.stLeaf.iA) * iIterations;
	const bool bOk = iSum == iExpectedSum && stPacket.RemainSize() == 0 && EqualTop(stExpected, stCheck);
	const double dNs = std::chrono::duration<double, std::nano>(stEnd - stStart).count() / iIterations;
	std::printf("scenario=nested iterations=%d bytes=%zu ns=%.2f checksum=%llu ok=%d\n",
	    iIterations, stPacket.Size(), dNs, static_cast<unsigned long long>(iSum), bOk ? 1 : 0);
	return bOk;
}

bool DecodeFrame(BytesStream& stPacket, uint32_t& iId, uint64_t& iCallId, Mid& stDecoded)
{
	uint32_t iSize = 0;
	stPacket.ReadPbHeader(iId, iSize);
#ifdef OI_BENCH_READ_LIMIT
	BytesStream::ReadLimit stLimit(stPacket, iSize, true);
	stPacket >> iCallId;
	stDecoded.deserialize(stPacket);
	return stPacket.RemainSize() == 0;
#else
	BytesStream stBody;
	stBody.Append(stPacket.GetCurPtr(), iSize);
	stPacket.AddCurPos(iSize);
	stBody >> iCallId;
	stDecoded.deserialize(stBody);
	return stBody.RemainSize() == 0;
#endif
}

bool BenchFrames()
{
	constexpr int iFrames = 64;
	constexpr int iBatches = 100000;
	constexpr uint32_t iProtocol = 3001;
	std::array<Mid, iFrames> vExpected;
	BytesStream stPacket;
	uint64_t iBatchSum = 0;
	for (int iIndex = 0; iIndex < iFrames; ++iIndex)
	{
		vExpected[iIndex] = MakeMid(iIndex);
		BytesStream stBody;
		stBody << static_cast<uint64_t>(1000 + iIndex);
		vExpected[iIndex].serialize(stBody);
		stPacket.WritePb(iProtocol, static_cast<const OI::Base::Bytes&>(stBody));
		iBatchSum += static_cast<uint64_t>(1000 + iIndex + vExpected[iIndex].iId + vExpected[iIndex].stLeaf.iA);
	}
	stPacket.StartTransaction();
	uint64_t iSum = 0;
	bool bOk = true;
	const auto stStart = Clock::now();
	for (int iBatch = 0; iBatch < iBatches; ++iBatch)
	{
		for (int iIndex = 0; iIndex < iFrames; ++iIndex)
		{
			uint32_t iId = 0;
			uint64_t iCallId = 0;
			Mid stDecoded;
			const bool bFrameOk = DecodeFrame(stPacket, iId, iCallId, stDecoded);
			bOk = bOk && bFrameOk && iId == iProtocol;
			iSum += iCallId + static_cast<uint64_t>(stDecoded.iId + stDecoded.stLeaf.iA);
		}
		bOk = bOk && stPacket.RemainSize() == 0;
		stPacket.RollBack();
	}
	const auto stEnd = Clock::now();
	for (int iIndex = 0; iIndex < iFrames; ++iIndex)
	{
		uint32_t iId = 0;
		uint64_t iCallId = 0;
		Mid stDecoded;
		const bool bFrameOk = DecodeFrame(stPacket, iId, iCallId, stDecoded);
		bOk = bOk && bFrameOk && iId == iProtocol
		    && iCallId == static_cast<uint64_t>(1000 + iIndex) && EqualMid(vExpected[iIndex], stDecoded);
	}
	bOk = bOk && stPacket.RemainSize() == 0 && iSum == iBatchSum * iBatches;
	const int iCount = iBatches * iFrames;
	const double dNs = std::chrono::duration<double, std::nano>(stEnd - stStart).count() / iCount;
	std::printf("scenario=frames iterations=%d frames_per_batch=%d bytes=%zu ns=%.2f checksum=%llu ok=%d\n",
	    iCount, iFrames, stPacket.Size(), dNs, static_cast<unsigned long long>(iSum), bOk ? 1 : 0);
	return bOk;
}

bool BenchFlat()
{
	constexpr int iIterations = 4000000;
	Flat stExpected;
	stExpected.Fill();
	BytesStream stPacket;
	stExpected.serialize(stPacket);
	stPacket.StartTransaction();
	uint64_t iSum = 0;
	const auto stStart = Clock::now();
	for (int iIndex = 0; iIndex < iIterations; ++iIndex)
	{
		Flat stDecoded;
		stDecoded.deserialize(stPacket);
		iSum += static_cast<uint64_t>(stDecoded.iValue0 + stDecoded.iValue127);
		stPacket.RollBack();
	}
	const auto stEnd = Clock::now();
	Flat stCheck;
	stCheck.deserialize(stPacket);
	const bool bOk = iSum == uint64_t(129) * iIterations && stPacket.RemainSize() == 0
	    && stCheck.Equals(stExpected);
	const double dNs = std::chrono::duration<double, std::nano>(stEnd - stStart).count() / iIterations;
	std::printf("scenario=flat iterations=%d bytes=%zu ns=%.2f checksum=%llu ok=%d\n",
	    iIterations, stPacket.Size(), dNs, static_cast<unsigned long long>(iSum), bOk ? 1 : 0);
	return bOk;
}

int main()
{
	try
	{
		const bool bNested = BenchNested();
		const bool bFrames = BenchFrames();
		const bool bFlat = BenchFlat();
		return bNested && bFrames && bFlat ? 0 : 1;
	}
	catch (const std::exception& stError)
	{
		std::fprintf(stderr, "benchmark failed: %s\n", stError.what());
		return 2;
	}
}
