#include "oi_http.h"
#include "oi_channel.h"
#include "oi_net_os.h"
#include "oi_poll.h"
#include "oi_rpc_coro.h"
#include "oi_socket.h"
#include "oi_thread.h"
#include "oi_timer_manager.h"
#include "uv.h"
#include <atomic>
#include <cstdio>
#include <cstring>

namespace OI { namespace Http {

namespace
{
const size_t kMaxHeader = 65536;
const size_t kMaxBody = 8 * 1024 * 1024;

std::string EncodeQuery(const std::string& value)
{
	static const char* hex = "0123456789ABCDEF";
	std::string out;
	out.reserve(value.size());
	for (size_t i = 0; i < value.size(); ++i)
	{
		unsigned char c = (unsigned char)value[i];
		bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
			|| c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')';
		if (plain)
		{
			out.push_back((char)c);
		}
		else
		{
			out.push_back('%');
			out.push_back(hex[c >> 4]);
			out.push_back(hex[c & 15]);
		}
	}
	return out;
}

bool EqName(const std::string& text, size_t pos, size_t end, const char* name)
{
	size_t n = std::strlen(name);
	if (pos + n > end)
	{
		return false;
	}
	for (size_t i = 0; i < n; ++i)
	{
		char c = text[pos + i];
		if (c >= 'A' && c <= 'Z')
		{
			c = (char)(c - 'A' + 'a');
		}
		if (c != name[i])
		{
			return false;
		}
	}
	return true;
}

bool ParseU64(const std::string& text, size_t pos, size_t end, uint64_t& out)
{
	if (pos >= end || text[pos] < '0' || text[pos] > '9')
	{
		return false;
	}
	uint64_t value = 0;
	for (size_t i = pos; i < end; ++i)
	{
		char c = text[i];
		if (c < '0' || c > '9')
		{
			break;
		}
		if (value > (UINT64_MAX - 9) / 10)
		{
			return false;
		}
		value = value * 10 + (uint64_t)(c - '0');
	}
	out = value;
	return true;
}

// chunk 行里分号后面是扩展，忽略。
bool ParseHex(const std::string& text, size_t pos, size_t end, uint64_t& out)
{
	uint64_t value = 0;
	size_t digits = 0;
	for (size_t i = pos; i < end; ++i)
	{
		char c = text[i];
		int d = -1;
		if (c >= '0' && c <= '9')
		{
			d = c - '0';
		}
		else if (c >= 'a' && c <= 'f')
		{
			d = c - 'a' + 10;
		}
		else if (c >= 'A' && c <= 'F')
		{
			d = c - 'A' + 10;
		}
		if (d < 0)
		{
			break;
		}
		if (value > (UINT64_MAX >> 4))
		{
			return false;
		}
		value = (value << 4) | (uint64_t)d;
		digits += 1;
	}
	out = value;
	return digits > 0;
}
}

struct Call::State
{
	enum Phase
	{
		PHASE_CONNECT = 0,
		PHASE_WRITE = 1,
		PHASE_READ = 2,
	};

	std::string host;
	int32_t port;
	int32_t timeout_ms;
	std::string request;
	std::string incoming;
	std::string body;
	std::string error;
	size_t sent;
	size_t header_bytes;
	uint64_t content_length;
	int32_t status;
	int32_t phase;
	bool headers_ready;
	bool has_length;
	bool chunked;
	size_t chunk_pos;
	bool failed;
	bool in_io;
	net_socket_t fd;
	HttpChannel* channel;
	std::coroutine_handle<> handle;
	std::atomic<int32_t> done;
	std::atomic<int32_t> woke;
	// Call、超时定时器、域名解析各持一份。定时器不能取消，请求结束后它还会来。
	std::atomic<int32_t> refs;
	uv_getaddrinfo_t dns;

	State()
		: port(0), timeout_ms(5000), sent(0), header_bytes(0), content_length(0), status(0), phase(PHASE_CONNECT),
		headers_ready(false), has_length(false), chunked(false), chunk_pos(0), failed(false), in_io(false),
		fd(INVALID_SOCKET_ID), channel(NULL), handle(), done(0), woke(0), refs(1)
	{
	}
	~State();

	void Retain()
	{
		refs.fetch_add(1);
	}

	void Release()
	{
		if (refs.fetch_sub(1) == 1)
		{
			delete this;
		}
	}

	void Wake()
	{
		if (woke.exchange(1) != 0)
		{
			return;
		}
		if (handle)
		{
			coro::Schedule(handle);
		}
	}

	void DropChannel();

	void Complete()
	{
		if (done.exchange(1) != 0)
		{
			return;
		}
		if (in_io)
		{
			return;
		}
		DropChannel();
		Wake();
	}

