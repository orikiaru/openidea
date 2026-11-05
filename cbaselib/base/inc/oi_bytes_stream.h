#pragma once
#include "oi_bytes.h"
#include <vector>
#include <list>
#include <map>
#include <string>
#include <string.h>
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

class BytesStream : public Bytes
{
	size_t m_iCurrentPosition;  //反序列化时使用
	size_t m_iSnapshotPosition; //快照位置
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

	void StartTransaction()
	{
		m_iSnapshotPosition = m_iCurrentPosition;
	}
	void RollBack()
	{
		m_iCurrentPosition = m_iSnapshotPosition;
	}

	template <typename T>
	typename std::enable_if<PbFixedTrait<T>::bValue, BytesStream&>::type
	operator<<(const T& stValueLocal)
	{
		Append(&stValueLocal, sizeof(T));
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
			memcpy(&stValueLocal, GetCurPtr(), sizeof(T));
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
	// vector 的每个元素单独一条，seqid 相同。均为 uint32 小端。
	BytesStream& WritePbRaw(uint32_t iSequence, const void* pData, uint32_t iSize);
	PbHeaderState PreparePbHeader(uint32_t& iSequence, uint32_t& iSize);
	void ReadPbHeader(uint32_t& iSequence, uint32_t& iSize);
	void SkipPb(uint32_t iSize);

	template <typename T>
	typename std::enable_if<PbFixedTrait<T>::bValue, BytesStream&>::type
	WritePb(uint32_t iSequence, const T& stValueLocal)
	{
		return WritePbRaw(iSequence, &stValueLocal, (uint32_t)sizeof(T));
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
			memcpy(&stValueLocal, GetCurPtr(), sizeof(T));
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
