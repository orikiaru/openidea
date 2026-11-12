#pragma once
#include "oi_bytes.h"
#include <vector>
#include <list>
#include <map>
#include <string>
#include <string.h>
#include <algorithm>
#include <bit>
#include <exception>
#include <type_traits>
#include <utility>

namespace OI
{
namespace Rpc
{
class Serialize;
}
}
namespace OI
{
namespace Base
{
// 解码字节不够或类型写不进去。调用方用它丢掉这一包。
struct ParserException : public std::exception
{
	const char* what() const noexcept override
	{
		return "ParserException";
	}
};

// PreparePbHeader 的结果。BAD 表示 seq/size 最高位为 1（与原先有符号 int 头不兼容）。
enum PbHeaderState
{
	PB_HEADER_OK = 0,
	PB_HEADER_NEED_MORE,
	PB_HEADER_BAD,
};

template <typename T>
struct PbFixedTrait
{
	static const bool bValue = std::is_arithmetic<T>::value || std::is_enum<T>::value;
};

static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big, "mixed-endian hosts are not supported");

// 序列化统一小端：小端主机原样拷贝，大端主机翻转字节。
template <typename T>
void StoreLittleEndian(void* pDestination, const T& stValue)
{
	static_assert(PbFixedTrait<T>::bValue);
	memcpy(pDestination, &stValue, sizeof(T));
	if constexpr (std::endian::native == std::endian::big)
	{
		auto* pBytes = static_cast<unsigned char*>(pDestination);
		std::reverse(pBytes, pBytes + sizeof(T));
	}
}

template <typename T>
T LoadLittleEndian(const void* pSource)
{
	static_assert(PbFixedTrait<T>::bValue);
	unsigned char vBytes[sizeof(T)];
	memcpy(vBytes, pSource, sizeof(T));
	if constexpr (std::endian::native == std::endian::big)
	{
		std::reverse(vBytes, vBytes + sizeof(T));
	}
	T stValue;
	memcpy(&stValue, vBytes, sizeof(T));
	return stValue;
}

// 带读游标的字节流。编码往末尾追加，解码从游标往前读，字节不够抛 ParserException。
class BytesStream : public Bytes
{
	// 下一次读取的起点。
	size_t m_iCurrentPosition;
	// StartTransaction 记下的游标。RollBack 回到这里。
	size_t m_iSnapshotPosition;
  public:
	BytesStream()
	    : m_iCurrentPosition(0), m_iSnapshotPosition(0)
	{
	}
	BytesStream(const Bytes& stBytes)
	    : Bytes(stBytes), m_iCurrentPosition(0), m_iSnapshotPosition(0)
	{
	}
	BytesStream(const char* pData, size_t iDataLength)
	    : Bytes(pData, iDataLength), m_iCurrentPosition(0), m_iSnapshotPosition(0)
	{
	}
	void Clear()
	{
		Bytes::Clear();
		m_iCurrentPosition = 0;
		m_iSnapshotPosition = 0;
	}
	Bytes& GetMutableBuff()
	{
		return *this;
	}
	const Bytes& GetUnMutableBuff() const
	{
		return *this;
	}
	size_t GetCurPos() const
	{
		return m_iCurrentPosition;
	}
	void AddCurPos(size_t iOffset)
	{
		m_iCurrentPosition += iOffset;
	}
	char* GetCurPtr()
	{
		return &(Begin()[m_iCurrentPosition]);
	}
	size_t RemainSize() const
	{
		return Size() - m_iCurrentPosition;
	}

	// 记下当前读游标，半包解析失败时可以退回去。
	void StartTransaction()
	{
		m_iSnapshotPosition = m_iCurrentPosition;
	}
	// 读游标退回最近一次 StartTransaction。
	void RollBack()
	{
		m_iCurrentPosition = m_iSnapshotPosition;
	}

	template <typename T>
	typename std::enable_if<PbFixedTrait<T>::bValue, BytesStream&>::type
	operator<<(const T& stValueLocal)
	{
		unsigned char vBytes[sizeof(T)];
		StoreLittleEndian(vBytes, stValueLocal);
		Append(vBytes, sizeof(T));
		return *this;
	}
	template <typename T>
	typename std::enable_if<PbFixedTrait<T>::bValue, BytesStream&>::type
	operator>>(T& stValueLocal)
	{
		if (RemainSize() < sizeof(T))
		{
			throw ParserException();
		}
		if constexpr (std::is_same_v<T, bool>)
		{
			const auto iValue = static_cast<unsigned char>(*GetCurPtr());
			if (iValue > 1)
			{
				throw ParserException();
			}
			stValueLocal = iValue != 0;
		}
		else
		{
			stValueLocal = LoadLittleEndian<T>(GetCurPtr());
		}
		m_iCurrentPosition += sizeof(T);
		return *this;
	}

	BytesStream& operator<<(const std::string& sFirst);
	BytesStream& operator<<(const Bytes& stFirst);
	BytesStream& operator<<(const BytesStream& stFirst); //全量写入，内容本身已经是序列化结果

	BytesStream& operator<<(const ::OI::Rpc::Serialize& stFirst);

	BytesStream& operator>>(std::string& sFirst);
	BytesStream& operator>>(Bytes& stFirst);

	BytesStream& operator>>(::OI::Rpc::Serialize& stFirst);

