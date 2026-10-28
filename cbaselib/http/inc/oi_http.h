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
	int32_t status;
	std::string body;
	bool fail;
	std::string error;
	Response() : status(0), fail(true) {}
};

class HttpChannel;

// 一次请求。co_await 挂起，poll 线程上收完再 Schedule 回来，不在回调里 resume。
class Call
{
	friend class Client;
	friend class HttpChannel;
	struct State;
	State* _state;
	explicit Call(State* state) : _state(state) {}
public:
	Call(Call&& other) noexcept : _state(other._state) { other._state = nullptr; }
	Call(const Call&) = delete;
	Call& operator=(const Call&) = delete;
	~Call();
	bool await_ready() const noexcept { return false; }
	void await_suspend(std::coroutine_handle<> handle) noexcept;
	Response await_resume();
};

// 连一个 HTTP/1.1 服务。host 可以是 IPv4 或域名，域名在 libuv 线程池里解析。
// 查询参数按百分号编码，body 按字节发送。响应支持 Content-Length、chunked 和读到断开。
class Client
{
	std::string _host;
	int32_t _port;
	int32_t _timeout_ms;
public:
	Client(const std::string& host, int32_t port, int32_t timeout_ms = 5000);
	Call Get(const std::string& path, const std::vector<std::pair<std::string, std::string>>& query);
	Call Post(const std::string& path, const std::string& body, const std::string& content_type);
};

}}
