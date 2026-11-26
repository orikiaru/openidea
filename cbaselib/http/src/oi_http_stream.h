#pragma once
#include "oi_http_message.h"
#include "oi_session.h"
#include <algorithm>
#include <charconv>
#include <uv.h>

namespace OI::Http::Detail
{
// 上限只计算长度行正文。分片末尾的 CR 可能是尚未收全的 CRLF。
inline bool ChunkLineTooLong(std::string_view sData, size_t iEnd)
{
	constexpr size_t iMaxChunkLineBytes = 1024;
	if (iEnd != sData.npos)
	{
		return iEnd > iMaxChunkLineBytes;
	}
	return sData.size() - static_cast<size_t>(sData.ends_with('\r')) > iMaxChunkLineBytes;
}

inline bool ValidFieldLine(std::string_view sLine)
{
	const auto iColon = sLine.find(':');
	if (iColon == sLine.npos || iColon == 0)
	{
		return false;
	}
	for (const unsigned char iByte : sLine.substr(0, iColon))
	{
		if (!((iByte >= 'A' && iByte <= 'Z') || (iByte >= 'a' && iByte <= 'z') ||
		        (iByte >= '0' && iByte <= '9') || std::string_view("!#$%&'*+-.^_`|~").find(iByte) != sLine.npos))
		{
			return false;
		}
	}
	for (const unsigned char iByte : sLine.substr(iColon + 1))
	{
		if ((iByte < 32 && iByte != 9) || iByte == 127)
		{
			return false;
		}
	}
	return true;
}
// Only the poll thread touches the timer or its owner. A callback can stop its own timer.
class StreamPump
{
	uv_timer_t* m_pTimer = nullptr;

  public:
	StreamPump() = default;
	StreamPump(const StreamPump&) = delete;
	StreamPump& operator=(const StreamPump&) = delete;
	~StreamPump()
	{
		Stop();
	}
	bool Start(void* pOwner, uv_timer_cb fnTick, uint64_t iIntervalMilliseconds = 2)
	{
		if (m_pTimer)
		{
			return true;
		}
		auto pTimer = std::make_unique<uv_timer_t>();
		if (uv_timer_init(Net::Poll::GetInstance().Loop(), pTimer.get()) != 0)
		{
			return false;
		}
		m_pTimer = pTimer.release();
		m_pTimer->data = pOwner;
		if (uv_timer_start(m_pTimer, fnTick, iIntervalMilliseconds, iIntervalMilliseconds) != 0)
		{
			Stop();
			return false;
		}
		return true;
	}
	void Stop()
	{
		if (m_pTimer)
		{
			m_pTimer->data = nullptr;
			uv_close(reinterpret_cast<uv_handle_t*>(m_pTimer), [](uv_handle_t* pHandle)
			{
				delete reinterpret_cast<uv_timer_t*>(pHandle);
			});
			m_pTimer = nullptr;
		}
	}
};

class StreamBody
{
	enum class Phase
	{
		Length,
		Size,
		Data,
		CrLf,
		Trailers,
		Eof,
		Done
	};
	Phase m_iPhase = Phase::Done;
	uint64_t m_iRemaining = 0;
	size_t m_iTrailerBytes = 0;
	size_t m_iRetrySize = 0;
	Base::Bytes m_stPendingWire;
	bool m_bPendingFinal = false;

