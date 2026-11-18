#pragma once
#include <coroutine>
#include <cstdint>
#include "oi_http_message.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace OI
{
namespace Http
{

// 一次请求。co_await 挂起，poll 线程上收完再 Schedule 回来，不在回调里 resume。
class Call
{
	friend class Client;

  public:
	struct Request;

  private:
	// 这一次请求的路径、正文和完成门。await 期间必须还活着。
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

struct TlsOptions
{
	std::string sCaFile; // 留空使用系统信任根，也可指定 PEM 格式 CA 文件。
};

struct ClientOptions
{
	int32_t iTimeoutMilliseconds = 5000;
	std::string sCaFile;
};

// A bound client holds only request defaults. Connections belong to the process-wide pool.
class Client
{
	std::string m_sHost;
	int32_t m_iPort;
	ClientOptions m_stOptions;
	bool m_bHttps;
	bool m_bReuse;
	Call Build(const Request& stRequest, std::string_view sPath) const;

  public:
	static Call GetUrl(std::string_view sUrl, const ClientOptions& stOptions = {});
	static Call PostUrl(std::string_view sUrl, const std::string& sBody, const std::string& sContentType, const ClientOptions& stOptions = {});
	static Call SendUrl(std::string_view sUrl, const Request& stRequest, const ClientOptions& stOptions = {});
	Client(const std::string& sHost, int32_t iPort, int32_t iTimeoutMilliseconds = 5000, bool bReuse = true);
	Client(const std::string& sHost, int32_t iPort, const TlsOptions& stTls, int32_t iTimeoutMilliseconds = 5000, bool bReuse = true);

	Call Send(const Request& stRequest);

	// 发 GET。查询参数按百分号编码。
	Call Get(const std::string& sPath, const std::vector<std::pair<std::string, std::string>>& vQuery = {});
	// 发 POST。正文按字节送出，Content-Type 用调用方给的。
	Call Post(const std::string& sPath, const std::string& sBody, const std::string& sContentType);
};

}
}
