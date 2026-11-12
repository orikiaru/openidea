# 模块和协议

三个 xml 是生成输入，都在 `modules/xmlconfig`。

| 文件 | 作用 |
| --- | --- |
| `module.xml` | 一个 `<module>` 是一个进程 |
| `module_component.xml` | 模块下的网络组件，`type` 为 `Client` 或 `Server`，上面挂 rpc |
| `proto.xml` | 协议正文，写法像 protobuf：`字段号 类型 名字;`。不是 XML |

字段号写在每一行前面，这就是线上的序号。缺了、重了，生成失败，不会按出现顺序补号。跳号直接写下一个数字，例如 `20 int32_t iGap json gap;`。

```
rpcdata AllTypesData {
    0 int8_t iInt8 json c;
    9 vector<int32_t> vNumbers json nums;
    13 map<int32_t, string> mTable json table;
    20 int32_t iGap json gap;
}
```

整数字段写 `int8_t`、`int16_t`、`int32_t`、`int64_t` 和 `uint8_t`、`uint16_t`、`uint32_t`、`uint64_t`。`string` 就是 `std::string`。`map` 的键可以是这些标量、`string`，也可以是名字后面加了 `key` 的 `rpcdata`。`key` 会生成 `operator<`。嵌进别的 `key`，或者再当 `map` 键时，里层也必须有比较器，生成时会检查。标了 `key` 的结构不生成 JSON；谁的字段里放了这种 `map`，谁也不生成 JSON。其余 `rpcdata` 仍生成 `ToJson` / `FromJson`。`rpc` 不生成 JSON。库是 JsonCpp。

基础库在 `framework.cmake`。它编三方库和 `cbaselib`，并生成 net 内部的工作协议，不加入进程模块。根目录 `CMakeLists.txt` include 它，再调用 `cbaselib/proto/proto_tool/main.py`。输入是上面三个文件，输出到 `modules/include`、`modules` 和 `deploy`，并在构建目录写出模块清单。修改任一输入会触发重新配置和生成；生成失败会终止配置。外部工程同样 include `openidea/framework.cmake`，换成自己的三个 xml 和输出目录，不再手写基础库列表。开发标准见 [开发标准](develop.md) 第 2 节。

模块需要访问数据库时，见 [数据库客户端](database.md)。协议数据库映射和 schema migration 本轮尚未实现。

## 什么会重写

`modules/include` 里的序列化头和 `rpc_ids.h` 每次 cmake 都重写。改这些文件无效。每条 `rpcdata` 写成 `oi_rpcdata_<名字>.h`，例如 `oi_rpcdata_alltypesdata.h`，都在 `OI::RpcData` 里。序列化基类仍在 `OI::Rpc`。`rpc` 不再另生成一层类，也不把函数体 `#include` 进类里。模块下不建 `protocol` 目录，生成时如果还在就删掉。

下面这些只在文件不存在时生成，已有文件会留下：

- `modules/<name>/src/main.cpp`
- `modules/<name>/CMakeLists.txt`
- 组件的 `inc/*.h` 和 `src/*.cpp`
- `deploy/config/<name>.toml`

生成的组件都是单例（`DECLARE_SINGLETON`），带 `OI::Coro::Task OnSecondUpdate(time_t)`，默认函数体是 `co_return;`。日历秒变化时 `StartTask` 启动，只启动、不等待。Server 继承 `ProtoSessionManager`；Client 继承 `ProtoMultiClientManager`，配置段多一个 `service`，留空只连 ip/port 这一台，填了服务名就连注册表里的每一台，见 [服务注册](registry.md) 的“每台各连一条”。

`module.xml` 是模块注册的唯一来源；CMake 使用生成的清单添加模块，不需要再维护一份手写列表。模块名必须匹配 ASCII 格式 `[A-Za-z0-9_][A-Za-z0-9_-]*`，忽略大小写后不能重复；生成目录及清单使用小写名称。

## 加一个回声协议

在 `proto.xml` 增加请求和回复两个 `rpcdata`，字段号自己写上。`rpc` 放在文件最后，一行一条，请求和回复共用一个协议号。调用号最高位是 1 表示请求，回包清掉这一位。

在 `module_component.xml` 里把这条 rpc 挂到对应的 Client 和 Server 组件上。

服务端在组件 `Init` 里 `HandleRpc`，客户端在连上之后 `co_await RpcCall`。写法见 [协程](coroutine.md)。

现成例子是 `AllEcho`：`ipcserver` 用 `OI_RPC_SYNC_HANDLER` 把 `stData` 抄进回复并写 `iCode = 1`，`ipcclient` 连上后用 `CallAny` 按条发出去并核对回复。`Hold` 用来看超时和回包同时到：服务端 `co_await OI::Coro::yield(stRequest.iDelayMilliseconds)` 再回，客户端按调用号认这一次。按服务名打一台或一批见 [服务注册](registry.md)。

## ipcclient 的测试开关

`deploy/config/ipcclient.toml` 的 `[Test]` 里 `echo`、`log`、`redis`、`hold` 各管一个测试，默认只开回声。一次只开一个。

