#pragma once
#include <coroutine>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace OI { namespace Http {

// status 为 0 表示没拿到 HTTP 状态行。fail 为真是连接、超时或报文不完整。
struct Response
{
	int32_t iStatus;
	std::string sBody;
	bool bFail;
	std::string sError;
	Response() : iStatus(0), bFail(true) {}
};

class HttpChannel;

// 一次请求。co_await 挂起，poll 线程上收完再 Schedule 回来，不在回调里 resume。
class Call
{
	friend class Client;
	friend class HttpChannel;
	struct State;
	State* m_pState;
	explicit Call(State* pStateParam) : m_pState(pStateParam) {}
public:
	Call(Call&& stOtherParam) noexcept : m_pState(stOtherParam.m_pState) { stOtherParam.m_pState = nullptr; }
	Call(const Call&) = delete;
	Call& operator=(const Call&) = delete;
	~Call();
	bool await_ready() const noexcept { return false; }
	void await_suspend(std::coroutine_handle<> stHandleParam) noexcept;
	Response await_resume();
};

// 连一个 HTTP/1.1 服务。host 可以是 IPv4 或域名，域名在 libuv 线程池里解析。
// 查询参数按百分号编码，body 按字节发送。响应支持 Content-Length、chunked 和读到断开。
class Client
{
	std::string m_sHost;
	int32_t m_iPort;
	int32_t m_iTimeoutMilliseconds;
public:
	Client(const std::string& sHostParam, int32_t iPortParam, int32_t iTimeoutMillisecondsParam = 5000);
	Call Get(const std::string& sPathParam, const std::vector<std::pair<std::string, std::string>>& vQueryParam);
	Call Post(const std::string& sPathParam, const std::string& sBodyParam, const std::string& sContentTypeParam);
};

}}
