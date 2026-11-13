#pragma once
#include "oi_bytes.h"
#include <vector>
#include <array>
#include <span>
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

// PreparePbHeader 的结果。序号允许为负，框架协议号用负数。长度最高位为 1 是 BAD。
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

class BytesStream;

// 按字段号升序排列，大小只与实际字段数有关。
using PbReadFn = void (*)(BytesStream&, uint32_t, void*);
struct PbField
{
	uint32_t iSequence;
	PbReadFn fnRead;
};

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
		static_assert(!std::is_same_v<T, bool>, "vector<bool> is not a protocol container");
		uint32_t iCount = 0;
		(*this) >> iCount;
		// 变长值至少有四字节长度/数量头，不能按不可信数量先分配。
		constexpr size_t iMinElementSize = PbFixedTrait<T>::bValue ? sizeof(T) : sizeof(uint32_t);
		if (iCount > 0x7FFFFFFF || iCount > RemainSize() / iMinElementSize)
		{
			throw ParserException();
		}
		std::vector<T> vDecoded;
		for (uint32_t iIndex = 0; iIndex < iCount; ++iIndex)
		{
			T stElement{};
			(*this) >> stElement;
			vDecoded.push_back(std::move(stElement));
		}
		stValueLocal = std::move(vDecoded);
		return *this;
	}
	template <typename T>
	BytesStream& operator<<(const std::vector<T>& stValueLocal)
	{
		static_assert(!std::is_same_v<T, bool>, "vector<bool> is not a protocol container");
		if (stValueLocal.size() > 0x7FFFFFFF)
		{
			throw ParserException();
		}
		(*this) << static_cast<uint32_t>(stValueLocal.size());
		for (const auto& stValue : stValueLocal)
		{
			(*this) << stValue;
		}
		return *this;
	}
	template <typename K, typename V>
	BytesStream& operator>>(std::map<K, V>& stValueLocal)
	{
		uint32_t iCount = 0;
		(*this) >> iCount;
		constexpr size_t iMinKeySize = PbFixedTrait<K>::bValue ? sizeof(K) : sizeof(uint32_t);
		constexpr size_t iMinValueSize = PbFixedTrait<V>::bValue ? sizeof(V) : sizeof(uint32_t);
		if (iCount > 0x7FFFFFFF || iCount > RemainSize() / (iMinKeySize + iMinValueSize))
		{
			throw ParserException();
		}
		std::map<K, V> mDecoded;
		for (uint32_t iIndex = 0; iIndex < iCount; ++iIndex)
		{
			K stKey{};
			V stValue{};
			(*this) >> stKey >> stValue;
			if (!mDecoded.emplace(std::move(stKey), std::move(stValue)).second)
			{
				throw ParserException();
			}
		}
		stValueLocal = std::move(mDecoded);
		return *this;
	}
	template <typename K, typename V>
	BytesStream& operator<<(const std::map<K, V>& stValueLocal)
	{
		if (stValueLocal.size() > 0x7FFFFFFF)
		{
			throw ParserException();
		}
		(*this) << static_cast<uint32_t>(stValueLocal.size());
		for (const auto& [stKey, stValue] : stValueLocal)
		{
			(*this) << stKey << stValue;
		}
		return *this;
	}

	// 整包：协议id + totalsize + (seqid + size + data) 重复。
	// totalsize 是后面所有 seqid+size+data 的总字节数。
	// seqid 从 0 起，按字段声明顺序对应；允许缺号、乱序。
	// 没出现的 seqid 保持字段原值，不认识的 seqid 按 size 跳过。
	// 每个字段只占一条；vector/map 的字段体是数量加元素，嵌套值自行编码。
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
		BytesStream stPayload;
		stPayload << stValueLocal;
		return WritePb(iSequence, static_cast<const Bytes&>(stPayload));
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
		ReadPbContainer(iSize, stValueLocal);
	}
	template <typename K, typename V>
	BytesStream& WritePb(uint32_t iSequence, const std::map<K, V>& stValueLocal)
	{
		BytesStream stPayload;
		stPayload << stValueLocal;
		return WritePb(iSequence, static_cast<const Bytes&>(stPayload));
	}
	template <typename K, typename V>
	void ReadPb(uint32_t iSize, std::map<K, V>& stValueLocal)
	{
		ReadPbContainer(iSize, stValueLocal);
	}

	BytesStream& ReadFields(void* pObject, std::span<const PbField> vFields, std::span<uint8_t> vSeen);
	// mFields 按字段号索引，一个进程一份。vSeen 用字段号当下标，长度要盖住最大字段号。
	BytesStream& ReadFieldsByMap(void* pObject, const std::map<uint32_t, PbReadFn>& mFields, std::span<uint8_t> vSeen);

  private:
	template <typename T>
	void ReadPbContainer(uint32_t iSize, T& stValueLocal)
	{
		if (RemainSize() < iSize)
		{
			throw ParserException();
		}
		BytesStream stPayload(GetCurPtr(), iSize);
		T stDecoded;
		stPayload >> stDecoded;
		if (stPayload.RemainSize() != 0)
		{
			throw ParserException();
		}
		stValueLocal = std::move(stDecoded);
		m_iCurrentPosition += iSize;
	}
};
}
}

// 表项直接生成对应成员的解码函数，不引入成员指针模板实例。
#define OI_PB_FIELD(Type, Sequence, Member)                                             \
	::OI::Base::PbField                                                                 \
	{                                                                                   \
		Sequence, +[](::OI::Base::BytesStream& stStream, uint32_t iSize, void* pObject) \
		{                                                                               \
			stStream.ReadPb(iSize, static_cast<Type*>(pObject)->Member);                \
		}                                                                               \
	}

// 和 OI_PB_FIELD 同一条读函数，收进按字段号索引的 map。没人调用就不会实例化到调用方里。
#define OI_PB_MAP_FIELD(Type, Sequence, Member)                                         \
	{                                                                                   \
		Sequence, +[](::OI::Base::BytesStream& stStream, uint32_t iSize, void* pObject) \
		{                                                                               \
			stStream.ReadPb(iSize, static_cast<Type*>(pObject)->Member);                \
		}                                                                               \
	}