`log = true` 时日志服必须已经在听，`ipcserver` 也要起：`LogMgr` 是在连上 `ipcserver` 之后才开始发的，连不上就一直不发，CPU 是 0。`SendLog` 失败后测试会立刻再排一次发送，字节数加了又减，到不了目标，进程也不会退出。

## Web

`webserver` 和 `webclient` 是两个独立进程，只走 HTTP，不开 HTTPS。JSON 用 JsonCpp。body 若是 `rpcdata`，用它自己的 `FromJson` / `ToJson`。

`webserver` 仍用 `cpp-httplib` 听 `WebServer.port`（默认 18080）。`GET /query` 回解码后的 query，`POST /json` 回 JSON，`POST /rpcjson` 用 `JsonAll` 的 `FromJson` / `ToJson` 走一遍全量字段并把 `gap` 加一，`POST /bin` 把 body 原样写回。这个进程自己的 listen 是阻塞的，不进协程。

`webclient` 不直接调 `cpp-httplib`。它走基础库 `OI::Http::Client`：`Start` 用 `StartTask` 拉起 `Run`，四次请求都是 `co_await`。完成时 poll 线程 `Schedule` 协程句柄，不在回调里 `resume`。单线程模块链 `http_static`。退出码 0 表示四下都对上了。以后换 Go 或其他语言的 HTTP 服务时，仍用这个客户端，只改地址。

`host` 可以写 IPv4、IPv6 字面地址或域名。IPv6 字面地址可以带方括号或不带方括号，请求的 `Host` 头统一使用方括号；端口不是 80 时附加端口。域名用 libuv 的 `uv_getaddrinfo` 在它自己的线程池里解析一次，回调回到 poll 线程，再交给 `SessionManager` 去连，不卡 poll。`Client` 最后一个参数 `bReuse` 默认是复用：同一个 `Client` 上的请求串行共用这一条连接，请求头是 `Connection: keep-alive`。传 `false` 时请求头改成 `Connection: close`，这一响结束后主动关掉，下一个请求重新连。对端写了 `Connection: close`、HTTP/1.0 没有 keep-alive、报文没收完对端就断开、这一响解析失败，或者当前响应后面还粘着没消费的字节，就把这条会话关掉，下一个请求重新连。响应认 `Content-Length`、`chunked` 和读到断开三种结尾。超时定时器取消不了，所以请求状态用共享指针留到定时器回来；已经收完的结果不会被晚到的超时改掉。

`ipcserver` 也链了这个客户端，多线程模块链 `http_m_static`。`ipcserver.toml` 的 `[HttpProbe]` 填了 `host` 时，起服后 `StartTask` 发一次 GET，打印 `http probe ... status=... bytes=... title=...`。默认不填，压测时不要打开。

## 字段命名和 JSON 兼容

协议字段使用类型前缀 + 大驼峰名称，完整规则见项目 AGENTS.md。生成器同时检查文本协议和 XML 协议；名称不合规范会直接报错，不自动猜测业务名字。

字段名与 JSON 键可以分开：`0 int32_t iCount json count;` 的 C++ 字段为 `iCount`，JSON 仍使用 `count`。省略 `json` 时 JSON 键就是字段名。XML 输入对应 `<member name="iCount" type="int32_t" seq="0" json="count"/>`。同一结构中 JSON 名称不能重复。

这次只修改 C++ 字段名：字段编号、类型、顺序和已有 JSON 键保持不变，配置键不改。生成后的头文件不要手工修；修改协议源后重新执行 cmake。

JSON 解码约定：整数只接受 JSON 整数 token，拒绝小数（包括 `1.0`）、越界和负数转无符号；整数 map 键必须完整匹配十进制整数，不接受空白、加号、尾随字符或溢出。生成的 `FromJson` 返回 false 时保留原对象，成功时更新提供的字段，缺失字段保持原值；容器成功后整体替换。保证覆盖普通验证失败，不把内存分配异常伪装为 false。

## 枚举和错误码

文本协议认 `rpcdata`、`rpc`、`enum`、`errordef` 四种。`enum` 和 `errordef` 只生成普通 C 枚举，不参与序列化，全部写进 `modules/include/rpc_enums.h`，名字空间 `OI::RpcData`，`rpc_ids.h` 已经包含它。

```text
enum UdpProbe {
    UDP_PROBE_PEER_ADDR = -1;
    UDP_PROBE_SESSIONS = -2;
}

errordef UdpError {
    UDP_ERR_REJECTED = 1001;
}
```

- 每项写成 `名字 = 值;`，值必须手写。名字是大写下划线，不带作用域，所有 `enum` 和 `errordef` 之间不能重名；同一个枚举里值不能重。
- `errordef` 是业务错误码，取值范围为 1..2147483647，所有 `errordef` 之间值不能重；普通 `enum` 的取值规则不变。框架错误码在 `oi_errno.h`，是负数，0 是成功。处理函数直接 `co_return ::OI::RpcData::UDP_ERR_REJECTED;`，客户端 `RpcCall` 原样拿到这个值。
- `rpcdata` 字段不能写枚举类型，用定宽整数（通常是 `int32_t`），比较和赋值时用枚举常量。
- 只有文本协议支持，XML 协议不支持。用例是 `modules/xmlconfig/proto.xml` 的 `UdpProbe`、`UdpError`，由 `udp` 回归覆盖。