  public:
	static constexpr size_t Block = 64 * 1024;
	void Begin(bool bChunked, bool bLength, uint64_t iLength)
	{
		m_iPhase = bChunked ? Phase::Size : (bLength ? Phase::Length : Phase::Eof);
		m_iRemaining = iLength;
		m_iTrailerBytes = 0;
		m_iRetrySize = 0;
	}
	bool Done() const
	{
		return m_iPhase == Phase::Done;
	}
	bool UntilClose() const
	{
		return m_iPhase == Phase::Eof;
	}
	int32_t Consume(std::string_view sData, BodySink& stSink, size_t& iConsumed)
	{
		iConsumed = 0;
		while (!Done())
		{
			if (stSink.IsCancelled())
			{
				return OI_ERR_CANCELLED;
			}
			if (m_iPhase == Phase::Length && !m_iRemaining)
			{
				m_iPhase = Phase::Done;
				break;
			}
			auto sRest = sData.substr(iConsumed);
			if (sRest.empty())
			{
				break;
			}
			if (m_iPhase == Phase::Size || m_iPhase == Phase::Trailers)
			{
				auto iEnd = sRest.find("\r\n");
				if (m_iPhase == Phase::Size && ChunkLineTooLong(sRest, iEnd))
				{
					return OI_ERR_PROTOCOL;
				}
				const size_t iTrailerLimit = Block - m_iTrailerBytes;
				if (iEnd == sRest.npos)
				{
					return m_iPhase == Phase::Trailers && sRest.size() > iTrailerLimit ? OI_ERR_PROTOCOL : OI_OK;
				}
				if (m_iPhase == Phase::Trailers && iEnd + 2 > iTrailerLimit)
				{
					return OI_ERR_PROTOCOL;
				}
				iConsumed += iEnd + 2;
				if (m_iPhase == Phase::Trailers)
				{
					if (iEnd && !ValidFieldLine(sRest.substr(0, iEnd)))
					{
						return OI_ERR_PROTOCOL;
					}
					m_iTrailerBytes += iEnd + 2;
					if (!iEnd)
					{
						m_iPhase = Phase::Done;
					}
					continue;
				}
				auto sNumber = sRest.substr(0, iEnd);
				sNumber = sNumber.substr(0, sNumber.find(';'));
				auto stRet = std::from_chars(sNumber.data(), sNumber.data() + sNumber.size(), m_iRemaining, 16);
				if (stRet.ec != std::errc{} || stRet.ptr != sNumber.data() + sNumber.size())
				{
					return OI_ERR_PROTOCOL;
				}
				m_iPhase = m_iRemaining ? Phase::Data : Phase::Trailers;
				continue;
			}
			if (m_iPhase == Phase::CrLf)
			{
				if (sRest.size() < 2)
				{
					break;
				}
				if (!sRest.starts_with("\r\n"))
				{
					return OI_ERR_PROTOCOL;
				}
				iConsumed += 2;
				m_iPhase = Phase::Size;
				continue;
			}
			size_t iSize = std::min(Block, sRest.size());
			if (m_iPhase != Phase::Eof)
			{
				iSize = static_cast<size_t>(std::min<uint64_t>(iSize, m_iRemaining));
			}
			if (m_iRetrySize)
			{
				iSize = m_iRetrySize;
			}
			int32_t iCode;
			try
			{
				iCode = stSink.Write(sRest.substr(0, iSize));
			}
			catch (...)
			{
				return OI_ERR_FAIL;
			}
			if (iCode != OI_OK)
			{
				m_iRetrySize = iSize;
				return iCode;
			}
			m_iRetrySize = 0;
			iConsumed += iSize;
			if (m_iPhase != Phase::Eof)
			{
				m_iRemaining -= iSize;
				if (m_iPhase == Phase::Data && !m_iRemaining)
				{
					m_iPhase = Phase::CrLf;
				}
			}
		}
		return OI_OK;
	}

	void ResetOutput()
	{
		m_stPendingWire.Clear();
		m_bPendingFinal = false;
	}
	int32_t Produce(Net::Session& stSession, BodySource& stSource, bool& bFinished, bool& bProgress, bool bChunked = true)
	{
		bProgress = false;
		for (int i = 0; i < 4; ++i)
		{
			if (stSource.IsCancelled())
			{
				return OI_ERR_CANCELLED;
			}
			if (m_stPendingWire.Size())
			{
				const size_t iTake = std::min(stSession.SendCapacity(), m_stPendingWire.Size());
				if (!iTake)
				{
					break;
				}
				const int32_t iCode = iTake == m_stPendingWire.Size()
				                          ? stSession.SendResult(m_stPendingWire)
				                          : stSession.SendResult(Base::Bytes(m_stPendingWire.Data(), iTake));
				if (iCode == OI_ERR_BUSY)
				{
					break;
				}
				if (iCode != OI_OK)
				{
					return iCode;
				}
				m_stPendingWire.Erase(0, iTake);
				bProgress = true;
				if (m_stPendingWire.Size())
				{
					break;
				}
				if (m_bPendingFinal)
				{
					m_bPendingFinal = false;
					bFinished = true;
					break;
				}
			}
			if (stSession.PendingSendBytes() >= Block)
			{
				break;
			}
			char vData[Block];
			size_t iSize = 0;
			int32_t iCode;
			try
			{
				iCode = stSource.Read(vData, iSize);
			}
			catch (...)
			{
				return OI_ERR_FAIL;
			}
			if (iCode == OI_ERR_BUSY)
			{
				break;
			}
			if (iCode != OI_OK || iSize > Block)
			{
				return iCode != OI_OK ? iCode : OI_ERR_INVALID_ARGUMENT;
			}
			if (bChunked)
			{
				char vHex[32];
				auto stRet = std::to_chars(vHex, vHex + sizeof(vHex), iSize, 16);
				m_stPendingWire.Append(vHex, static_cast<size_t>(stRet.ptr - vHex));
				m_stPendingWire.Append("\r\n", 2);
			}
			m_stPendingWire.Append(vData, iSize);
			if (bChunked)
			{
				m_stPendingWire.Append("\r\n", 2);
			}
			m_bPendingFinal = !iSize;
			if (!m_stPendingWire.Size())
			{
				m_bPendingFinal = false;
				bFinished = true;
				bProgress = true;
				break;
			}
		}
		return OI_OK;
	}
};
}
