#include "oi_bytes_stream.h"
#include <string>
#include "oi_rpc.h"

namespace OI
{
namespace Base
{
// 长度前缀是 4 字节小端，最高位为 1 的长度对端按非法处理。
static void WriteLenPrefixed(BytesStream& stOutputStream, const void* pData, size_t iSize)
{
	if (iSize > (size_t)0x7FFFFFFF)
	{
		throw ParserException();
	}
	stOutputStream << (int32_t)iSize;
	if (iSize > 0)
	{
		stOutputStream.Append(pData, iSize);
	}
}

static size_t ReadLenPrefix(BytesStream& stOutputStream)
{
	int32_t iSize;
	stOutputStream >> iSize;
	if (iSize < 0 || stOutputStream.RemainSize() < (size_t)iSize)
	{
		throw ParserException();
	}
	return (size_t)iSize;
}

BytesStream& BytesStream::operator<<(const std::string& sFirst)
{
	WriteLenPrefixed(*this, sFirst.data(), sFirst.size());
	return *this;
}
BytesStream& BytesStream::operator>>(std::string& sFirst)
{
	size_t iSize = ReadLenPrefix(*this);
	sFirst.assign(GetCurPtr(), iSize);
	m_iCurrentPosition += iSize;
	return *this;
}

BytesStream& BytesStream::operator<<(const Bytes& stFirst)
{
	WriteLenPrefixed(*this, stFirst.Data(), stFirst.Size());
	return *this;
}
// 覆盖 a 原有内容，和 ReadPb 一致。
BytesStream& BytesStream::operator>>(Bytes& stFirst)
{
	size_t iSize = ReadLenPrefix(*this);
	stFirst.Clear();
	stFirst.Append(GetCurPtr(), iSize);
	m_iCurrentPosition += iSize;
	return *this;
}

BytesStream& BytesStream::operator<<(const BytesStream& stFirst)
{
	Append(stFirst);
	return *this;
}

BytesStream& BytesStream::operator<<(const ::OI::Rpc::Serialize& stFirst)
{
	BytesStream stPayload;
	stFirst.serialize(stPayload);
	WriteLenPrefixed(*this, stPayload.Data(), stPayload.Size());
	return *this;
}

BytesStream& BytesStream::operator>>(::OI::Rpc::Serialize& stFirst)
{
	const size_t iSize = ReadLenPrefix(*this);
	ReadPb(static_cast<uint32_t>(iSize), stFirst);
	return *this;
}

BytesStream& BytesStream::WritePbRaw(uint32_t iSequence, const void* pData, uint32_t iSize)
{
	if ((int32_t)iSequence < 0 || (int32_t)iSize < 0)
	{
		throw ParserException();
	}
	unsigned char vSequence[sizeof(uint32_t)];
	unsigned char vSizeBytes[sizeof(uint32_t)];
	StoreLittleEndian(vSequence, iSequence);
	StoreLittleEndian(vSizeBytes, iSize);
	Append(vSequence, sizeof(vSequence));
	Append(vSizeBytes, sizeof(vSizeBytes));
	if (iSize > 0)
	{
		if (!pData)
		{
			throw ParserException();
		}
		Append(pData, iSize);
	}
	return *this;
}

PbHeaderState BytesStream::PreparePbHeader(uint32_t& iSequence, uint32_t& iSize)
{
	if (RemainSize() < sizeof(uint32_t) * 2)
	{
		return PB_HEADER_NEED_MORE;
	}
	uint32_t iSequenceValue = LoadLittleEndian<uint32_t>(GetCurPtr());
	uint32_t iSizeValue = LoadLittleEndian<uint32_t>(GetCurPtr() + sizeof(uint32_t));
	if ((int32_t)iSequenceValue < 0 || (int32_t)iSizeValue < 0)
	{
		return PB_HEADER_BAD;
	}
	if (RemainSize() < sizeof(uint32_t) * 2 + (size_t)iSizeValue)
	{
		return PB_HEADER_NEED_MORE;
	}
	m_iCurrentPosition += sizeof(uint32_t) * 2;
	iSequence = iSequenceValue;
	iSize = iSizeValue;
	return PB_HEADER_OK;
}

BytesStream& BytesStream::ReadFields(void* pObject, std::span<const PbField> vFields, std::span<uint8_t> vSeen)
{
	if (vSeen.size() != vFields.size() || (!pObject && !vFields.empty()))
	{
		throw ParserException();
	}
	while (RemainSize() > 0)
	{
		uint32_t iSequence = 0;
		uint32_t iSize = 0;
		ReadPbHeader(iSequence, iSize);
		const auto it = std::lower_bound(vFields.begin(), vFields.end(), iSequence,
		    [](const PbField& stField, uint32_t iSequenceLocal)
		{
			return stField.iSequence < iSequenceLocal;
		});
		if (it == vFields.end() || it->iSequence != iSequence)
		{
			SkipPb(iSize);
			continue;
		}
		const size_t iField = static_cast<size_t>(it - vFields.begin());
		if (vSeen[iField] != 0)
		{
			throw ParserException();
		}
		it->fnRead(*this, iSize, pObject);
		vSeen[iField] = 1;
	}
	return *this;
}

void BytesStream::ReadPbHeader(uint32_t& iSequence, uint32_t& iSize)
{
	if (PreparePbHeader(iSequence, iSize) != PB_HEADER_OK)
	{
		throw ParserException();
	}
}

void BytesStream::SkipPb(uint32_t iSize)
{
	if (RemainSize() < iSize)
	{
		throw ParserException();
	}
	m_iCurrentPosition += iSize;
}

BytesStream& BytesStream::WritePb(uint32_t iSequence, const std::string& sValue)
{
	if (sValue.size() > (size_t)0x7FFFFFFF)
	{
		throw ParserException();
	}
	const void* pData = sValue.empty() ? NULL : sValue.data();
	return WritePbRaw(iSequence, pData, (uint32_t)sValue.size());
}

BytesStream& BytesStream::WritePb(uint32_t iSequence, const Bytes& stValue)
{
	if (stValue.Size() > (size_t)0x7FFFFFFF)
	{
		throw ParserException();
	}
	uint32_t iSize = (uint32_t)stValue.Size();
	const void* pData = iSize ? stValue.Data() : NULL;
	return WritePbRaw(iSequence, pData, iSize);
}

BytesStream& BytesStream::WritePb(uint32_t iSequence, const ::OI::Rpc::Serialize& stValue)
{
	BytesStream stInner;
	stValue.serialize(stInner);
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
	return WritePbRaw(iSequence, pData, iSize);
}

void BytesStream::ReadPb(uint32_t iSize, std::string& sValue)
{
	if (RemainSize() < iSize)
	{
		throw ParserException();
	}
	if (iSize == 0)
	{
		sValue.clear();
	}
	else
	{
		sValue.assign(GetCurPtr(), iSize);
	}
	m_iCurrentPosition += iSize;
}

void BytesStream::ReadPb(uint32_t iSize, Bytes& stValue)
{
	if (RemainSize() < iSize)
	{
		throw ParserException();
	}
	stValue.Clear();
	if (iSize > 0)
	{
		stValue.Append(GetCurPtr(), iSize);
	}
	m_iCurrentPosition += iSize;
}

void BytesStream::ReadPb(uint32_t iSize, ::OI::Rpc::Serialize& stValue)
{
	if (RemainSize() < iSize)
	{
		throw ParserException();
	}
	BytesStream stSub;
	if (iSize > 0)
	{
		stSub.Append(GetCurPtr(), iSize);
	}
	stValue.deserialize(stSub);
	if (stSub.RemainSize() != 0)
	{
		throw ParserException();
	}
	m_iCurrentPosition += iSize;
}
}
}
