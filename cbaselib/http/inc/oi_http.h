#pragma once
#include <coroutine>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace OI
{
namespace Http
{

// status 为 0 表示没拿到 HTTP 状态行。fail 为真是连接、超时或报文不完整。
struct Response
{
	int32_t iStatus;
	std::string sBody;
	bool bFail;
	std::string sError;
	Response()
	    : iStatus(0), bFail(true)
	{
	}
};

// 一次请求。co_await 挂起，poll 线程上收完再 Schedule 回来，不在回调里 resume。
// 复用时，同一个 Client 上的请求串行走 SessionManager 的一条连接，发完不主动拆。
// 不复用时每条请求带 Connection: close，收完就关，下一条重新连。
class Call
{
	friend class Client;

  public:
	struct Request;

  private:
	std::shared_ptr<Request> m_pRequest;
	explicit Call(std::shared_ptr<Request> pRequest);

  public:
	Call(Call&& stOther) noexcept;
	Call(const Call&) = delete;
	Call& operator=(const Call&) = delete;
	~Call();
	bool await_ready() const noexcept
	{
		return false;
	}
	// 返回 false 表示结果已经写好，调用方不挂起。完成方只在门已经是挂起时 Schedule。
	bool await_suspend(std::coroutine_handle<> stHandle);
	Response await_resume();
};

// 连一个 HTTP/1.1 服务。host 可以是 IPv4 或域名，域名在 libuv 线程池里解析。
// 查询参数按百分号编码，body 按字节发送。响应支持 Content-Length、chunked 和读到断开。
// 外层只排队。连接和收发都在内部的 SessionManager 上，同一时间线上只有一个请求。
// bReuse 为真时复用这条连接；为假时每条请求结束后主动关掉。
class Client
{
  public:
	struct Impl;

  private:
	std::shared_ptr<Impl> m_pImpl;

  public:
	Client(const std::string& sHost, int32_t iPort, int32_t iTimeoutMilliseconds = 5000, bool bReuse = true);
	~Client();
	Client(const Client&) = delete;
	Client& operator=(const Client&) = delete;

	Call Get(const std::string& sPath, const std::vector<std::pair<std::string, std::string>>& vQuery);
	Call Post(const std::string& sPath, const std::string& sBody, const std::string& sContentType);
};

}
}
