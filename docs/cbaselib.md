# 基础库

`cbaselib` 编成两套静态库。多线程是 `base_m_static`、`net_m_static`、`proto_m_static`、`timer_m_static`、`logger_m_static`、`redis_m_static`、`http_m_static`。单线程是不带 `_m_` 的同名库，编译时定义 `_SINGLE_THREAD_`。

`_SINGLE_THREAD_` 下 `Mutex` 和 `SpinLock` 的加锁是空操作。多线程的 `Mutex` 是 `std::recursive_mutex`。

## 目录

`base` 是进程里的公共件。`Bytes` 是二进制缓冲，`BytesStream` 做序列化。`Config` 读 toml。`ThreadPool` 跑普通任务和协程句柄。应用侧用 `AddTask(lambda)` 或协程，不要自己继承 `Task` 再 `new`/`delete`。`Task` 的分配留在线程池里。`DECLARE_SINGLETON(T)` 提供 `T::GetInstance()`，返回唯一实例的引用，不进入继承链。`FilePath` 检查路径在不在，并补齐中间目录。

`net` 是网络，类型在 `OI::Net`。`Poll` 包 libuv 的事件循环。`NetSocket`、`AcceptChannel`、`ConnectChannel`、`TcpChannel` 负责监听、连接和收发。`Session` 和 `SessionManager` 管一类连接，组件配置里的 ip、port、type 从这里读。监听口在 `bind` 之前打开 `SO_REUSEADDR`。进程退出后旧连接还会在 `TIME_WAIT` 里待一会儿，但不再挡住下一次监听。客户端连出去不走 `bind`。按服务名发现地址、收掉重复的会话管理器，记在 [服务注册](registry.md)，后期再做。

`proto` 是自有协议，类型在 `OI::Rpc`。`Protocol` 是协议基类，`Rpc` 是远程调用。`oi_rpc_coro.h` 是协程任务和 RPC 等待。收包按协议号交给 `DeliverRpc`，不再按协议号克隆存根。`parser` 做组包。`proto_tool` 是 cmake 时跑的 Python 生成器。

`timer` 是分层时间轮，类型在 `OI::Timer`，见 [定时器](timer.md)。

`logger` 是日志客户端，类型在 `OI::Log`。业务进程用它把日志发到 `logd`。`logd` 自己的服务端在 `modules/logd`。`SendLog` 在日志服还没连上时先攒着，待发超过 4MB 就返回 false，调用方自己决定要不要再试。

`redis` 是连 `redisserver` 的客户端，类型在 `OI::Redis`，字节是 RESP，不进 proto。一条连接上的命令按发送顺序对回包。超时后这条命令仍留在队列里，等它自己的回包来认领，避免后面的应答串位。回包如果再也没来，而连接还在，后面的命令会一直顶在它后面。超时单位是毫秒，来自 `RedisClient.timeout_ms`，由 200 毫秒的周期定时器扫描。先到的那个算数。`RedisResult.status` 区分 Ok、Timeout、Disconnected、InvalidArgument、SendFailed、ServerError、ProtocolError、Busy；`timeout` 仅在真正超时时为真。错误回执仍保留在共享 `reply` 中，`if (result)` 只对 Ok 成立。

推荐 `auto result = co_await RedisGet("key")` 或 `co_await CallRedis({"GET", "key"})`；旧的输出引用接口仍保留。过期时间可用 `RedisExpire("key", OI::Timer::Seconds(60))`，列表范围支持整数下标。`RedisClient.max_pending` 默认 65536，包含已超时但尚未收到回执的配对项，满后返回 Busy，不自动重试或重放写操作。

共享上游连接不支持改变连接状态的命令（切库、认证、事务、订阅、CLIENT 等）。代理收到这类命令会记录错误并关闭该下游连接，避免本地错误回包与在途回包错位。阻塞命令仍会阻塞共用该上游的其他会话，生产环境应单独部署这类调用。多线程版同步 Redis 调用禁止在 poll 线程执行，会抛出 `std::logic_error`；该线程应使用协程接口。

`http` 是 HTTP/1.1 客户端，在 `OI::Http`。套接字是一条 `OI::Net::Channel`，可读走 `DataIn`，可写走 `DataOut`。`co_await` 之后由 poll 线程收完，再 `Schedule` 把协程句柄交回去，不在回调里 `resume`，也不睡线程。查询参数按百分号编码，和 `cpp-httplib` 的 `encode_query_param` 同一套字符。body 按 `Content-Length` 原样发送。单线程模块链 `http_static`，多线程模块链 `http_m_static`。服务端不在这个库里。

`port` 是套接字的平台差异。`port/win` 给 MSVC 补头文件。

`3rd` 放进来一起编的第三方代码：`cpptoml`、`libuv`、`hiredis`、`jsoncpp`。`webserver` 仍用 header-only 的 `cpp-httplib` 听端口。JSON 只用 JsonCpp。Windows 的事件循环用 libuv 自己的实现。

`include` 是给业务包含的汇总头：`oi_header.h`、`oi_base.h`、`oi_net.h`。

## 进程里谁先起来

时序在 [时序图](sequence-diagram/README.md)。生成出来的 `main` 按这个顺序：

1. `Config` 读 `deploy/config/<进程名>.toml`
2. `ThreadPool::Init`
3. `OI::Timer::BaseTimerManager::Init`
4. `OI::Net::Poll::Init`
5. `OI::Log::Init`
6. 各组件 `Init`，服务端再 `StartServer`，客户端再 `StartClient`
7. `ThreadPool::StartWork` 进入主循环
