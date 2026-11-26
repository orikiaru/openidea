# 模块和协议

生成输入在 `modules/protoconfig`。协议和模块都是 `.proto` 文本，不是 XML。`db.proto`、`json.proto`、`rpc.proto` 写在根目录 `CMakeLists.txt` 的 `OI_PROTO_FILES` 里，按这个顺序读入。

| 文件 | 作用 |
| --- | --- |
| `module.proto` | 一个 `module` 是一个进程 |
| `db.proto` | 数据库行，`DbChild`、`DbSample` |
| `json.proto` | JSON 样例，`Inner`、`JsonPlain`、`JsonDefault`、`JsonAll`。`Inner` 也给 `rpc.proto` 用。`JsonPlain` 写了 `[json]` 但不改名。`JsonDefault` 的字段不写标记，靠结构上的 `json` 默认参加 |
| `rpc.proto` | 业务 RPC、枚举和错误码。每行是 `字段号 类型 名字;` |

```
module LogD {
}

module IpcClient {
}
```

生成器读完列表里的全部文件后，再检查类型引用、枚举和比较器。`rpcdata`、`enum`、`errordef`、`rpc` 的名字不能重复，协议号也不能重复。一份文件里的 `rpcdata` 可以被其他文件引用，不要求定义写在前面。`rpc` 的请求和回复也要能在这些文件里找到。

字段号写在每一行前面，这就是线上的序号。缺了、重了，生成失败，不会按出现顺序补号。跳号直接写下一个数字，例如 `20 [json gap] int32_t iGap;`。

```
rpcdata DbSample json db {
    0 [db id] [dbkey] int64_t iId;
    1 [db] [dbtype VARBINARY 64] string sName;
    2 [nojson] int32_t iSecret;
    3 [nodb] string sCache;
    8 vector<int32_t> vNumbers;
    10 DbChild stChild;
    13 [dbjson] vector<int32_t> vJsonNumbers;
    14 [db notes] [dbjson] [dbtype TEXT] map<string, int32_t> mJsonFlags;
}
```

整数字段写 `int8_t`、`int16_t`、`int32_t`、`int64_t` 和 `uint8_t`、`uint16_t`、`uint32_t`、`uint64_t`。`string` 就是 `std::string`。`map` 的键可以是这些标量、`string`，也可以是名字后面加了 `key` 的 `rpcdata`。`key` 会生成 `operator<`。嵌进别的 `key`，或者再当 `map` 键时，里层也必须有比较器，生成时会检查。`key` 不能再标 `json`。

`json` 和 `db` 写在结构名后面，才生成对应方法。没标的结构不生成。标了以后字段默认都参加，序列化名就是字段名。字段标记各自写成 `[]`，可以连着写：`[json]`、`[json 键名]`、`[nojson]`、`[db]`、`[db 列名]`、`[nodb]`、`[dbjson]`、`[dbkey]`、`[dbtype 类型 长度]`。`[json]`、`[db]` 后面的名字可省略，省略就用字段名。同一字段不能既改名又排除。至少要留下一个参加的字段。`rpc` 不生成 JSON，也不生成数据库方法。库是 JsonCpp。

JSON 会穿透。嵌套的 `rpcdata`、`vector`、`map` 按内层继续生成，里层 `rpcdata` 自己也要标 `json`，否则外层字段无法生成 JSON。`[nojson]` 的字段不检查能不能表达成 JSON，所以不适合 JSON 的 `map` 可以留在同一个结构里，只要标 `[nojson]`。要沿用外部键就写 `[json 键名]`，例如 `0 [json count] int32_t iCount;`。不改名就写成 `0 [json] int32_t iCount;`，键名仍是 `iCount`，和什么都不写一样。

`db` 不穿透，只映射头一层。整数、浮点、`bool`、`string`、`Bytes` 各是一列。`vector`、`map` 和嵌套 `rpcdata` 默认整段收成 PB blob：序号固定为 0，每个容器恰好一条字段，空容器也包含数量为 0 的 payload。写了 `[dbjson]` 才改用现有 JSON 文本入库，这时里面的 `rpcdata` 必须自己标 `json`，标量不能写 `[dbjson]`。没写 `[dbjson]` 就不做这套 JSON。blob 里的结构不用标 `db`。列名默认是字段名，`[db 列名]` 才换，只写 `[db]` 列名仍是字段名。结构没标 `db`，就不能写 `[db]`、`[nodb]`、`[dbjson]`。字段写了 `[nodb]` 就不再是数据库字段，不能再写 `[db]`、`[dbjson]`、`[dbkey]`、`[dbtype]`。

