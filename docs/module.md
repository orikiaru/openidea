# 模块和协议

三个 xml 是生成输入，都在 `modules/xmlconfig`。

| 文件 | 作用 |
| --- | --- |
| `module.xml` | 一个 `<module>` 是一个进程 |
| `module_component.xml` | 模块下的网络组件，`type` 为 `Client` 或 `Server`，上面挂 rpc |
| `proto.xml` | `rpcdata` 和 `rpc`。`rpc` 写 `request`、`response`、`id`、`res_id`、`timeout` |

字段按声明顺序从 0 编号。要跳号时写 `seq`。

cmake 调用 `cbaselib/proto/proto_tool/main.py`，参数是这三个 xml、`modules/include`、`modules`、`deploy`。

## 什么会重写

`modules/include` 里的序列化头和 `rpc_ids.h` 每次 cmake 都重写。改这些文件无效。这些 rpcdata 在 `OI::Rpc` 里。`rpc` 不再另生成一层类，也不把函数体 `#include` 进类里。

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

在 `proto.xml` 增加请求和回复两个 `rpcdata`，再加一条 `rpc`，请求和回复各一个协议号。

在 `module_component.xml` 里把这条 rpc 挂到对应的 Client 和 Server 组件上。

服务端在组件 `Init` 里 `HandleRpc`，客户端在连上之后 `co_await RpcCall`。写法见 [协程](coroutine.md)。

现成例子是 `AllEcho`：`ipcserver` 把请求原样放进回复，`ipcclient` 连上后按条发出去并核对回复。`Hold` 用来看超时和回包同时到：服务端 `co_await coro::yield(delay_ms)` 再回，客户端按调用号认这一次。

## ipcclient 的测试开关

`deploy/config/ipcclient.toml` 的 `[Test]` 里 `echo`、`log`、`redis`、`hold` 各管一个测试，默认只开回声。一次只开一个。

`log = true` 时日志服必须已经在听。`SendLog` 失败后测试会立刻再排一次发送，字节数加了又减，到不了目标，进程也不会退出。

## Web

`webserver` 和 `webclient` 是两个独立进程，只走 HTTP，不进 proto，也不开 HTTPS。query、JSON 和二进制 body 交给 `cbaselib/3rd/cpp-httplib` 与 `cbaselib/3rd/nlohmann`。

`webserver` 听 `WebServer.port`（默认 18080）。`GET /query` 回解码后的 query，`POST /json` 回 JSON，`POST /bin` 把 body 原样写回。`webclient` 连上后打这三下，退出码 0 表示都对上了。