	void Fail(const char* why)
	{
		// 都在 poll 线程上调。已经收完的结果不能被晚到的超时改掉。
		if (done.load() != 0)
		{
			return;
		}
		failed = true;
		if (error.empty() && why)
		{
			error = why;
		}
		Complete();
	}

	bool ParseHeaders(size_t header_end)
	{
		size_t line_end = incoming.find("\r\n");
		if (line_end == std::string::npos || line_end > header_end)
		{
			return false;
		}
		size_t sp = incoming.find(' ');
		if (sp == std::string::npos || sp >= line_end)
		{
			return false;
		}
		size_t code_at = sp + 1;
		while (code_at < line_end && incoming[code_at] == ' ')
		{
			code_at += 1;
		}
		uint64_t code = 0;
		if (!ParseU64(incoming, code_at, line_end, code) || code == 0 || code > 999)
		{
			return false;
		}
		status = (int32_t)code;
		size_t pos = line_end + 2;
		while (pos < header_end)
		{
			size_t end = incoming.find("\r\n", pos);
			if (end == std::string::npos || end > header_end)
			{
				break;
			}
			size_t colon = incoming.find(':', pos);
			if (colon != std::string::npos && colon < end)
			{
				size_t value = colon + 1;
				while (value < end && incoming[value] == ' ')
				{
					value += 1;
				}
				size_t name_end = colon;
				while (name_end > pos && incoming[name_end - 1] == ' ')
				{
					name_end -= 1;
				}
				if (EqName(incoming, pos, name_end, "content-length") && pos + std::strlen("content-length") == name_end)
				{
					uint64_t len = 0;
					if (!ParseU64(incoming, value, end, len) || len > kMaxBody)
					{
						return false;
					}
					has_length = true;
					content_length = len;
				}
				else if (EqName(incoming, pos, name_end, "transfer-encoding") && pos + std::strlen("transfer-encoding") == name_end)
				{
					std::string coding = incoming.substr(value, end - value);
					for (size_t i = 0; i < coding.size(); ++i)
					{
						if (coding[i] >= 'A' && coding[i] <= 'Z')
						{
							coding[i] = (char)(coding[i] - 'A' + 'a');
						}
					}
					if (coding.find("chunked") != std::string::npos)
					{
						chunked = true;
					}
				}
			}
			pos = end + 2;
		}
		return true;
	}

	// 头或正文已经收齐就结束。返回 true 表示这一轮不用再读。
	bool Consume()
	{
		if (!headers_ready)
		{
			size_t header_end = incoming.find("\r\n\r\n");
			if (header_end == std::string::npos)
			{
				if (incoming.size() > kMaxHeader)
				{
					Fail("header");
					return true;
				}
				return false;
			}
			if (!ParseHeaders(header_end))
			{
				Fail("status");
				return true;
			}
			headers_ready = true;
			header_bytes = header_end + 4;
			chunk_pos = header_bytes;
		}
		if (chunked)
		{
			return ConsumeChunks();
		}
		if (!has_length)
		{
			return false;
		}
		if (incoming.size() - header_bytes < content_length)
		{
			return false;
		}
		body.assign(incoming, header_bytes, (size_t)content_length);
		Complete();
		return true;
	}

	// 有 chunked 时不看 Content-Length。chunk_pos 停在下一个 chunk 的长度行。
	bool ConsumeChunks()
	{
		for (;;)
		{
			size_t line_end = incoming.find("\r\n", chunk_pos);
			if (line_end == std::string::npos)
			{
				if (incoming.size() - chunk_pos > 1024)
				{
					Fail("chunk");
					return true;
				}
				return false;
			}
			uint64_t size = 0;
			if (!ParseHex(incoming, chunk_pos, line_end, size))
			{
				Fail("chunk");
				return true;
			}
			size_t data_at = line_end + 2;
			if (size == 0)
			{
				// 最后一块后面可以跟 trailer，以空行结束。
				size_t pos = data_at;
				for (;;)
				{
					size_t end = incoming.find("\r\n", pos);
					if (end == std::string::npos)
					{
						return false;
					}
					if (end == pos)
					{
						Complete();
						return true;
					}
					pos = end + 2;
				}
			}
			if (size > kMaxBody || body.size() + (size_t)size > kMaxBody)
			{
				Fail("size");
				return true;
			}
			if (incoming.size() < data_at + (size_t)size + 2)
			{
				return false;
			}
			body.append(incoming, data_at, (size_t)size);
			chunk_pos = data_at + (size_t)size + 2;
		}
	}

	void OnEof()
	{
		if (Consume())
		{
			return;
		}
		if (!headers_ready || has_length || chunked)
		{
			Fail("eof");
			return;
		}
		body.assign(incoming, header_bytes, std::string::npos);
		Complete();
	}

