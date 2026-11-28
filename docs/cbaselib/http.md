# HTTP 客户端

客户端和服务端都在 `OI::Http`。`webserver` 使用框架会话管理器接收请求，处理函数以协程执行。标准例子是 [`web_client.cpp`](../../modules/webclient/src/web_client.cpp#L45)。

## 接口

| 用途 | 声明 |
| --- | --- |
| GET 完整 URL | [`Client::GetUrl`](../../cbaselib/http/inc/oi_http.h) |
| POST 完整 URL | [`Client::PostUrl`](../../cbaselib/http/inc/oi_http.h) |
| 自定义 URL 请求 | [`Client::SendUrl`](../../cbaselib/http/inc/oi_http.h) |
| 请求选项 | [`ClientOptions`](../../cbaselib/http/inc/oi_http.h) |
| 已有配置的绑定客户端 | [`Client`](../../cbaselib/http/inc/oi_http.h) |
| 请求 | [`Request`](../../cbaselib/http/inc/oi_http_message.h) |
| 一次可等待的请求 | [`Call`](../../cbaselib/http/inc/oi_http.h) |
| 响应 | [`Response`](../../cbaselib/http/inc/oi_http_message.h) |

```cpp
::OI::Http::Response stResponse = co_await ::OI::Http::Client::GetUrl(sUrl);
if (stResponse.iCode != OI_OK)
{
    co_return stResponse.iCode;
}
```

需要新连接或自定义请求时可用 `SendUrl`：

```cpp
::OI::Http::Request stRequest;
stRequest.sMethod = "POST";
stRequest.sBody = sBody;
stRequest.sContentType = "application/octet-stream";
stRequest.bFreshConnection = true;
::OI::Http::Response stResponse = co_await ::OI::Http::Client::SendUrl(sUrl, stRequest);
if (stResponse.iCode != OI_OK)
{
    co_return stResponse.iCode;
}
```

已有 host、端口配置的调用方仍可构造 `Client`，使用 `Get`、`Post`、`Send`。

> **注意：** `host` 可以是 IPv4、IPv6 字面地址或域名。IPv6 的 `Host` 头会带方括号。域名在 libuv 的线程池里解析，不占 poll。

> **注意：** 客户端默认复用进程共享池中的空闲连接，并发请求使用独立会话。构造时 `bReuse` 传 `false`，请求写 `Connection: close` 并使用新连接。

> **注意：** `iCode` 是操作错误码：成功为 `OI_OK`，连接失败为 `OI_ERR_CONNECT`，超时为 `OI_ERR_TIMEOUT`，无效 host 为 `OI_ERR_INVALID_ARGUMENT`，报文解析失败或截断为 `OI_ERR_PROTOCOL`；发送失败保留原始错误码。HTTP 4xx/5xx 本身不算失败，状态码在 [`iStatus`](../../cbaselib/http/inc/oi_http_message.h)，正文在 [`sBody`](../../cbaselib/http/inc/oi_http_message.h)。

> **注意：** 单线程模块链 `http_static`，多线程链 `http_m_static`。完成回调里不直接 `resume`，由 poll 线程 `Schedule` 回来。

请求和响应共享有序 `vHeaders`，保留重复字段；`GetHeader` 大小写不敏感，返回首个匹配项的指针，缺失时返回空指针；需要全部值时遍历 `vHeaders`。有名的 const 或非 const 左值可取指针，临时消息调用在编译期被拒绝；该限制由共同的消息基类提供。指针是借用，随对应消息对象及其 header 容器的修改或销毁失效。`Send` 目前支持 GET 和 POST；`Get`、`Post` 保留。请求自定义头不允许覆盖 `Host`、`Connection`、`Content-Length`、`Transfer-Encoding`、`Content-Type`、`Trailer`、`Upgrade` 或 `Expect`，正文类型由 `sContentType` 指定。非法方法、请求目标或自定义头等输入错误在 `co_await` 返回 `OI_ERR_INVALID_ARGUMENT`，状态码为 0，请求不会发送。服务端 handler 也使用同一消息类型。

请求和响应中的多个 `Connection` 字段及逗号分隔的 token 会一起判断：只要出现 `close`，就关闭连接，即使其他字段包含 `keep-alive`。chunked 正文的 chunk size 行最多 1024 字节，行尾的 CRLF 不计入上限。

当前正文默认整体缓冲。HTTP 使用独立接收额度，不跟随普通 TCP/RPC 的收包默认值（64 MiB + 8）或组件配置：客户端保留 `kMaxHeader + kMaxBody`（64 KiB + 8 MiB），服务端也保留自己的头部/正文限制；普通缓冲正文上限仍为 8 MiB。此额度不是预分配大小或精确 RSS 上限；流式正文另见下节接口。

## HTTPS

使用 TLS 构造重载启用 HTTPS，始终校验证书链和请求主机名或 IP。空 CA 文件在 Linux 使用 OpenSSL 默认信任路径，在 Windows 使用系统证书存储；也可以指定可信 CA 文件：

```cpp
::OI::Http::Client stClient(sHost, iPort, ::OI::Http::TlsOptions{sCaFile}, 5000, true);
```

请求超时覆盖建连、握手和响应；验证、握手及连接失败应检查返回的错误码和错误信息。TLS 传输由可独立复用的 `OI::Net::TlsSession` 提供，网络回调与 TLS 写入在 poll 线程执行；其他协议需要 TLS 时可链接单线程 `tls_static` 或多线程 `tls_m_static`。服务端不提供 TLS，可由反向代理终止 TLS。构建依赖 OpenSSL 开发文件：Linux 要求 3.0 或更高，Windows 要求 3.2 或更高以支持系统 ROOT 证书存储。Windows 配置 CMake 时可用 `OPENSSL_ROOT_DIR` 指定 OpenSSL 安装目录。动态链接部署时须提供匹配的 OpenSSL 运行库，静态链接无需单独分发 OpenSSL DLL；使用私有 CA 时将 CA 证书文件部署到客户端可读取的位置，并通过 `ca_file` 或 `TlsOptions` 指定，证书轮换由部署方管理。

## URL 单次请求

可以直接用 `Client::GetUrl(url[, options])`、`Client::PostUrl(url, body, contentType[, options])` 或 `Client::SendUrl(url, request[, options])` 发起单次请求。`ClientOptions` 的 `iTimeoutMilliseconds` 控制请求超时，`sCaFile` 指定 HTTPS 的可信 CA 文件；结果仍先检查 `iCode`，再读取响应。`SendUrl` 使用 URL 中的路径和查询，`Request::sPath` 不覆盖 URL 路径。

绑定客户端和 URL 请求共用进程级 HTTP 会话管理器，并发请求使用独立会话；HTTP、HTTPS 及不同 CA 配置不混用连接；限额按 scheme、规范化 host 和 port 统计，同一目标的不同 CA 配置共享该限额。需要本次请求使用新连接时，设置 `Request::bFreshConnection`。`[HttpClientPool] max_connections_per_server` 默认 4，限制每个目标的连接数；达到上限时排队，请求超时也覆盖排队时间。`max_waiting_per_server` 默认 1024，`max_waiting_bytes_per_server` 默认 67108864，以编码后的请求头和正文计数，任一配置为 0 禁止排队，超过限额返回 `OI_ERR_BUSY`。`idle_timeout_ms` 默认 60000，空闲连接由秒级周期清理。新连接也计入上限，达到上限时需先回收空闲连接才能创建。

域名解析返回多个地址时，HTTP 可在请求发送之前连接失败后尝试下一个地址；请求发送后不自动重放。服务端对 `Expect: 100-continue` 先发送临时响应再接收正文，未知 expectation 返回 417。

## 服务端连接与请求时限

服务端组件的 `idle_timeout_ms` 默认 60000 毫秒：连接后一直没有首字节，以及完整请求和响应后的 keep-alive 空闲连接，达到该时限后回收；处理中的 handler、半帧、暂停读取或仍有待发送数据的连接不按普通空闲连接回收。

普通请求从检测到未完成帧起使用 `incomplete_frame_timeout_ms`（默认 10000 毫秒），到期仍未完成则关闭连接。流式 source/sink 使用独立的无进度计时：连续 `idle_timeout_ms` 没有产出或消费进度则关闭，慢但持续取得进度会刷新时限；回调返回 `OI_ERR_BUSY` 不等于消费当前片。

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

包含 `oi_http_json.h` 后，`Message::SetJson(value)` 将 rpcdata、标量或容器编码到正文并设置 `application/json`。`Message::GetJson(output)` 按内容类型和目标类型解码；支持 `application/json`（可带参数）及 `application/*+json`。媒体类型不符返回 `OI_ERR_PROTOCOL`，正文超限、语法或类型错误返回 `OI_ERR_DECODE`；失败时输出保持原值。编码时拒绝非有限浮点数和无效 UTF-8（包括不符合 UTF-8 的原始二进制字段），返回 `OI_ERR_ENCODE` 并保持原消息；解码时拒绝无效 UTF-8，返回 `OI_ERR_DECODE` 并保持输出。合法 UTF-8、Unicode 和字符串内 NUL 支持往返；任意二进制需由调用方先编码。解码到 `float` 时超出其有限范围的数值会被拒绝；整数键 map 中不同文本键若转成相同整数，也会返回 `OI_ERR_DECODE`。调用方应先检查返回码再使用输出。
