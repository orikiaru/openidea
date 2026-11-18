#pragma once
#include "oi_http_message.h"
#include <algorithm>

// Deterministic binary stream used only by the web regression routes.
class WebTestSource : public OI::Http::BodySource
{
  public:
	explicit WebTestSource(size_t iSize)
	    : m_iSize(iSize)
	{
	}
	int32_t Read(std::span<char> vBuffer, size_t& iSize) override
	{
		if (vBuffer.size() > 64 * 1024)
		{
			return OI_ERR_INVALID_ARGUMENT;
		}
		iSize = std::min(vBuffer.size(), m_iSize - m_iOffset);
		for (size_t iIndex = 0; iIndex < iSize; ++iIndex)
		{
			vBuffer[iIndex] = static_cast<char>((m_iOffset + iIndex) % 251);
		}
		m_iOffset += iSize;
		return OI_OK;
	}

  private:
	size_t m_iSize;
	size_t m_iOffset = 0;
};

class WebTestSink : public OI::Http::BodySink
{
  public:
	int32_t Write(std::string_view sBytes) override
	{
		if (sBytes.size() > 64 * 1024)
		{
			return OI_ERR_INVALID_ARGUMENT;
		}
		if (m_iSize + sBytes.size() == 9 * 1024 * 1024 && m_iFinalBusyCount < 20)
		{
			++m_iFinalBusyCount;
			return OI_ERR_BUSY;
		}
		if (!m_bRetry)
		{
			m_bRetry = true;
			return OI_ERR_BUSY;
		}
		m_bRetry = false;
		for (size_t iIndex = 0; iIndex < sBytes.size(); ++iIndex)
		{
			if (static_cast<unsigned char>(sBytes[iIndex]) != (m_iSize + iIndex) % 251)
			{
				m_bValid = false;
			}
		}
		m_iSize += sBytes.size();
		return OI_OK;
	}
	size_t Size() const
	{
		return m_iSize;
	}
	bool Valid() const
	{
		return m_bValid;
	}

  private:
	size_t m_iSize = 0;
	bool m_bValid = true;
	bool m_bRetry = false;
	int32_t m_iFinalBusyCount = 0;
};

class WebTestBlockedSource : public OI::Http::BodySource
{
  public:
	int32_t Read(std::span<char>, size_t& iSize) override
	{
		iSize = 0;
		return OI_ERR_BUSY;
	}
};

class WebTestCancelSink : public OI::Http::BodySink
{
  public:
	int32_t Write(std::string_view sBytes) override
	{
		m_iSize += sBytes.size();
		Cancel();
		return OI_OK;
	}
	size_t Size() const
	{
		return m_iSize;
	}

  private:
	size_t m_iSize = 0;
};