	void PumpRead()
	{
		char buf[16384];
		for (;;)
		{
			int n = NetOs_Recv(fd, buf, sizeof(buf));
			if (n > 0)
			{
				if (incoming.size() + (size_t)n > kMaxHeader + kMaxBody)
				{
					Fail("size");
					return;
				}
				incoming.append(buf, (size_t)n);
				if (Consume())
				{
					return;
				}
				continue;
			}
			if (n == 0)
			{
				OnEof();
				return;
			}
			if (errno == EINTR)
			{
				continue;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				return;
			}
			Fail("recv");
			return;
		}
	}

	void PumpWrite()
	{
		while (sent < request.size())
		{
			int n = NetOs_Send(fd, request.data() + sent, request.size() - sent);
			if (n > 0)
			{
				sent += (size_t)n;
				continue;
			}
			if (n < 0 && errno == EINTR)
			{
				continue;
			}
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			{
				Arm(::OI::Net::IO_EV_WRITE);
				return;
			}
			Fail("send");
			return;
		}
		phase = PHASE_READ;
		Arm(::OI::Net::IO_EV_READ);
	}

	void Arm(int32_t mask);

	void OnWritable()
	{
		if (done.load() != 0)
		{
			return;
		}
		in_io = true;
		if (phase == PHASE_CONNECT)
		{
			int err = NetOs_GetSocketError(fd);
			if (err == EINPROGRESS || err == EINTR || err == EAGAIN || err == EWOULDBLOCK)
			{
				in_io = false;
				return;
			}
			if (err != 0)
			{
				Fail("connect");
				in_io = false;
				return;
			}
			phase = PHASE_WRITE;
		}
		if (phase == PHASE_WRITE)
		{
			PumpWrite();
		}
		in_io = false;
	}

	void OnReadable()
	{
		if (done.load() != 0)
		{
			return;
		}
		if (phase != PHASE_READ)
		{
			in_io = true;
			Fail("connect");
			in_io = false;
			return;
		}
		in_io = true;
		PumpRead();
		in_io = false;
	}

	static void OnTimeout(void* arg);
	static void Start(void* arg);
	static void OnResolved(uv_getaddrinfo_t* req, int status, struct addrinfo* res);
	void Connect(const sockaddr_in& addr);
};

class HttpChannel : public ::OI::Net::Channel
{
	Call::State* _state;
public:
	HttpChannel(::OI::Net::NetSocket sock, Call::State* state)
		: ::OI::Net::Channel(sock, NULL), _state(state)
	{
	}
	void DataIn()
	{
		_state->OnReadable();
	}
	void DataOut()
	{
		_state->OnWritable();
	}
	bool TryClose()
	{
		if (_state->done.load() == 0)
		{
			return false;
		}
		_state->channel = NULL;
		_state->fd = INVALID_SOCKET_ID;
		Call::State* state = _state;
		delete this;
		state->Wake();
		return true;
	}
};

void Call::State::Arm(int32_t mask)
{
	if (!channel)
	{
		return;
	}
	bool read = (mask & ::OI::Net::IO_EV_READ) != 0;
	bool write = (mask & ::OI::Net::IO_EV_WRITE) != 0;
	if (channel->_poll_token == NULL)
	{
		::OI::Net::Poll::GetInstance().IOEventRegister(channel, read, write);
		return;
	}
	::OI::Net::Poll::GetInstance().IOEventMod(channel, mask);
}

void Call::State::DropChannel()
{
	if (channel)
	{
		HttpChannel* held = channel;
		channel = NULL;
		fd = INVALID_SOCKET_ID;
		delete held;
		return;
	}
	if (fd != INVALID_SOCKET_ID)
	{
		NetOs_Close(fd);
		fd = INVALID_SOCKET_ID;
	}
}

void Call::State::OnTimeout(void* arg)
{
	State* self = (State*)arg;
	self->Fail("timeout");
	self->Release();
}

void Call::State::Start(void* arg)
{
	State* self = (State*)arg;
	if (self->port <= 0 || self->port > 65535 || self->host.empty())
	{
		self->Fail("host");
		return;
	}
	int32_t timeout = self->timeout_ms > 0 ? self->timeout_ms : 5000;
	self->Retain();
	::OI::Timer::BaseTimerManager::GetInstance().AddTimer([self]() {
		::OI::Net::Poll::GetInstance().CallOnLoop(State::OnTimeout, self);
	}, timeout);
	sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)self->port);
	if (inet_pton(AF_INET, self->host.c_str(), &addr.sin_addr) == 1)
	{
		self->Connect(addr);
		return;
	}
	// 域名交给 libuv 的线程池解析，回调回到 poll 线程，不卡 poll。
	addrinfo hints;
	std::memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	self->dns.data = self;
	self->Retain();
	if (uv_getaddrinfo(::OI::Net::Poll::GetInstance().Loop(), &self->dns, State::OnResolved, self->host.c_str(), NULL, &hints) != 0)
	{
		self->Fail("dns");
		self->Release();
	}
}

