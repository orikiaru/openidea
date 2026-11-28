#include "web_client.h"
#include "oi_errno.h"
#include "oi_log.h"
#include "oi_thread.h"
#include <cstdio>
#include <string>
#include <utility>

template <typename T>
concept CanBorrowHeader = requires(T&& stMessage) {
	std::forward<T>(stMessage).GetHeader("Content-Type");
};

static_assert(CanBorrowHeader<OI::Http::Response&>);
static_assert(CanBorrowHeader<const OI::Http::Response&>);
static_assert(!CanBorrowHeader<OI::Http::Response>);
static_assert(!CanBorrowHeader<const OI::Http::Response>);

OI::Coro::Task WebClient::RunBodylessRegression(int32_t iPort)
{
	const std::string sBase = "http://127.0.0.1:" + std::to_string(iPort);
	auto stNoContent = co_await OI::Http::Client::GetUrl(sBase + "/no-content");
	auto stNotModified = co_await OI::Http::Client::GetUrl(sBase + "/not-modified");
	auto stEarlyHints = co_await OI::Http::Client::GetUrl(sBase + "/early-hints");
	auto stNext = co_await OI::Http::Client::GetUrl(sBase + "/next");
	const bool bGood = stNoContent.iCode == OI_OK && stNoContent.iStatus == 204 && stNoContent.sBody.empty() &&
	                   stNotModified.iCode == OI_OK && stNotModified.iStatus == 304 && stNotModified.sBody.empty() &&
	                   stEarlyHints.iCode == OI_OK && stEarlyHints.iStatus == 200 && stEarlyHints.sBody == "first-body" &&
	                   stNext.iCode == OI_OK && stNext.iStatus == 200 && stNext.sBody == "next-body";
	LOG_TRACE("bodyless regression codes=%d/%d/%d/%d statuses=%d/%d/%d/%d bodies=%zu/%zu/%zu/%zu bad=%d",
	    stNoContent.iCode, stNotModified.iCode, stEarlyHints.iCode, stNext.iCode, stNoContent.iStatus,
	    stNotModified.iStatus, stEarlyHints.iStatus, stNext.iStatus, stNoContent.sBody.size(),
	    stNotModified.sBody.size(), stEarlyHints.sBody.size(), stNext.sBody.size(), bGood ? 0 : 1);
	fflush(stdout);
	OI::Thread::ThreadPool::GetInstance().RequestStop(bGood ? 0 : 1);
}
