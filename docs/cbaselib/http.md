# HTTP 客户端

只有客户端，在 `OI::Http`。服务端不在这个库里，`webserver` 仍用 `cpp-httplib`。标准例子是 [`web_client.cpp`](../../modules/webclient/src/web_client.cpp#L45)。

## 接口

| 用途 | 声明 |
| --- | --- |
| 客户端 | [`Client`](../../cbaselib/http/inc/oi_http.h#L64) |
| GET | [`Client::Get`](../../cbaselib/http/inc/oi_http.h#L80) |
| POST | [`Client::Post`](../../cbaselib/http/inc/oi_http.h#L82) |
| 一次可等待的请求 | [`Call`](../../cbaselib/http/inc/oi_http.h#L34) |
| 响应 | [`Response`](../../cbaselib/http/inc/oi_http.h#L15) |

```cpp
::OI::Http::Client stClient(sHost, iPort);
::OI::Http::Response stGot = co_await stClient.Get("/query", vQuery);
if (stGot.bFail)
{
	co_return OI_ERR_FAIL;
}
```

> **注意：** `host` 可以是 IPv4、IPv6 字面地址或域名。IPv6 的 `Host` 头会带方括号。域名在 libuv 的线程池里解析，不占 poll。

> **注意：** 同一个 `Client` 默认复用一条连接，请求串行。构造时 `bReuse` 传 `false`，这条请求才写 `Connection: close`。

> **注意：** `bFail` 表示连接、超时或报文不完整。HTTP 4xx/5xx 本身不算失败，状态码在 [`iStatus`](../../cbaselib/http/inc/oi_http.h#L18)，正文在 [`sBody`](../../cbaselib/http/inc/oi_http.h#L20)。

> **注意：** 单线程模块链 `http_static`，多线程链 `http_m_static`。完成回调里不直接 `resume`，由 poll 线程 `Schedule` 回来。