void Call::State::OnResolved(uv_getaddrinfo_t* req, int status, struct addrinfo* res)
{
	State* self = (State*)req->data;
	if (self->done.load() == 0)
	{
		if (status != 0 || res == NULL || res->ai_addr == NULL || res->ai_family != AF_INET)
		{
			self->Fail("dns");
		}
		else
		{
			sockaddr_in addr = *(const sockaddr_in*)res->ai_addr;
			addr.sin_port = htons((uint16_t)self->port);
			self->Connect(addr);
		}
	}
	uv_freeaddrinfo(res);
	self->Release();
}

void Call::State::Connect(const sockaddr_in& addr)
{
	fd = NetOs_CreateTcp(true);
	if (fd == INVALID_SOCKET_ID)
	{
		Fail("socket");
		return;
	}
	NetOs_SetNoDelay(fd);
	::OI::Net::NetSocket sock;
	sock.Attach(fd);
	channel = new HttpChannel(sock, this);
	int rc = -1;
	for (;;)
	{
		rc = NetOs_Connect(fd, (const sockaddr*)&addr, (int)sizeof(addr));
		if (rc == 0 || errno != EINTR)
		{
			break;
		}
	}
	if (rc != 0 && errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK)
	{
		Fail("connect");
		return;
	}
	if (rc != 0)
	{
		phase = PHASE_CONNECT;
		Arm(::OI::Net::IO_EV_WRITE);
		return;
	}
	phase = PHASE_WRITE;
	PumpWrite();
}

Call::State::~State()
{
	DropChannel();
}

#ifdef _SINGLE_THREAD_
static bool DrainSt()
{
	return coro::StScheduler::Poll();
}

static bool PendingSt()
{
	return coro::StScheduler::Queued();
}

static struct StReg
{
	StReg()
	{
		::OI::Thread::ThreadPool::GetInstance().SetDrain(DrainSt);
		::OI::Thread::ThreadPool::GetInstance().SetPending(PendingSt);
	}
} g_st_reg;
#endif

Call::~Call()
{
	if (_state)
	{
		_state->Release();
	}
}

void Call::await_suspend(std::coroutine_handle<> handle) noexcept
{
	_state->handle = handle;
	::OI::Net::Poll::GetInstance().CallOnLoop(State::Start, _state);
}

Response Call::await_resume()
{
	Response out;
	out.status = _state->status;
	out.body = std::move(_state->body);
	out.fail = _state->failed || _state->status == 0;
	out.error = std::move(_state->error);
	_state->Release();
	_state = nullptr;
	return out;
}

Client::Client(const std::string& host, int32_t port, int32_t timeout_ms)
	: _host(host), _port(port), _timeout_ms(timeout_ms)
{
}

static std::string HostHeader(const std::string& host, int32_t port)
{
	std::string line = "Host: ";
	line += host;
	if (port != 80)
	{
		line += ":";
		line += std::to_string(port);
	}
	line += "\r\n";
	return line;
}

Call Client::Get(const std::string& path, const std::vector<std::pair<std::string, std::string>>& query)
{
	std::string target = path.empty() ? "/" : path;
	if (!query.empty())
	{
		target.push_back('?');
		for (size_t i = 0; i < query.size(); ++i)
		{
			if (i != 0)
			{
				target.push_back('&');
			}
			target += EncodeQuery(query[i].first);
			target.push_back('=');
			target += EncodeQuery(query[i].second);
		}
	}
	Call::State* state = new Call::State();
	state->host = _host;
	state->port = _port;
	state->timeout_ms = _timeout_ms;
	state->request = "GET " + target + " HTTP/1.1\r\n";
	state->request += HostHeader(_host, _port);
	state->request += "Connection: close\r\n\r\n";
	return Call(state);
}

Call Client::Post(const std::string& path, const std::string& body, const std::string& content_type)
{
	std::string target = path.empty() ? "/" : path;
	Call::State* state = new Call::State();
	state->host = _host;
	state->port = _port;
	state->timeout_ms = _timeout_ms;
	state->request = "POST " + target + " HTTP/1.1\r\n";
	state->request += HostHeader(_host, _port);
	state->request += "Content-Type: " + content_type + "\r\n";
	state->request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
	state->request += "Connection: close\r\n\r\n";
	state->request.append(body.data(), body.size());
	return Call(state);
}

}}
