#include "oi_bytes_stream.h"
#include <string>
#include <string.h>
#include "oi_rpc.h"

namespace OI
{
namespace Base
{
//暂不处理大小端问题，默认小端

// 长度前缀是 4 字节，最高位为 1 的长度对端按非法处理。
static void WriteLenPrefixed(BytesStream& stOutputStreamParam, const void* pDataParam, size_t iSizeParam)
{
	if (iSizeParam > (size_t)0x7FFFFFFF)
	{
		throw ParserException();
	}
	stOutputStreamParam << (int32_t)iSizeParam;
	if (iSizeParam > 0)
	{
		stOutputStreamParam.Append(pDataParam, iSizeParam);
	}
}

static size_t ReadLenPrefix(BytesStream& stOutputStreamParam)
{
	int32_t iSize;
	stOutputStreamParam >> iSize;
	if (iSize < 0 || stOutputStreamParam.RemainSize() < (size_t)iSize)
	{
		throw ParserException();
	}
	return (size_t)iSize;
}

BytesStream& BytesStream::operator<<(const std::string& sFirstParam)
{
	WriteLenPrefixed(*this, sFirstParam.data(), sFirstParam.size());
	return *this;
}
BytesStream& BytesStream::operator>>(std::string& sFirstParam)
{
	size_t iSize = ReadLenPrefix(*this);
	sFirstParam.assign(GetCurPtr(), iSize);
	m_iCurrentPosition += iSize;
	return *this;
}

BytesStream& BytesStream::operator<<(const Bytes& stFirstParam)
{
	WriteLenPrefixed(*this, stFirstParam.Data(), stFirstParam.Size());
	return *this;
}
// 覆盖 a 原有内容，和 ReadPb 一致。
BytesStream& BytesStream::operator>>(Bytes& stFirstParam)
{
	size_t iSize = ReadLenPrefix(*this);
	stFirstParam.Clear();
	stFirstParam.Append(GetCurPtr(), iSize);
	m_iCurrentPosition += iSize;
	return *this;
}

BytesStream& BytesStream::operator<<(const BytesStream& stFirstParam)
{
	Append(stFirstParam);
	return *this;
}

BytesStream& BytesStream::operator<<(const ::OI::Rpc::Serialize& stFirstParam)
{
	stFirstParam.serialize(*this);
	return *this;
}

BytesStream& BytesStream::operator>>(::OI::Rpc::Serialize& stFirstParam)
{
	stFirstParam.deserialize(*this);
	return *this;
}

BytesStream& BytesStream::WritePbRaw(uint32_t iSequenceParam, const void* pDataParam, uint32_t iSizeParam)
{
	if ((int32_t)iSequenceParam < 0 || (int32_t)iSizeParam < 0)
	{
		throw ParserException();
	}
	Append(&iSequenceParam, sizeof(iSequenceParam));
	Append(&iSizeParam, sizeof(iSizeParam));
	if (iSizeParam > 0)
	{
		if (!pDataParam)
		{
			throw ParserException();
		}
		Append(pDataParam, iSizeParam);
	}
	return *this;
}

PbHeaderState BytesStream::PreparePbHeader(uint32_t& iSequenceParam, uint32_t& iSizeParam)
{
	if (RemainSize() < sizeof(uint32_t) * 2)
	{
		return PB_HEADER_NEED_MORE;
	}
	uint32_t iSequenceValue = 0;
	uint32_t iSizeValue = 0;
	memcpy(&iSequenceValue, GetCurPtr(), sizeof(iSequenceValue));
	memcpy(&iSizeValue, GetCurPtr() + sizeof(iSequenceValue), sizeof(iSizeValue));
	if ((int32_t)iSequenceValue < 0 || (int32_t)iSizeValue < 0)
	{
		return PB_HEADER_BAD;
	}
	if (RemainSize() < sizeof(uint32_t) * 2 + (size_t)iSizeValue)
	{
		return PB_HEADER_NEED_MORE;
	}
	m_iCurrentPosition += sizeof(uint32_t) * 2;
	iSequenceParam = iSequenceValue;
	iSizeParam = iSizeValue;
	return PB_HEADER_OK;
}

void BytesStream::ReadPbHeader(uint32_t& iSequenceParam, uint32_t& iSizeParam)
{
	if (PreparePbHeader(iSequenceParam, iSizeParam) != PB_HEADER_OK)
	{
		throw ParserException();
	}
}

void BytesStream::SkipPb(uint32_t iSizeParam)
{
	if (RemainSize() < iSizeParam)
	{
		throw ParserException();
	}
	m_iCurrentPosition += iSizeParam;
}

BytesStream& BytesStream::WritePb(uint32_t iSequenceParam, const std::string& sValueParam)
{
	if (sValueParam.size() > (size_t)0x7FFFFFFF)
	{
		throw ParserException();
	}
	const void* pData = sValueParam.empty() ? NULL : sValueParam.data();
	return WritePbRaw(iSequenceParam, pData, (uint32_t)sValueParam.size());
}

BytesStream& BytesStream::WritePb(uint32_t iSequenceParam, const Bytes& stValueParam)
{
	if (stValueParam.Size() > (size_t)0x7FFFFFFF)
	{
		throw ParserException();
	}
	uint32_t iSize = (uint32_t)stValueParam.Size();
	const void* pData = iSize ? stValueParam.Data() : NULL;
	return WritePbRaw(iSequenceParam, pData, iSize);
}

BytesStream& BytesStream::WritePb(uint32_t iSequenceParam, const ::OI::Rpc::Serialize& stValueParam)
{
	BytesStream stInner;
	stValueParam.serialize(stInner);
	if (stInner.Size() > (size_t)0x7FFFFFFF)
	{
		throw ParserException();
	}
	uint32_t iSize = (uint32_t)stInner.Size();
	const void* pData = NULL;
	if (iSize > 0)
	{
		pData = stInner.Begin();
	}
	return WritePbRaw(iSequenceParam, pData, iSize);
}

void BytesStream::ReadPb(uint32_t iSizeParam, std::string& sValueParam)
{
	if (RemainSize() < iSizeParam)
	{
		throw ParserException();
	}
	if (iSizeParam == 0)
	{
		sValueParam.clear();
	}
	else
	{
		sValueParam.assign(GetCurPtr(), iSizeParam);
	}
	m_iCurrentPosition += iSizeParam;
}

void BytesStream::ReadPb(uint32_t iSizeParam, Bytes& stValueParam)
{
	if (RemainSize() < iSizeParam)
	{
		throw ParserException();
	}
	stValueParam.Clear();
	if (iSizeParam > 0)
	{
		stValueParam.Append(GetCurPtr(), iSizeParam);
	}
	m_iCurrentPosition += iSizeParam;
}

void BytesStream::ReadPb(uint32_t iSizeParam, ::OI::Rpc::Serialize& stValueParam)
{
	if (RemainSize() < iSizeParam)
	{
		throw ParserException();
	}
	BytesStream stSub;
	if (iSizeParam > 0)
	{
		stSub.Append(GetCurPtr(), iSizeParam);
	}
	stValueParam.deserialize(stSub);
	if (stSub.RemainSize() != 0)
	{
		throw ParserException();
	}
	m_iCurrentPosition += iSizeParam;
}
}
}
