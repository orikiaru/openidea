#pragma once
#include "oi_bytes_stream.h"
#include "oi_db.h"
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>
#include <type_traits>
#include <utility>

namespace OI::Db
{
// 数值列按 MySQL 以文本取回的十进制写回。空文本、空白、正号和尾随字符都拒绝。
template <typename T>
bool FromDbInteger(const Blob& stBlob, T& stOutput)
{
	static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
	if (stBlob.empty())
	{
		return false;
	}
	const char* pBegin = reinterpret_cast<const char*>(stBlob.data());
	const char* pEnd = pBegin + stBlob.size();
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

inline bool FromDbBool(const Blob& stBlob, bool& bOutput)
{
	if (stBlob.size() == 1 && stBlob[0] == static_cast<uint8_t>('0'))
	{
		bOutput = false;
		return true;
	}
	if (stBlob.size() == 1 && stBlob[0] == static_cast<uint8_t>('1'))
	{
		bOutput = true;
		return true;
	}
	return false;
}

inline bool ParseDbDouble(const Blob& stBlob, double& dOutput)
{
	if (stBlob.empty())
	{
		return false;
	}
	const char* pBegin = reinterpret_cast<const char*>(stBlob.data());
	const char* pEnd = pBegin + stBlob.size();
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
bool FromDbReal(const Blob& stBlob, T& stOutput)
{
	static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>);
	double dValue = 0;
	if (!ParseDbDouble(stBlob, dValue))
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

inline bool FromDbText(const Blob& stBlob, std::string& sOutput)
{
	if (stBlob.empty())
	{
		sOutput.clear();
		return true;
	}
	sOutput.assign(reinterpret_cast<const char*>(stBlob.data()), stBlob.size());
	return true;
}

inline bool FromDbText(const Blob& stBlob, ::OI::Base::Bytes& stOutput)
{
	stOutput = ::OI::Base::Bytes(stBlob.empty() ? nullptr : reinterpret_cast<const char*>(stBlob.data()), stBlob.size());
	return true;
}

// 头一层的 vector、map、rpcdata 不拆列。blob 是这条值自己的 PB：序号固定为 0。
// vector 每个元素一条，空 vector 是空 blob；map 和 rpcdata 是一整条。
template <typename T>
Blob ToDbBlob(const T& stValue)
{
	::OI::Base::BytesStream stStream;
	stStream.WritePb(0, stValue);
	if (stStream.Size() == 0)
	{
		return Blob();
	}
	const auto* pData = reinterpret_cast<const uint8_t*>(stStream.Data());
	return Blob(pData, pData + stStream.Size());
}

template <typename T>
bool FromDbBlob(const Blob& stBlob, T& stOutput)
{
	try
	{
		::OI::Base::BytesStream stStream(stBlob.empty() ? nullptr : reinterpret_cast<const char*>(stBlob.data()), stBlob.size());
		T stDecoded{};
		while (stStream.RemainSize() > 0)
		{
			uint32_t iSequence = 0;
			uint32_t iSize = 0;
			stStream.ReadPbHeader(iSequence, iSize);
			if (iSequence != 0)
			{
				return false;
			}
			stStream.ReadPb(iSize, stDecoded);
		}
		stOutput = std::move(stDecoded);
		return true;
	}
	catch (const ::OI::Base::ParserException&)
	{
		return false;
	}
}
}
