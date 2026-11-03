# 基础库

`cbaselib` 编成两套静态库。多线程是 `base_m_static`、`net_m_static`、`proto_m_static`、`timer_m_static`、`logger_m_static`、`redis_m_static`、`http_m_static`。单线程是不带 `_m_` 的同名库，编译时定义 `_SINGLE_THREAD_`。

`_SINGLE_THREAD_` 下 `Mutex` 和 `SpinLock` 的加锁是空操作。多线程的 `Mutex` 是 `std::recursive_mutex`。

## 目录

`base` 是进程里的公共件。`Bytes` 是二进制缓冲，`BytesStream` 做序列化。`Config` 读 toml。`ThreadPool` 跑普通任务和协程句柄。应用侧用 `AddTask(lambda)` 或协程，不要自己继承 `Task` 再 `new`/`delete`。`Task` 的分配留在线程池里。`DECLARE_SINGLETON(T)` 提供 `T::GetInstance()`，返回唯一实例的引用，不进入继承链。`FilePath` 检查路径在不在，并补齐中间目录。

`net` 是网络，类型在 `OI::Net`。`Poll` 包 libuv 的事件循环。`NetSocket`、`AcceptChannel`、`ConnectChannel`、`TcpChannel` 负责监听、连接和收发。`Session` 和 `SessionManager` 管一类连接，组件配置里的 ip、port、type 从这里读。`SendBytes` 只在查会话表时持管理器锁，拷贝数据在锁外、持会话锁做；会话带引用计数，通道销毁时先断开会话到通道的指针再释放引用。监听口在 `bind` 之前打开 `SO_REUSEADDR`。进程退出后旧连接还会在 `TIME_WAIT` 里待一会儿，但不再挡住下一次监听。客户端连出去不走 `bind`。按服务名发现地址在 [服务注册](registry.md)。表由 `registry` 进程管，存在 Redis，应用只连 registry。配置里写了 `<名字>.service` 时，`SessionManager::Init` 先向 registry 查，查不到再用同一段的 ip/port。监听方在 `StartServer` 成功后登记并定时报告在线。空间按 registry 的 `[Registry] project/env` 分开。

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

1. `OI::Base::StartFramework(配置路径)`：`Config` 读 `deploy/config/<进程名>.toml`，再依次 `ThreadPool::Init`、`BaseTimerManager::Init`、`Poll::Init`。返回非 0 时 `main` 直接把它返回，配置读不了是 -2，框架初始化失败是 -1
2. `OI::Log::Init`
3. 各组件 `Init`，服务端再 `StartServer`，客户端再 `StartClient`
4. `return OI::Base::RunFramework()`：进入主循环，退出后返回退出码

`logd` 不调 `OI::Log::Init`，不作为日志客户端连自己。它在 `LogManager::Init` 之后调 `OI::Log::InitLocal`，自己的日志直接进本地文件。

`[ThreadPool] max_task_count` 限制每个优先级队列里排队的 `Task` 数，满了 `AddTask` 返回 false 并销毁这个任务。不配不限。协程句柄不走这条队列，不受它限制。

## 发送缓冲限额与同步等待

各网络组件配置段可设置 `max_send_queue_bytes`，默认每条连接 67108864（64MiB），配置必须为正数。计数包括会话和通道内尚未交给操作系统的字节，缓冲移动不恢复额度，实际写出才恢复。不是整个进程 RSS 上限，也不包含内核缓冲；大于配置上限的单条消息同样被拒绝，需要按业务大小调整配置。

连接已关闭或容量不足时 `SendBytes` 返回 false，不追加半条消息。客户端 RPC 按发送失败结束等待；日志调用方可让出后重试，框架不自动重放写请求。服务端 RPC、Redis 和注册服务若无法排入回复，会关闭该连接，避免无声丢回复或 RESP 配对错位。会话关闭状态通过原子变量发布，缓冲仍由框架 Mutex 保护，单线程下该锁为空操作。

Redis 同步接口在单线程里也会推进普通任务与超时检查，不再只转网络和协程队列；在 libuv 回调中两种线程模式都明确抛 logic_error，避免重入网络循环。应用优先使用 co_await，单线程同步调用仍可能在等待时运行其他业务。
