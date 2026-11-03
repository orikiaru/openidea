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

namespace OI { namespace Rpc { class Serialize; } }
namespace OI { namespace Base {
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

template<typename T>
struct PbFixedTrait
{
	static const bool bValue = std::is_arithmetic<T>::value || std::is_enum<T>::value;
};

class BytesStream :public Bytes
{
	size_t 	m_iCurrentPosition;	//反序列化时使用
	size_t  m_iSnapshotPosition;	//快照位置
public:
	BytesStream() :m_iCurrentPosition(0), m_iSnapshotPosition(0) {}
	BytesStream(const Bytes& stBytesParam) :Bytes(stBytesParam), m_iCurrentPosition(0), m_iSnapshotPosition(0) {}
	BytesStream(const char* pDataParam, size_t iDataLengthParam) :Bytes(pDataParam, iDataLengthParam), m_iCurrentPosition(0), m_iSnapshotPosition(0) {}
	void Clear()
	{
		Bytes::Clear();
		m_iCurrentPosition = 0;
		m_iSnapshotPosition = 0;
	}
	Bytes& GetMutableBuff() { return *this; }
	const Bytes& GetUnMutableBuff()const { return *this; }
	size_t	GetCurPos()const { return m_iCurrentPosition; }
	void 	AddCurPos(size_t iOffsetParam) { m_iCurrentPosition += iOffsetParam; }
	char*	GetCurPtr() { return &(Begin()[m_iCurrentPosition]); }
	size_t	RemainSize()const { return Size() - m_iCurrentPosition; }

	void	StartTransaction() { m_iSnapshotPosition = m_iCurrentPosition; }
	void  	RollBack() { m_iCurrentPosition = m_iSnapshotPosition; }

	template<typename T>
	typename std::enable_if<PbFixedTrait<T>::bValue, BytesStream&>::type
	operator<< (const T& stValueParam)
	{
		Append(&stValueParam, sizeof(T));
		return *this;
	}
	template<typename T>
	typename std::enable_if<PbFixedTrait<T>::bValue, BytesStream&>::type
	operator>> (T& stValueParam)
	{
		if (RemainSize() < sizeof(T))
		{
			throw ParserException();
		}
		if constexpr (std::is_same_v<T, bool>)
		{
			const auto iValue = static_cast<unsigned char>(*GetCurPtr());
			if (iValue > 1) throw ParserException();
			stValueParam = iValue != 0;
		}
		else
		{
			memcpy(&stValueParam, GetCurPtr(), sizeof(T));
		}
		m_iCurrentPosition += sizeof(T);
		return *this;
	}

	BytesStream& operator<< (const std::string& sFirstParam);
	BytesStream& operator<< (const Bytes& stFirstParam);
	BytesStream& operator<< (const BytesStream& stFirstParam);	//全量写入，内容本身已经是序列化结果

	BytesStream& operator<< (const ::OI::Rpc::Serialize& stFirstParam);

	BytesStream& operator>> (std::string& sFirstParam);
	BytesStream& operator>> (Bytes& stFirstParam);

	BytesStream& operator>> (::OI::Rpc::Serialize& stFirstParam);

