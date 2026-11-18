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

当前正文默认整体缓冲；TLS 尚未提供专门接口，后续分期见 [待办](../todo.md)。

## Cookie

`Request::AddCookie(name, value)` 和 `Request::GetCookies(output)` 处理请求 Cookie；`Response::AddCookie(cookie)` 和 `Response::GetCookies(output)` 处理响应 Set-Cookie。均返回 `int32_t`，失败时保持输出不变。重复项保留顺序和同名值，Cookie 名区分大小写；属性名解析不区分大小写。`Cookie` 包含 domain、path、expires、可选 max-age、secure、httponly 和 SameSite，`iMaxAge` 区分未指定与 0。

值默认是原始 wire cookie-octets，不自动编码或解码。任意字节先用 `Cookie::EncodeValue` 编码，再传给 `AddCookie`；读取后显式调用 `Cookie::DecodeValue`，先检查返回码再使用输出。客户端不自动保存或附加 Cookie，跨请求由调用方管理。

## Query 参数

`Request::mQuery` 按添加顺序保存键值对，`Add(name, value)` 追加而不覆盖同名项。`Get(name)` 返回首个值的指针，缺键返回空指针；`GetAll(name)` 返回按原序排列的所有值，缺键返回空集合。裸 key 和 `key=` 都解析为空串。指针在容器修改或销毁后不再保证有效。

`Client::Get(path, vector<pair<string, string>>)` 和 `Client::Send(request)` 都支持重复参数；路径已带查询时追加参数并编码新键值，保留已有查询内容。服务端 `/query` 保持 `text` 为首个值；有多个 `text` 时额外返回有序 `texts` 数组。旧的单值调用保持兼容。

## 表单

`Message::SetForm` / `GetForm` 处理 URL 编码表单，`QueryParams` 保留重复项和顺序；`SetMultipart` / `GetMultipart` 处理 multipart，`FormPart` 的 `sName` 和 `sValue` 保存字段名与字节，`sFilename` 区分普通字段和文件，`sContentType` 指定内容类型。调用后检查 `int32_t` 返回码；文件和文本都支持空值及二进制正文。表单接口处理完整缓冲体，不代表流式上传；编码后正文和解析正文上限均为 8 MiB，最多 1024 项，multipart 每段头上限为 64 KiB。失败时保留原消息或输出参数。

## 流式正文

发送方设置 `pBodySource`，`Read` 写入给定缓冲并返回读取字节数；`OI_OK` 且长度为 0 表示结束，`OI_ERR_BUSY` 表示暂时无法提供数据，框架稍后重试。客户端可在请求的 `pBodySink` 指定响应接收方；服务端用 `RegisterStream` 注册 sink 工厂和处理协程，handler 从请求取得已读完的 sink。`Write` 对每片要么全部消费并返回 `OI_OK`，要么返回 `OI_ERR_BUSY` 等重试，不能部分消费。

单片缓冲上限为 64 KiB；回调不能阻塞 poll，也不能保留传入 span 或字节视图。可通过流对象 `Cancel()` 请求取消；超时和断连也会结束本次流处理。需在回调以外保存共享对象以观察或取消。

## JSON

包含 `oi_http_json.h` 后，`Message::SetJson(value)` 将 rpcdata、标量或容器编码到正文并设置 `application/json`。`Message::GetJson(output)` 按内容类型和目标类型解码；支持 `application/json`（可带参数）及 `application/*+json`。媒体类型不符返回 `OI_ERR_PROTOCOL`，正文超限、语法或类型错误返回 `OI_ERR_DECODE`；失败时输出保持原值。编码时拒绝非有限浮点数和无效 UTF-8（包括不符合 UTF-8 的原始二进制字段），返回 `OI_ERR_ENCODE` 并保持原消息；解码时拒绝无效 UTF-8，返回 `OI_ERR_DECODE` 并保持输出。合法 UTF-8、Unicode 和字符串内 NUL 支持往返；任意二进制需由调用方先编码。调用方应先检查返回码再使用输出。
