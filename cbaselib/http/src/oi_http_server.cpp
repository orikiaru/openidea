#include "oi_http_server.h"
#include "oi_http_stream.h"
#include "oi_http_header_fields.h"
#include "oi_net_session.h"
#include "oi_timer_manager.h"
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace OI::Http
{
namespace
{
constexpr size_t kHeaderLimit = 64 * 1024;
constexpr size_t kBodyLimit = 8 * 1024 * 1024;
using View = std::string_view;

bool EqualName(View sLeft, View sRight)
{
	return sLeft.size() == sRight.size() && std::equal(sLeft.begin(), sLeft.end(), sRight.begin(), [](char iLeft, char iRight)
	{
		return (iLeft >= 'A' && iLeft <= 'Z' ? iLeft + ('a' - 'A') : iLeft) == iRight;
	});
}

View Trim(View sText)
{
	const auto iFirst = sText.find_first_not_of(" \t");
	return iFirst == View::npos ? View{} : sText.substr(iFirst, sText.find_last_not_of(" \t") - iFirst + 1);
}

bool Token(View sText)
{
	return !sText.empty() && std::all_of(sText.begin(), sText.end(), [](unsigned char iChar)
	{
		return (iChar >= 'a' && iChar <= 'z') || (iChar >= 'A' && iChar <= 'Z') ||
		       (iChar >= '0' && iChar <= '9') || View("!#$%&'*+-.^_|`~").find(iChar) != View::npos;
	});
}

bool Number(View sText, size_t& iValue, int iBase)
{
	if (sText.empty())
	{
		return false;
	}
	const auto stResult = std::from_chars(sText.data(), sText.data() + sText.size(), iValue, iBase);
	return stResult.ec == std::errc{} && stResult.ptr == sText.data() + sText.size();
}

bool Decode(View sText, std::string& sOutput, bool bQuery)
{
	for (size_t i = 0; i < sText.size(); ++i)
	{
		if (sText[i] == '%')
		{
			size_t iByte = 0;
			if (sText.size() - i < 3 || !Number(sText.substr(i + 1, 2), iByte, 16))
			{
				return false;
			}
			sOutput.push_back(static_cast<char>(iByte));
			i += 2;
		}
		else
		{
			sOutput.push_back(bQuery && sText[i] == '+' ? ' ' : sText[i]);
		}
	}
	return true;
}

bool ParseTarget(View sTarget, Request& stRequest)
{
	const auto iQuery = sTarget.find('?');
	if (sTarget.empty() || sTarget.front() != '/' || !Decode(sTarget.substr(0, iQuery), stRequest.sPath, false))
	{
		return false;
	}
	if (iQuery == View::npos)
	{
		return true;
	}
	sTarget.remove_prefix(iQuery + 1);
	while (!sTarget.empty())
	{
		auto iEnd = sTarget.find('&');
		auto sParam = sTarget.substr(0, iEnd);
		auto iEquals = sParam.find('=');
		std::string sKey, sValue;
		if (!Decode(sParam.substr(0, iEquals), sKey, true) ||
		    (iEquals != View::npos && !Decode(sParam.substr(iEquals + 1), sValue, true)))
		{
			return false;
		}
		stRequest.mQuery.Add(std::move(sKey), std::move(sValue));
		if (iEnd == View::npos)
		{
			break;
		}
		sTarget.remove_prefix(iEnd + 1);
	}
	return true;
}

struct RequestFrame
{
	Detail::StreamBody stStream;
	size_t iStreamConsumed = 0;
	Request stRequest;
	size_t iRemaining = 0;
	size_t iScan = 0;
	bool bHeaders = false;
	bool bHttp10 = false;
	bool bClose = false;
	bool bChunked = false;
	bool bTrailer = false;

	int ParseHeaders(View sText)
	{
		const auto iLine = sText.find("\r\n");
		const auto sLine = sText.substr(0, iLine);
		const auto iSpace = sLine.find(' ');
		const auto iLast = sLine.rfind(' ');
		if (iSpace == View::npos || iSpace == iLast || !Token(sLine.substr(0, iSpace)) ||
		    !ParseTarget(sLine.substr(iSpace + 1, iLast - iSpace - 1), stRequest))
		{
			return 400;
		}
		stRequest.sMethod = sLine.substr(0, iSpace);
		const auto sVersion = sLine.substr(iLast + 1);
		bHttp10 = sVersion == "HTTP/1.0";
		if (sVersion != "HTTP/1.1" && sVersion != "HTTP/1.0")
		{
			return 400;
		}
		bool bCloseToken = false, bKeepToken = false, bLength = false, bHost = false;
		sText.remove_prefix(iLine + 2);
		while (!sText.empty())
		{
			const auto iEnd = sText.find("\r\n");
			const auto sHeader = sText.substr(0, iEnd);
			const auto iColon = sHeader.find(':');
			if (iColon == View::npos || !Token(sHeader.substr(0, iColon)))
			{
				return 400;
			}
			const auto sName = sHeader.substr(0, iColon);
			auto sValue = Trim(sHeader.substr(iColon + 1));
			if (std::any_of(sValue.begin(), sValue.end(), [](unsigned char iChar)
			{
				return (iChar < 32 && iChar != 9) || iChar == 127;
			}))
			{
				return 400;
			}
			stRequest.vHeaders.emplace_back(sName, sValue);
			if (EqualName(sName, "content-type"))
			{
				stRequest.sContentType = sValue;
			}
			else if (EqualName(sName, "content-length"))
			{
				if (bLength || !Number(sValue, iRemaining, 10))
				{
					return 400;
				}
				bLength = true;
			}
			else if (EqualName(sName, "transfer-encoding"))
			{
				if (bChunked || !EqualName(sValue, "chunked"))
				{
					return 400;
				}
				bChunked = true;
			}
			else if (EqualName(sName, "host"))
			{
				if (bHost || sValue.empty())
				{
					return 400;
				}
				bHost = true;
			}
			else if (EqualName(sName, "expect"))
			{
				return 417;
			}
			else if (EqualName(sName, "connection"))
			{
				while (!sValue.empty())
				{
					const auto iEndToken = sValue.find(',');
					const auto sToken = Trim(sValue.substr(0, iEndToken));
					bCloseToken |= EqualName(sToken, "close");
					bKeepToken |= EqualName(sToken, "keep-alive");
					if (iEndToken == View::npos)
					{
						break;
					}
					sValue.remove_prefix(iEndToken + 1);
				}
			}
			sText.remove_prefix(iEnd + 2);
		}
		bClose = bCloseToken || (sVersion == "HTTP/1.0" && !bKeepToken);
		if ((bChunked && bLength) || (!bHost && sVersion == "HTTP/1.1"))
		{
			return 400;
		}
		return 0;
	}

	// 0 等后续字节，200 为完整请求，其余为应返回的 HTTP 错误。
	int Read(Base::Bytes& stBytes, const ServerManager& stServer)
	{
		iStreamConsumed = 0;
		auto fnView = [&stBytes]()
		{
			return View(stBytes.Begin(), stBytes.Size());
		};
		if (!bHeaders)
		{
			const auto iEnd = fnView().find("\r\n\r\n", iScan);
			if (iEnd == View::npos)
			{
				iScan = stBytes.Size() > 3 ? stBytes.Size() - 3 : 0;
				return stBytes.Size() > kHeaderLimit ? 431 : 0;
			}
			const int iCode = iEnd > kHeaderLimit ? 431 : ParseHeaders(fnView().substr(0, iEnd + 2));
			if (iCode != 0)
			{
				return iCode;
			}
			stBytes.Erase(0, iEnd + 4);
			bHeaders = true;
			if (auto fnFactory = stServer.FindSink(stRequest))
			{
				try
				{
					stRequest.pBodySink = fnFactory(stRequest);
				}
				catch (...)
				{
					return 500;
				}
				if (!stRequest.pBodySink)
				{
					return 400;
				}
				stStream.Begin(bChunked, true, iRemaining);
			}
			else if (iRemaining > kBodyLimit)
			{
				return 413;
			}
		}
		if (stRequest.pBodySink)
		{
			const int32_t iRet = stStream.Consume(fnView(), *stRequest.pBodySink, iStreamConsumed);
			stBytes.Erase(0, iStreamConsumed);
			if (iRet == OI_ERR_BUSY)
			{
				return iRet;
			}
			return iRet == OI_OK ? (stStream.Done() ? 200 : 0) : 400;
		}
		if (!bChunked)
		{
			if (stBytes.Size() < iRemaining)
			{
				return 0;
			}
			stRequest.sBody.assign(stBytes.Begin(), iRemaining);
			stBytes.Erase(0, iRemaining);
			return 200;
		}
		while (!bTrailer)
		{
			if (iRemaining == 0)
			{
				const auto iEnd = fnView().find("\r\n");
				if (iEnd == View::npos || iEnd > 1024)
				{
					return stBytes.Size() > 1024 ? 400 : 0;
				}
				const auto sLine = fnView().substr(0, iEnd);
				if (!Number(sLine.substr(0, sLine.find(';')), iRemaining, 16))
				{
					return 400;
				}
				if (iRemaining > kBodyLimit - stRequest.sBody.size())
				{
					return 413;
				}
				stBytes.Erase(0, iEnd + 2);
				if (iRemaining == 0)
				{
					bTrailer = true;
					break;
				}
			}
			if (stBytes.Size() < iRemaining + 2)
			{
				return 0;
			}
			if (fnView().substr(iRemaining, 2) != "\r\n")
			{
				return 400;
			}
			stRequest.sBody.append(stBytes.Begin(), iRemaining);
			stBytes.Erase(0, iRemaining + 2);
			iRemaining = 0;
		}
		if (fnView().substr(0, 2) == "\r\n")
		{
			stBytes.Erase(0, 2);
			return 200;
		}
		const auto iEnd = fnView().find("\r\n\r\n");
		if (iEnd == View::npos || iEnd > kHeaderLimit)
		{
			return stBytes.Size() > kHeaderLimit ? 431 : 0;
		}
		stBytes.Erase(0, iEnd + 4);
		return 200;
	}
};

const char* Reason(int iStatus)
{
	switch (iStatus)
	{
	case 200:
		return "OK";
	case 400:
		return "Bad Request";
	case 404:
		return "Not Found";
	case 413:
		return "Content Too Large";
	case 417:
		return "Expectation Failed";
	case 431:
		return "Request Header Fields Too Large";
	case 500:
		return "Internal Server Error";
	case 502:
		return "Bad Gateway";
	default:
		return "";
	}
}

class ServerSession final : public Net::NetSession
{
	ServerManager& m_stServer;
	RequestFrame m_stFrame;
	bool m_bBusy = false;
	bool m_bCloseAfter = false;
	bool m_bClosePosted = false;
	Net::Poll::QueuedCall m_stCloseCall;
	uint64_t m_iFrameGeneration = 0;
	bool m_bPartial = false;
	Detail::StreamPump m_stPump;
	std::shared_ptr<BodySource> m_pSource;
	bool m_bStreamClose = false;
	bool m_bStreamChunked = true;
	bool m_bReadPaused = false;
	uint64_t m_iProgress = 0;

	bool StartPump()
	{
		return m_stPump.Start(this, [](uv_timer_t* pTimer)
		{
			static_cast<ServerSession*>(pTimer->data)->PumpStream();
		});
	}

	void PumpStream()
	{
		const auto iNow = uv_now(Net::Poll::GetInstance().Loop());
		if (iNow - m_iProgress >= m_stServer.IncompleteFrameTimeoutMilliseconds())
		{
			m_stServer.CloseSession(GetSessionID());
			return;
		}
		if (m_pSource)
		{
			bool bDone = false, bProgress = false;
			if (Detail::StreamBody::Produce(*this, *m_pSource, bDone, bProgress, m_bStreamChunked) != OI_OK)
			{
				m_stServer.CloseSession(GetSessionID());
				return;
			}
			if (bProgress)
			{
				m_iProgress = iNow;
			}
			if (bDone)
			{
				m_pSource.reset();
				m_stPump.Stop();
				m_bCloseAfter = m_bStreamClose;
				m_bBusy = false;
				if (m_bCloseAfter && PendingSendBytes() == 0)
				{
					m_stServer.CloseSession(GetSessionID());
					return;
				}
				OnDataIn();
			}
		}
		else if (m_stFrame.stRequest.pBodySink && m_stFrame.stRequest.pBodySink->IsCancelled())
		{
			m_stServer.CloseSession(GetSessionID());
		}
		else if (m_bReadPaused)
		{
			OnDataIn();
		}
	}

	struct Completion
	{
		std::weak_ptr<ServerSession*> pBinding;
		Response stReply;
		bool bClose;
		bool bHttp10;
		Net::Poll::QueuedCall stCall;
	};
	// 仅 poll 线程解引用；断连立即失效，迟到的完成回调不持有会话。
	std::shared_ptr<ServerSession*> m_pBinding;

	static Coro::Task Dispatch(std::unique_ptr<Completion> pDone, ServerManager::Handler fnHandler, Request stRequest)
	{
		try
		{
			if (fnHandler)
			{
				co_await fnHandler(stRequest, pDone->stReply);
			}
			else
			{
				pDone->stReply.iStatus = 404;
			}
		}
		catch (...)
		{
			std::fprintf(stderr, "web handler exception\n");
			pDone->stReply = {};
			pDone->stReply.iStatus = 500;
		}
		auto* pPending = pDone.release();
		Net::Poll::GetInstance().PostOnLoop(pPending->stCall);
	}

	void Reply(const Response& stReply, bool bClose, bool bHttp10)
	{
		if (IsClosed())
		{
			return;
		}
		const bool bValid = stReply.iStatus >= 200 && stReply.iStatus <= 599 &&
		                    HeaderFields::ValidForSend(stReply) && (!stReply.pBodySource || stReply.sBody.empty());
		const int iStatusLocal = bValid ? stReply.iStatus : 500;
		std::string sWire = std::string(bHttp10 ? "HTTP/1.0 " : "HTTP/1.1 ") + std::to_string(iStatusLocal) + " " + Reason(iStatusLocal) + "\r\n";
		const bool bBody = iStatusLocal != 204 && iStatusLocal != 304;
		if (bBody)
		{
			if (bValid && stReply.pBodySource)
			{
				if (bHttp10)
				{
					bClose = true;
				}
				else
				{
					sWire += "Transfer-Encoding: chunked\r\n";
				}
			}
			else
			{
				sWire += "Content-Length: " + std::to_string(bValid ? stReply.sBody.size() : 0) + "\r\n";
			}
			if (bValid && !stReply.sContentType.empty())
			{
				sWire += "Content-Type: " + stReply.sContentType + "\r\n";
			}
		}
		if (bValid)
		{
			for (const auto& [sName, sValue] : stReply.vHeaders)
			{
				sWire += sName + ": " + sValue + "\r\n";
			}
		}
		sWire += bClose ? "Connection: close\r\n\r\n" : "Connection: keep-alive\r\n\r\n";
		if (bBody && bValid)
		{
			sWire += stReply.sBody;
		}
		m_pSource = bBody && bValid ? stReply.pBodySource : nullptr;
		m_bStreamClose = bClose;
		m_bStreamChunked = !bHttp10;
		m_bCloseAfter = bClose && !m_pSource;
		if (SendResult(Base::Bytes(sWire.data(), sWire.size())) != OI_OK)
		{
			m_stServer.CloseSession(GetSessionID());
			return;
		}
		if (m_pSource)
		{
			m_iProgress = uv_now(Net::Poll::GetInstance().Loop());
			if (!StartPump())
			{
				m_stServer.CloseSession(GetSessionID());
			}
			return;
		}
		m_bBusy = false;
		if (!bClose)
		{
			OnDataIn();
		}
	}

	void OnDataIn() override
	{
		if (m_bBusy || m_bCloseAfter || IsClosed() || !stInputBuffer.Size())
		{
			return;
		}
		const bool bHadSink = bool(m_stFrame.stRequest.pBodySink);
		const int iCode = m_stFrame.Read(stInputBuffer, m_stServer);
		if (m_stFrame.stRequest.pBodySink)
		{
			m_bPartial = false;
			if (!bHadSink || m_stFrame.iStreamConsumed)
			{
				m_iProgress = uv_now(Net::Poll::GetInstance().Loop());
			}
			if (iCode == OI_ERR_BUSY && !m_bReadPaused)
			{
				m_bReadPaused = true;
				ForbidRead();
			}
			else if (iCode != OI_ERR_BUSY && m_bReadPaused)
			{
				m_bReadPaused = false;
				AllowRead();
			}
			if (iCode == OI_ERR_BUSY || iCode == 0)
			{
				if (!StartPump())
				{
					m_stServer.CloseSession(GetSessionID());
				}
				return;
			}
			m_stPump.Stop();
		}
		if (iCode == 0)
		{
			if (!m_bPartial)
			{
				m_bPartial = true;
				const auto iGeneration = ++m_iFrameGeneration;
				const std::weak_ptr<ServerSession*> pWeak = m_pBinding;
				const auto iTimeout = std::min<uint64_t>(m_stServer.IncompleteFrameTimeoutMilliseconds(), INT32_MAX);
				if (Timer::BaseTimerManager::GetInstance().AddTimer([pWeak, iGeneration]()
				{
					if (auto pBinding = pWeak.lock())
					{
						auto* pSession = *pBinding;
						if (pSession->m_bPartial && pSession->m_iFrameGeneration == iGeneration)
						{
							pSession->m_stServer.CloseSession(pSession->GetSessionID());
						}
					}
				}, static_cast<int>(iTimeout)) != OI_OK)
				{
					m_stServer.CloseSession(GetSessionID());
				}
			}
			return;
		}
		m_bPartial = false;
		if (iCode != 200)
		{
			Response stReply;
			stReply.iStatus = iCode;
			Reply(stReply, true, m_stFrame.bHttp10);
			return;
		}
		m_bBusy = true;
		auto fnHandler = m_stServer.Find(m_stFrame.stRequest);
		auto pDone = std::make_unique<Completion>();
		pDone->stReply.iStatus = 200;
		pDone->stReply.iCode = OI_OK;
		pDone->stReply.sContentType = "text/plain";
		pDone->pBinding = m_pBinding;
		pDone->bClose = m_stFrame.bClose;
		pDone->bHttp10 = m_stFrame.bHttp10;
		pDone->stCall.fnCall = [](void* pArg)
		{
			std::unique_ptr<Completion> pFinished(static_cast<Completion*>(pArg));
			if (auto pBinding = pFinished->pBinding.lock())
			{
				(*pBinding)->Reply(pFinished->stReply, pFinished->bClose, pFinished->bHttp10);
			}
		};
		pDone->stCall.pArgument = pDone.get();
		auto stRequest = std::move(m_stFrame.stRequest);
		m_stFrame = {};
		Coro::StartTask([fnHandler, pDone = std::move(pDone), stRequest = std::move(stRequest)](Coro::Token) mutable
		{
			return Dispatch(std::move(pDone), fnHandler, std::move(stRequest));
		});
	}

	void OnDataOut() override
	{
		if (m_bCloseAfter && !m_bClosePosted && iQueuedBytes == 0)
		{
			// OnDataOut 持会话锁，到下一轮再通过管理器摘表并关闭。
			m_bClosePosted = true;
			AddRef();
			m_stCloseCall.fnCall = [](void* pArg)
			{
				auto* pSession = static_cast<ServerSession*>(pArg);
				pSession->m_stServer.CloseSession(pSession->GetSessionID());
				pSession->Release();
			};
			m_stCloseCall.pArgument = this;
			Net::Poll::GetInstance().PostOnLoop(m_stCloseCall);
		}
	}

	void OnClose() override
	{
		m_pBinding.reset();
		m_stPump.Stop();
		if (m_pSource)
		{
			m_pSource->Cancel();
		}
		if (m_stFrame.stRequest.pBodySink)
		{
			m_stFrame.stRequest.pBodySink->Cancel();
		}
		NetSession::OnClose();
	}

  public:
	explicit ServerSession(ServerManager* pManager)
	    : NetSession(pManager), m_stServer(*pManager), m_pBinding(std::make_shared<ServerSession*>(this))
	{
	}
};
}

void ServerManager::Register(std::string sMethod, std::string sPath, Handler fnHandler)
{
	if (!fnHandler || !m_mRoutes.emplace(std::make_pair(std::move(sMethod), std::move(sPath)), Route{fnHandler, nullptr}).second)
	{
		throw std::logic_error("invalid or duplicate HTTP route");
	}
}

ServerManager::Handler ServerManager::Find(const Request& stRequest) const
{
	auto it = m_mRoutes.find({stRequest.sMethod, stRequest.sPath});
	return it == m_mRoutes.end() ? nullptr : it->second.fnHandler;
}

void ServerManager::RegisterStream(std::string sMethod, std::string sPath, SinkFactory fnFactory, Handler fnHandler)
{
	if (!fnFactory || !fnHandler || !m_mRoutes.emplace(std::make_pair(std::move(sMethod), std::move(sPath)), Route{fnHandler, fnFactory}).second)
	{
		throw std::logic_error("invalid or duplicate HTTP streaming route");
	}
}

ServerManager::SinkFactory ServerManager::FindSink(const Request& stRequest) const
{
	auto it = m_mRoutes.find({stRequest.sMethod, stRequest.sPath});
	return it == m_mRoutes.end() ? nullptr : it->second.fnFactory;
}

Net::Session* ServerManager::CloneSession()
{
	return new ServerSession(this);
}
}