`[dbkey]` 标主键字段，可以标多个组成联合主键，`BuildCreateTable` 和按主键读改删的语句靠它，见 [数据库客户端](database.md) 的单表语句。主键只能是整数、`bool`、`string` 或 `Bytes`，`string`、`Bytes` 还必须写定长的 `[dbtype]`；浮点、`vector`、`map`、`rpcdata` 和 `[dbjson]` 不能做主键。`[dbtype]` 换建表时的列类型，类型名不分大小写，生成时统一成大写：`VARCHAR`、`VARBINARY` 后面跟 1 到 65535 的长度，`CHAR`、`BINARY` 跟 1 到 255 的长度，`TEXT`、`MEDIUMTEXT`、`LONGTEXT`、`BLOB`、`MEDIUMBLOB`、`LONGBLOB` 不带长度，例如 `[dbtype VARBINARY 64]`。`string` 可以用其中任何一种；`Bytes` 和 PB blob 只能用二进制类型；`[dbjson]` 只能用文本类型；数值字段不能写。读行时按列名匹配，缺列保持原值，NULL、类型不对、重复列和列数对不上都失败并保持原对象。schema migration 仍未实现。模块访问数据库见 [数据库客户端](database.md)。

基础库在 `framework.cmake`。它编三方库和 `cbaselib`，并用 `--data-only` 生成 `cbaselib/proto/protocol/framework.proto`，不加入进程模块。协议号从 -1000 起。`logd` 和 `registry` 也在框架里，不写进 `module.proto`。根目录 `CMakeLists.txt` include 它，再调用 `cbaselib/proto/proto_tool/main.py`。`--` 前面是 `module.proto`、头文件目录、模块目录、部署目录和模块清单，后面是 `OI_PROTO_FILES`。输出到 `modules/include`、`modules` 和 `deploy`。修改任一输入会触发重新配置和生成；生成失败会终止配置。外部工程同样 include `openidea/framework.cmake`，换成自己的协议文件列表、模块文本、输出目录，不再手写基础库列表。开发标准见 [开发标准](develop.md) 第 2 节。

## 什么会重写

`modules/include` 里的序列化头和 `rpc_ids.h` 每次 cmake 都重写。改这些文件无效。每条 `rpcdata` 写成 `oi_rpcdata_<名字>.h`，例如 `oi_rpcdata_alltypesdata.h`，都在 `OI::RpcData` 里。序列化基类仍在 `OI::Rpc`。`rpc` 不再另生成一层类，也不把函数体 `#include` 进类里。模块下不建 `protocol` 目录，生成时如果还在就删掉。

下面这些只在文件不存在时生成，已有文件会留下：

- `modules/<name>/src/main.cpp`
- `modules/<name>/CMakeLists.txt`
- 组件的 `inc/*.h` 和 `src/*.cpp`
- `deploy/config/<name>.toml`

生成的 main 把组件初始化包在 try/catch 里；启动期任何失败（错误码或异常）先 `StopFramework()` 停掉 poll 线程再返回，避免退出时静态析构和 poll 线程并发。

生成的组件都是单例（`DECLARE_SINGLETON`），带 `OI::Coro::Task OnSecondUpdate(time_t)`，默认函数体是 `co_return;`。日历秒变化时 `StartTask` 启动，只启动、不等待。Server 继承 `ProtoSessionManager`；Client 继承 `ProtoMultiClientManager`，配置段多一个 `service`。留空时若写了 `instances = ["实例号 ip 端口 [分区]", ...]` 就按这份名单连，配了它就不要求 ip/port；没写 instances 只连 ip/port 这一台，实例号 0。填了服务名就连注册表里的每一台，见 [服务注册](registry.md) 的“每台各连一条”。

`module.proto` 是模块注册的唯一来源；CMake 使用生成的清单添加模块，不需要再维护一份手写列表。模块名必须匹配 ASCII 格式 `[A-Za-z0-9_][A-Za-z0-9_-]*`，忽略大小写后不能重复；生成目录及清单使用小写名称。

协议解码默认生成宏字段表；配置 CMake 时用 `-DOI_PB_DISPATCH=switch` 可改成 switch 分发，重新配置会重新生成协议头。内联热路径后的基准中，128 字段包宏分发约 1597 ns、switch 约 499 ns；大规模协议下 switch 编译更慢。测量和取舍见 [字段分发](pb-dispatch.md)。

RPC 帧在 IO 线程按读取上限原地解码；嵌套结构和容器复用同一流的上限，不逐层复制包体。请求解好后再调度处理协程，回包解好后再唤醒等待方；线上格式不变。每个独立帧有逻辑解码预算，取 max(64 MiB, 4 × 会话帧上限)：默认 64 MiB 帧上限对应 256 MiB，对外入口收紧到 1 MiB 帧时仍为 64 MiB；vector 按元素对象大小计费，map 按键值对象加四个指针估算节点，string 和 Bytes 的内容另计；这不是实际堆内存的精确上限。嵌套结构共享预算，读取事务回滚时恢复预算，下一独立帧重新计数。

