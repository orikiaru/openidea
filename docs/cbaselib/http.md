# HTTP 客户端

客户端和服务端都在 `OI::Http`。`webserver` 使用框架会话管理器接收请求，处理函数以协程执行。标准例子是 [`web_client.cpp`](../../modules/webclient/src/web_client.cpp#L45)。

## 接口

| 用途 | 声明 |
| --- | --- |
| 客户端 | [`Client`](../../cbaselib/http/inc/oi_http.h) |
| 通用请求 | [`Client::Send`](../../cbaselib/http/inc/oi_http.h) |
| 请求 | [`Request`](../../cbaselib/http/inc/oi_http_message.h) |
| GET | [`Client::Get`](../../cbaselib/http/inc/oi_http.h) |
| POST | [`Client::Post`](../../cbaselib/http/inc/oi_http.h) |
| 一次可等待的请求 | [`Call`](../../cbaselib/http/inc/oi_http.h) |
| 响应 | [`Response`](../../cbaselib/http/inc/oi_http_message.h) |

```cpp
::OI::Http::Client stClient(sHost, iPort);
::OI::Http::Request stRequest;
stRequest.sMethod = "POST";
stRequest.sPath = "/bin";
stRequest.sBody = sBody;
stRequest.sContentType = "application/octet-stream";
stRequest.vHeaders.emplace_back("X-Echo-Tag", "sample-tag");
::OI::Http::Response stGot = co_await stClient.Send(stRequest);
if (stGot.iCode != OI_OK)
{
	co_return stGot.iCode;
}
```

> **注意：** `host` 可以是 IPv4、IPv6 字面地址或域名。IPv6 的 `Host` 头会带方括号。域名在 libuv 的线程池里解析，不占 poll。

> **注意：** 同一个 `Client` 默认复用一条连接，请求串行。构造时 `bReuse` 传 `false`，这条请求才写 `Connection: close`。

> **注意：** `iCode` 是操作错误码：成功为 `OI_OK`，连接失败为 `OI_ERR_CONNECT`，超时为 `OI_ERR_TIMEOUT`，无效 host 为 `OI_ERR_INVALID_ARGUMENT`，报文解析失败或截断为 `OI_ERR_PROTOCOL`；发送失败保留原始错误码。HTTP 4xx/5xx 本身不算失败，状态码在 [`iStatus`](../../cbaselib/http/inc/oi_http_message.h)，正文在 [`sBody`](../../cbaselib/http/inc/oi_http_message.h)。

> **注意：** 单线程模块链 `http_static`，多线程链 `http_m_static`。完成回调里不直接 `resume`，由 poll 线程 `Schedule` 回来。

请求和响应共享有序 `vHeaders`，保留重复字段；`GetHeader` 大小写不敏感，返回首个匹配项的指针，缺失时返回空指针；需要全部值时遍历 `vHeaders`。指针随对应消息对象及其 header 容器的修改或销毁失效。`Send` 目前支持 GET 和 POST；`Get`、`Post` 保留。请求自定义头不允许覆盖 `Host`、`Connection`、`Content-Length`、`Transfer-Encoding`、`Content-Type`、`Trailer`、`Upgrade` 或 `Expect`，正文类型由 `sContentType` 指定。非法方法、请求目标或自定义头等输入错误在 `co_await` 返回 `OI_ERR_INVALID_ARGUMENT`，状态码为 0，请求不会发送。服务端 handler 也使用同一消息类型。

当前正文整体缓冲；表单、流式收发和 TLS 尚未提供专门接口，后续分期见 [待办](../todo.md)。

## Cookie

`Request::AddCookie(name, value)` 和 `Request::GetCookies(output)` 处理请求 Cookie；`Response::AddCookie(cookie)` 和 `Response::GetCookies(output)` 处理响应 Set-Cookie。均返回 `int32_t`，失败时保持输出不变。重复项保留顺序和同名值，Cookie 名区分大小写；属性名解析不区分大小写。`Cookie` 包含 domain、path、expires、可选 max-age、secure、httponly 和 SameSite，`iMaxAge` 区分未指定与 0。

值默认是原始 wire cookie-octets，不自动编码或解码。任意字节先用 `Cookie::EncodeValue` 编码，再传给 `AddCookie`；读取后显式调用 `Cookie::DecodeValue`，先检查返回码再使用输出。客户端不自动保存或附加 Cookie，跨请求由调用方管理。

## Query 参数

`Request::mQuery` 按添加顺序保存键值对，`Add(name, value)` 追加而不覆盖同名项。`Get(name)` 返回首个值的指针，缺键返回空指针；`GetAll(name)` 返回按原序排列的所有值，缺键返回空集合。裸 key 和 `key=` 都解析为空串。指针在容器修改或销毁后不再保证有效。

`Client::Get(path, vector<pair<string, string>>)` 和 `Client::Send(request)` 都支持重复参数；路径已带查询时追加参数并编码新键值，保留已有查询内容。服务端 `/query` 保持 `text` 为首个值；有多个 `text` 时额外返回有序 `texts` 数组。旧的单值调用保持兼容。
