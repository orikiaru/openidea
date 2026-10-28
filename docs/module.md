# 模块和协议

三个 xml 是生成输入，都在 `modules/xmlconfig`。

| 文件 | 作用 |
| --- | --- |
| `module.xml` | 一个 `<module>` 是一个进程 |
| `module_component.xml` | 模块下的网络组件，`type` 为 `Client` 或 `Server`，上面挂 rpc |
| `proto.xml` | 协议正文，写法像 protobuf：`字段号 类型 名字;`。不是 XML |

字段号写在每一行前面，这就是线上的序号。缺了、重了，生成失败，不会按出现顺序补号。跳号直接写下一个数字，例如 `20 int32_t gap;`。

```
rpcdata AllTypesData {
    0 int8_t c;
    9 vector<int32_t> nums;
    13 map<int32_t, string> table;
    20 int32_t gap;
}
```

整数字段写 `int8_t`、`int16_t`、`int32_t`、`int64_t` 和 `uint8_t`、`uint16_t`、`uint32_t`、`uint64_t`。`string` 就是 `std::string`。`map` 的键可以是这些标量、`string`，也可以是名字后面加了 `key` 的 `rpcdata`。`key` 会生成 `operator<`。嵌进别的 `key`，或者再当 `map` 键时，里层也必须有比较器，生成时会检查。标了 `key` 的结构不生成 JSON；谁的字段里放了这种 `map`，谁也不生成 JSON。其余 `rpcdata` 仍生成 `ToJson` / `FromJson`。`rpc` 不生成 JSON。库是 JsonCpp。

cmake 调用 `cbaselib/proto/proto_tool/main.py`，参数是这三个 xml、`modules/include`、`modules`、`deploy`。

## 什么会重写

`modules/include` 里的序列化头和 `rpc_ids.h` 每次 cmake 都重写。改这些文件无效。每条 `rpcdata` 写成 `oi_rpcdata_<名字>.h`，例如 `oi_rpcdata_alltypesdata.h`，都在 `OI::RpcData` 里。序列化基类仍在 `OI::Rpc`。`rpc` 不再另生成一层类，也不把函数体 `#include` 进类里。模块下不建 `protocol` 目录，生成时如果还在就删掉。

下面这些只在文件不存在时生成，已有文件会留下：

- `modules/<name>/src/main.cpp`
- `modules/<name>/CMakeLists.txt`
- 组件的 `inc/*.h` 和 `src/*.cpp`
- `deploy/config/<name>.toml`

根目录 `CMakeLists.txt` 里写死了要编的模块名。新模块除了写 xml，还要把名字加进这个列表，否则不会编。

```cmake
foreach(_mod logd ipcclient ipcserver redisserver webserver webclient)
    if(EXISTS ${MODULES_PATH}/${_mod}/CMakeLists.txt)
        add_subdirectory(${MODULES_PATH}/${_mod})
    endif()
endforeach()
```

## 加一个回声协议

在 `proto.xml` 增加请求和回复两个 `rpcdata`，字段号自己写上，再加一条 `rpc`，请求和回复各一个协议号。

在 `module_component.xml` 里把这条 rpc 挂到对应的 Client 和 Server 组件上。

服务端在组件 `Init` 里 `HandleRpc`，客户端在连上之后 `co_await RpcCall`。写法见 [协程](coroutine.md)。

现成例子是 `AllEcho`：`ipcserver` 把请求原样放进回复，`ipcclient` 连上后按条发出去并核对回复。`Hold` 用来看超时和回包同时到：服务端 `co_await coro::yield(delay_ms)` 再回，客户端按调用号认这一次。

## ipcclient 的测试开关

`deploy/config/ipcclient.toml` 的 `[Test]` 里 `echo`、`log`、`redis`、`hold` 各管一个测试，默认只开回声。一次只开一个。

`log = true` 时日志服必须已经在听，`ipcserver` 也要起：`LogMgr` 是在连上 `ipcserver` 之后才开始发的，连不上就一直不发，CPU 是 0。`SendLog` 失败后测试会立刻再排一次发送，字节数加了又减，到不了目标，进程也不会退出。

## Web

`webserver` 和 `webclient` 是两个独立进程，只走 HTTP，不开 HTTPS。JSON 用 JsonCpp。body 若是 `rpcdata`，用它自己的 `FromJson` / `ToJson`。

`webserver` 仍用 `cpp-httplib` 听 `WebServer.port`（默认 18080）。`GET /query` 回解码后的 query，`POST /json` 回 JSON，`POST /rpcjson` 用 `JsonAll` 的 `FromJson` / `ToJson` 走一遍全量字段并把 `gap` 加一，`POST /bin` 把 body 原样写回。这个进程自己的 listen 是阻塞的，不进协程。

`webclient` 不直接调 `cpp-httplib`。它走基础库 `OI::Http::Client`：`Start` 用 `StartTask` 拉起 `Run`，四次请求都是 `co_await`。完成时 poll 线程 `Schedule` 协程句柄，不在回调里 `resume`。单线程模块链 `http_static`。退出码 0 表示四下都对上了。以后换 Go 或其他语言的 HTTP 服务时，仍用这个客户端，只改地址。

`host` 可以写 IPv4 或域名。域名用 libuv 的 `uv_getaddrinfo` 在它自己的线程池里解析，回调回到 poll 线程再连，不卡 poll。响应认 `Content-Length`、`chunked` 和读到断开三种结尾。超时定时器取消不了，所以请求状态带引用计数，定时器晚到时只释放，不改已经收完的结果。

`ipcserver` 也链了这个客户端，多线程模块链 `http_m_static`。`ipcserver.toml` 的 `[HttpProbe]` 填了 `host` 时，起服后 `StartTask` 发一次 GET，打印 `http probe ... status=... bytes=... title=...`。默认不填，压测时不要打开。