## 加一个回声协议

在 `rpc.proto` 增加请求和回复两个 `rpcdata`，字段号自己写上。`rpc` 放在文件最后，一行一条，请求和回复共用一个协议号。调用号最高位是 1 表示请求，回包清掉这一位。数据库行写进 `db.proto`，JSON 样例写进 `json.proto`。

服务端在组件 `Init` 里 `RegisterRpcHandler`。平时客户端 `co_await RouteManager::Call`，见 [服务注册](registry.md) 和 [协程](coroutine.md)。

现成的服务端是 `AllEcho`：`ipcserver` 的同步处理函数 `HandleAllEcho` 把 `stData` 抄进回复并写 `iCode = 1`。`ipcclient` 的回声用 `IpcClient::Call` 打固定连接，那是回归里的直连，业务调用不要照抄。`Hold` 用来看超时和回包同时到：服务端 `co_await OI::Coro::yield(stRequest.iDelayMilliseconds)` 再回，客户端按调用号认这一次。

## ipcclient 的测试开关

`deploy/config/ipcclient.toml` 的 `[Test]` 里 `echo`、`log`、`redis`、`hold` 各管一个测试，默认只开回声。一次只开一个。`coro` 是协程压力，默认关掉，不进 `--case all`。

`log = true` 时日志服必须已经在听，`ipcserver` 也要起：`TestLog` 是在连上 `ipcserver` 之后才开始发的，连不上就一直不发，CPU 是 0。`SendLog` 失败后测试会立刻再排一次发送，字节数加了又减，到不了目标，进程也不会退出。

## Web

`webserver` 和 `webclient` 是两个独立进程。服务端提供 HTTP，客户端支持 HTTP 和 HTTPS。JSON 用 JsonCpp。body 若是 `rpcdata`，用它自己的 `FromJson` / `ToJson`。

`webserver` 使用框架 HTTP 服务端，监听 `WebServer.ip`（默认 127.0.0.1）和整数 `WebServer.port`（默认 18080）。五个路由分别是：`GET /health` 返回正文 `ok`；`GET /query` 回解码后的 query，缺少 `text` 返回 400；`POST /json` 给 JSON 对象加 `ok`，`POST /rpcjson` 用 `JsonAll` 的 `FromJson` / `ToJson` 走一遍全量字段并把 `gap` 加一，JSON 无效时返回 400；`POST /bin` 把 body 原样写回。请求由框架 poll 接收并启动处理协程，同一连接按请求顺序返回响应；请求头和 body 上限分别为 64 KiB 和 8 MiB，支持 Content-Length 和 chunked 请求体。可选 `[WebRpc]` 配置字符串 `port`、`ip`、`type` 和整数 `delay_ms` 后，`/query` 先等待直连 Hold RPC；可选 `[RedisClient]` 配置字符串 `port`、`ip`、`type` 及 `timeout_ms` 后，再等待 Redis PING。上游失败返回 502。它是示例 HTTP 服务，不提供 HTTPS 或生产环境的认证和访问控制。`c`、`gap` 这些是别名。`iPlain` 这些写了 `[json]` 但不改名。`iDefault`、`sDefault`、`vDefault`、`mDefault`、`stDefault` 连 `[json]` 都不写，结构标了 `json` 就默认参加，键名仍是字段名。`stDefault` 里的 `iNumber`、`sText` 同样不写标记。`iSkip` 和 `stDefault.iSkip` 标了 `[nojson]`，不进 JSON，读入时即使带了这个键也不改原值。客户端同时核对这几类。

`webclient` 的无正文及中间响应专项位于独立的 `test_http_bodyless.cpp`，由同一个客户端入口按测试配置分派；配套服务端夹具验证持久连接复用和请求顺序。