	template<typename T>
	BytesStream& operator>> (std::vector<T>& stValueParam)
	{
		uint32_t iSize;
		(*this) >> iSize;
		// 个数是对端给的。先按剩余字节估一下，免得按一个巨大的数先分配。
		const size_t iMinElem = PbFixedTrait<T>::bValue ? sizeof(T) : 1;
		if ((size_t)iSize > RemainSize() / iMinElem)
		{
			throw ParserException();
		}
		stValueParam.resize(iSize);
		for (uint32_t iIndexLocal = 0; iIndexLocal < iSize; ++iIndexLocal)
		{
			(*this) >> stValueParam[iIndexLocal];
		}
		return *this;
	}
	template<typename T>
	BytesStream& operator << (const std::vector<T>& stValueParam)
	{
		uint32_t iSize = stValueParam.size();
		(*this) << iSize;
		for (const auto& stValue : stValueParam)
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
	BytesStream& WritePbRaw(uint32_t iSequenceParam, const void* pDataParam, uint32_t iSizeParam);
	PbHeaderState PreparePbHeader(uint32_t& iSequenceParam, uint32_t& iSizeParam);
	void ReadPbHeader(uint32_t& iSequenceParam, uint32_t& iSizeParam);
	void SkipPb(uint32_t iSizeParam);

	template<typename T>
	typename std::enable_if<PbFixedTrait<T>::bValue, BytesStream&>::type
	WritePb(uint32_t iSequenceParam, const T& stValueParam)
	{
		return WritePbRaw(iSequenceParam, &stValueParam, (uint32_t)sizeof(T));
	}
	BytesStream& WritePb(uint32_t iSequenceParam, const std::string& sValueParam);
	BytesStream& WritePb(uint32_t iSequenceParam, const Bytes& stValueParam);
	BytesStream& WritePb(uint32_t iSequenceParam, const ::OI::Rpc::Serialize& stValueParam);
	template<typename T>
	BytesStream& WritePb(uint32_t iSequenceParam, const std::vector<T>& stValueParam)
	{
		for (const auto& stValue : stValueParam)
		{
			WritePb(iSequenceParam, stValue);
		}
		return *this;
	}

	template<typename T>
	typename std::enable_if<PbFixedTrait<T>::bValue, void>::type
	ReadPb(uint32_t iSizeParam, T& stValueParam)
	{
		if (iSizeParam != (uint32_t)sizeof(T) || RemainSize() < iSizeParam)
		{
			throw ParserException();
		}
		if constexpr (std::is_same_v<T, bool>)
		{
			const auto iValue = static_cast<unsigned char>(*GetCurPtr());
			if (iValue > 1) throw ParserException();
			stValueParam = iValue != 0;
		}
		else
		{
			memcpy(&stValueParam, GetCurPtr(), sizeof(T));
		}
		m_iCurrentPosition += iSizeParam;
	}
	void ReadPb(uint32_t iSizeParam, std::string& sValueParam);
	void ReadPb(uint32_t iSizeParam, Bytes& stValueParam);
	void ReadPb(uint32_t iSizeParam, ::OI::Rpc::Serialize& stValueParam);
	template<typename T>
	void ReadPb(uint32_t iSizeParam, std::vector<T>& stValueParam)
	{
		T stElem{};
		ReadPb(iSizeParam, stElem);
		stValueParam.push_back(std::move(stElem));
	}
	template<typename K, typename V>
	BytesStream& WritePb(uint32_t iSequenceParam, const std::map<K, V>& stValueParam)
	{
		BytesStream stAll;
		for (const auto& [stKey, stValue] : stValueParam)
		{
			BytesStream stPair;
			stPair.WritePb(0, stKey);
			stPair.WritePb(1, stValue);
			uint32_t iPairSize = (uint32_t)stPair.Size();
			stAll.WritePbRaw(0, iPairSize ? stPair.Begin() : NULL, iPairSize);
		}
		uint32_t iSize = (uint32_t)stAll.Size();
		return WritePbRaw(iSequenceParam, iSize ? stAll.Begin() : NULL, iSize);
	}
	template<typename K, typename V>
	void ReadPb(uint32_t iSizeParam, std::map<K, V>& stValueParam)
	{
		if (RemainSize() < iSizeParam)
		{
			throw ParserException();
		}
		BytesStream stAll;
		if (iSizeParam > 0)
		{
			stAll.Append(GetCurPtr(), iSizeParam);
		}
		m_iCurrentPosition += iSizeParam;
		stValueParam.clear();
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
			stValueParam.insert(std::make_pair(stKey, stVal));
		}
	}


};
}}