	template <typename T>
	BytesStream& operator>>(std::vector<T>& stValueLocal)
	{
		uint32_t iSize;
		(*this) >> iSize;
		// 个数是对端给的。先按剩余字节估一下，免得按一个巨大的数先分配。
		const size_t iMinElem = PbFixedTrait<T>::bValue ? sizeof(T) : 1;
		if ((size_t)iSize > RemainSize() / iMinElem)
		{
			throw ParserException();
		}
		stValueLocal.resize(iSize);
		for (uint32_t iIndexLocal = 0; iIndexLocal < iSize; ++iIndexLocal)
		{
			(*this) >> stValueLocal[iIndexLocal];
		}
		return *this;
	}
	template <typename T>
	BytesStream& operator<<(const std::vector<T>& stValueLocal)
	{
		uint32_t iSize = stValueLocal.size();
		(*this) << iSize;
		for (const auto& stValue : stValueLocal)
		{
			(*this) << stValue;
		}
		return *this;
	}

	// 整包：协议id + totalsize + (seqid + size + data) 重复。
	// totalsize 是后面所有 seqid+size+data 的总字节数。
	// seqid 从 0 起，按字段声明顺序对应；允许缺号、乱序。
	// 没出现的 seqid 保持字段原值，不认识的 seqid 按 size 跳过。
	// vector 的每个元素单独一条，seqid 相同。
	// 字段头、长度前缀、定宽整数、枚举、float、double 都是小端，不看主机字节序。
	BytesStream& WritePbRaw(uint32_t iSequence, const void* pData, uint32_t iSize);
	PbHeaderState PreparePbHeader(uint32_t& iSequence, uint32_t& iSize);
	void ReadPbHeader(uint32_t& iSequence, uint32_t& iSize);
	void SkipPb(uint32_t iSize);

	template <typename T>
	typename std::enable_if<PbFixedTrait<T>::bValue, BytesStream&>::type
	WritePb(uint32_t iSequence, const T& stValueLocal)
	{
		unsigned char vBytes[sizeof(T)];
		StoreLittleEndian(vBytes, stValueLocal);
		return WritePbRaw(iSequence, vBytes, (uint32_t)sizeof(T));
	}
	BytesStream& WritePb(uint32_t iSequence, const std::string& sValue);
	BytesStream& WritePb(uint32_t iSequence, const Bytes& stValueLocal);
	BytesStream& WritePb(uint32_t iSequence, const ::OI::Rpc::Serialize& stValueLocal);
	template <typename T>
	BytesStream& WritePb(uint32_t iSequence, const std::vector<T>& stValueLocal)
	{
		for (const auto& stValue : stValueLocal)
		{
			WritePb(iSequence, stValue);
		}
		return *this;
	}

	template <typename T>
	typename std::enable_if<PbFixedTrait<T>::bValue, void>::type
	ReadPb(uint32_t iSize, T& stValueLocal)
	{
		if (iSize != (uint32_t)sizeof(T) || RemainSize() < iSize)
		{
			throw ParserException();
		}
		if constexpr (std::is_same_v<T, bool>)
		{
			const auto iValue = static_cast<unsigned char>(*GetCurPtr());
			if (iValue > 1)
			{
				throw ParserException();
			}
			stValueLocal = iValue != 0;
		}
		else
		{
			stValueLocal = LoadLittleEndian<T>(GetCurPtr());
		}
		m_iCurrentPosition += iSize;
	}
	void ReadPb(uint32_t iSize, std::string& sValue);
	void ReadPb(uint32_t iSize, Bytes& stValueLocal);
	void ReadPb(uint32_t iSize, ::OI::Rpc::Serialize& stValueLocal);
	template <typename T>
	void ReadPb(uint32_t iSize, std::vector<T>& stValueLocal)
	{
		T stElem{};
		ReadPb(iSize, stElem);
		stValueLocal.push_back(std::move(stElem));
	}
	template <typename K, typename V>
	BytesStream& WritePb(uint32_t iSequence, const std::map<K, V>& stValueLocal)
	{
		BytesStream stAll;
		for (const auto& [stKey, stValue] : stValueLocal)
		{
			BytesStream stPair;
			stPair.WritePb(0, stKey);
			stPair.WritePb(1, stValue);
			uint32_t iPairSize = (uint32_t)stPair.Size();
			stAll.WritePbRaw(0, iPairSize ? stPair.Begin() : NULL, iPairSize);
		}
		uint32_t iSize = (uint32_t)stAll.Size();
		return WritePbRaw(iSequence, iSize ? stAll.Begin() : NULL, iSize);
	}
	template <typename K, typename V>
	void ReadPb(uint32_t iSize, std::map<K, V>& stValueLocal)
	{
		if (RemainSize() < iSize)
		{
			throw ParserException();
		}
		BytesStream stAll;
		if (iSize > 0)
		{
			stAll.Append(GetCurPtr(), iSize);
		}
		m_iCurrentPosition += iSize;
		stValueLocal.clear();
		while (stAll.RemainSize() > 0)
		{
			uint32_t iPairSequence = 0;
			uint32_t iPairSize = 0;
			stAll.ReadPbHeader(iPairSequence, iPairSize);
			BytesStream stPair;
			if (iPairSize > 0)
			{
				stPair.Append(stAll.GetCurPtr(), iPairSize);
			}
			stAll.AddCurPos(iPairSize);
			K stKey;
			V stVal;
			bool bGotKey = false;
			bool bGotVal = false;
			while (stPair.RemainSize() > 0)
			{
				uint32_t iFieldSequence = 0;
				uint32_t iFieldSize = 0;
				stPair.ReadPbHeader(iFieldSequence, iFieldSize);
				if (iFieldSequence == 0)
				{
					stPair.ReadPb(iFieldSize, stKey);
					bGotKey = true;
				}
				else if (iFieldSequence == 1)
				{
					stPair.ReadPb(iFieldSize, stVal);
					bGotVal = true;
				}
				else
				{
					stPair.SkipPb(iFieldSize);
				}
			}
			if (!bGotKey || !bGotVal)
			{
				throw ParserException();
			}
			stValueLocal.insert(std::make_pair(stKey, stVal));
		}
	}
};
}
}