`webclient` 走基础库 `OI::Http::Client`：`Start` 用 `StartTask` 拉起 `Run`，九次请求都是 `co_await`：先请求无参数的 `/query` 并检查 400，再用同一客户端请求 `/health` 并检查 200 和正文；四次 GET 覆盖普通路径、已有查询、尾问号和尾 `&`，另外三次 POST 覆盖 JSON、rpcdata 和二进制。GET 追加参数时保留路径已有查询，按需使用 `?` 或 `&` 分隔；参数名和值会做 URL 编码。完成时 poll 线程 `Schedule` 协程句柄，不在回调里 `resume`。默认单线程模块链 `http_static`；将 `OI_WEBCLIENT_SINGLE_THREAD` 设为 OFF 可编译多线程客户端，链 `http_m_static` 并读取线程池配置。退出码 0 表示所有检查都通过。从 `deploy/bin` 先运行 `webserver`，等监听成功后运行 `webclient`，两者读取同级 `config` 下各自的 TOML；`WebClient` 可选 `timeout_ms`、`reuse`、`https` 和 `ca_file`，默认分别为 5000、true、false 和空字符串。启用 `https` 后客户端校验证书链和主机名；`ca_file` 可指定可信 CA 文件，空字符串在 Linux 使用 OpenSSL 默认信任路径，在 Windows 使用系统证书存储。以后换 Go 或其他语言的 HTTP 服务时，仍用这个客户端，只改地址。

`host` 可以写 IPv4、IPv6 字面地址或域名。IPv6 字面地址可以带方括号或不带方括号，请求的 `Host` 头统一使用方括号；HTTP 端口不是 80 或 HTTPS 端口不是 443 时附加端口。域名用 libuv 的 `uv_getaddrinfo` 在它自己的线程池里解析一次，回调回到 poll 线程，再交给 `SessionManager` 去连，不卡 poll。`Client` 最后一个参数 `bReuse` 默认是复用：请求从进程共享池复用空闲连接，并发请求使用独立会话，请求头是 `Connection: keep-alive`。传 `false` 时请求头改成 `Connection: close`，这一响结束后主动关掉，下一个请求重新连。对端写了 `Connection: close`、HTTP/1.0 没有 keep-alive、报文没收完对端就断开、这一响解析失败，或者当前响应后面还粘着没消费的字节，就把这条会话关掉，下一个请求重新连。响应认 `Content-Length`、`chunked` 和读到断开三种结尾。 服务端收到客户端写半关闭时，已完整收到的请求仍可完成并发送响应；请求未完整时关闭连接。客户端收到 FIN 时先将连接标记为不可复用，再处理已收到的响应；若 FIN 晚于连接再次借出，受影响请求按连接错误结束，不自动重发。超时定时器通过弱引用查找请求状态，不会延长已完成请求的生命周期；晚到的超时不会改写已完成结果。

无需保留客户端对象的场景可以使用 `Client::GetUrl`、`Client::PostUrl` 或 `Client::SendUrl` 直接请求完整 URL，并通过 `ClientOptions` 设置超时和 CA 文件。URL 请求共享进程级 HTTP 会话管理器，并发请求使用独立会话；HTTP、HTTPS 和不同 CA 配置不混用连接；限额按 scheme、规范化 host 和 port 统计，同一目标的不同 CA 配置共享该限额。单次请求需要新连接时设置 `Request::bFreshConnection`。`[HttpClientPool] max_connections_per_server` 默认 4，达到上限时请求排队且超时包含排队时间；`max_waiting_per_server` 默认 1024，`max_waiting_bytes_per_server` 默认 67108864，以编码后的请求头和正文计数，任一配置为 0 禁止排队，超过限额返回 `OI_ERR_BUSY`。`idle_timeout_ms` 默认 60000，空闲连接按秒级周期回收。新连接计入上限，达到上限时须回收空闲连接才能创建。

HTTP 域名解析得到多个地址时，连接失败且尚未发送请求可尝试下一个地址；请求发送后不自动重放。服务端收到 `Expect: 100-continue` 时先发送临时响应，再接收正文；未知 expectation 返回 417。

`ipcserver` 也链了这个客户端，多线程模块链 `http_m_static`。`ipcserver.toml` 的 `[HttpProbe]` 填了 `host` 时，起服后 `StartTask` 发一次 GET，打印 `http probe ... status=... bytes=... title=...`。默认不填，压测时不要打开。

## 字段命名和 JSON 兼容

协议字段使用类型前缀 + 大驼峰名称，完整规则见项目 AGENTS.md。生成器同时检查文本协议和 XML 协议；名称不合规范会直接报错，不自动猜测业务名字。

字段名与 JSON 键可以分开：结构标了 `json` 之后，`0 [json count] int32_t iCount;` 的 C++ 字段为 `iCount`，JSON 仍使用 `count`。不写 `[json 键名]` 时 JSON 键就是字段名。XML 输入对应 `<rpcdata name="Example" json="true">` 和 `<member name="iCount" type="int32_t" seq="0" json="count"/>`。同一结构中参加 JSON 的名称不能重复。数据库列名同样不能重复。

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
- 只有文本协议支持，XML 协议不支持。用例是 `modules/protoconfig/rpc.proto` 的 `UdpProbe`、`UdpError`，由 `udp` 回归覆盖。
