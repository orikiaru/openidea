# 基础库

`cbaselib` 编成两套静态库。多线程是 `base_m_static`、`net_m_static`、`proto_m_static`、`timer_m_static`、`logger_m_static`、`redis_m_static`、`http_m_static`。单线程是不带 `_m_` 的同名库，编译时定义 `_SINGLE_THREAD_`。

`_SINGLE_THREAD_` 下 `Mutex` 和 `SpinLock` 的加锁是空操作。多线程的 `Mutex` 是 `std::recursive_mutex`。

## 目录

`base` 是进程里的公共件。`Bytes` 是二进制缓冲，`BytesStream` 做序列化。`Config` 读 toml。`ThreadPool` 跑普通任务和协程句柄。应用侧用 `AddTask(lambda)` 或协程，不要自己继承 `Task` 再 `new`/`delete`。`Task` 的分配留在线程池里。`DECLARE_SINGLETON(T)` 提供 `T::GetInstance()`，返回唯一实例的引用，不进入继承链。`FilePath` 检查路径在不在，并补齐中间目录。

`net` 是网络，类型在 `OI::Net`。`Poll` 包 libuv 的事件循环。`NetSocket`、`AcceptChannel`、`ConnectChannel`、`TcpChannel` 负责监听、连接和收发。`Session` 和 `SessionManager` 管一类连接，组件配置里的 ip、port、type 从这里读。`SendBytes` 只在查会话表时持管理器锁，拷贝数据在锁外、持会话锁做；会话带引用计数，通道销毁时先断开会话到通道的指针再释放引用。监听口在 `bind` 之前打开 `SO_REUSEADDR`。进程退出后旧连接还会在 `TIME_WAIT` 里待一会儿，但不再挡住下一次监听。客户端连出去不走 `bind`。按服务名发现地址在 [服务注册](registry.md)。表由 `registry` 进程管，存在 Redis，应用只连 registry。配置里写了 `<名字>.service` 时，`SessionManager::Init` 先向 registry 查，查不到再用同一段的 ip/port。监听方在 `StartServer` 成功后登记并定时报告在线。空间按 registry 的 `[Registry] project/env` 分开。

`proto` 是自有协议，类型在 `OI::Rpc`。`Protocol` 是协议基类，`Rpc` 是远程调用。`oi_rpc_coro.h` 是协程任务和 RPC 等待。收包按协议号交给 `DeliverRpc`，不再按协议号克隆存根。`parser` 做组包。`proto_tool` 是 cmake 时跑的 Python 生成器。

`timer` 是分层时间轮，类型在 `OI::Timer`，见 [定时器](timer.md)。

`logger` 是日志客户端，类型在 `OI::Log`。业务进程用它把日志发到 `logd`。`logd` 自己的服务端在 `modules/logd`。`SendLog` 在日志服还没连上时先攒着，待发超过 4MB 就返回 false，调用方自己决定要不要再试。

logd 的 `[Log] max_pending_bytes` 默认 64MiB，所有日志级别共享，包含排队和正在写入、尚未 fflush 的批次及时间前缀。满时保留完整输入帧并暂停该连接读取，写线程完成后通过 poll 恢复；连接关闭后仍排空已读取的完整帧。进程正常退出先排空等待会话，再 join 写线程。本地日志返回 false 表示当前不能接纳，仍已按原规则输出到 stdout；不能在 poll 中阻塞等待磁盘。此限额不含接收缓冲、内核缓冲及分配器开销，不是 RSS 上限。

`python3 tools/run_regressions.py --case log-backpressure` 使用现有 LogMgr 发送 64MiB、通过临时 LD_PRELOAD 只延迟 logd 写盘线程，核对每条落盘正文、实际字节、队列峰值不超限、发生暂停且最终 pending=0；普通日志用例继续验证 1GiB 吞吐。全套 `--case all` 包含慢盘用例，两种客户端/服务端线程模式均需运行。

`redis` 是连 `redisserver` 的客户端，类型在 `OI::Redis`，字节是 RESP，不进 proto。一条连接上的命令按发送顺序对回包。超时后这条命令仍留在队列里，等它自己的回包来认领，避免后面的应答串位。回包如果再也没来，而连接还在，后面的命令会一直顶在它后面。超时单位是毫秒，来自 `RedisClient.timeout_ms`，由 200 毫秒的周期定时器扫描。先到的那个算数。`RedisResult.iStatus` 区分 Ok、Timeout、Disconnected、InvalidArgument、SendFailed、ServerError、ProtocolError、Busy；`bTimeout` 仅在真正超时时为真。错误回执仍保留在共享 `pReply` 中，`if (result)` 只对 Ok 成立。

推荐 `auto result = co_await RedisGet("key")` 或 `co_await CallRedis({"GET", "key"})`；旧的输出引用接口仍保留。过期时间可用 `RedisExpire("key", OI::Timer::Seconds(60))`，列表范围支持整数下标。`RedisClient.max_pending` 默认 65536，包含已超时但尚未收到回执的配对项，满后返回 Busy，不自动重试或重放写操作。

RedisCall 是单次请求对象，同一个对象只能 await 一次；重复等待、并发等待或等待已移走的对象会在提交前抛出 std::logic_error，不会再次发送写操作。需要再次请求时重新调用 RedisGet / CallRedis 创建对象；要复用已取得的响应，保存 RedisResult。

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

HTTP 客户端忽略临时 1xx 响应并继续读最终响应（最多 16 个，101 升级不支持）；204/304 在头部完成后结束，不等待 EOF。长度字段必须是完整十进制数；冲突 Content-Length、同时出现 Transfer-Encoding 和 Content-Length、未支持的传输编码、损坏的 chunk 长度及数据分隔符均按协议失败处理。chunked 支持扩展和 trailer。

客户端建连和服务地址切换统一在 poll 线程执行。`StartClient()` 调用即发起非阻塞连接，返回可等待对象；忽略对象为异步启动，`co_await StartClient()` 等结果（成功 true，失败/地址切换/停止入口后 false）。没有同步布尔开关。已有连接时成功，建连中的并发调用共享同一代连接；旧代次迟到的成功或失败不能改变当前状态。等待不占线程，管理器须覆盖网络操作生命周期。

Redis 入队、发送、解析和断连处理都在 poll 线程，等待项与实际发送使用同一个 sid；收包必须携带 sid，旧会话的数据直接丢弃。新连接会先结束旧等待再重建解析器。注册发现的 FIFO 请求入队和发送共用框架锁，发送失败/异常在锁内回滚，旧会话的回复和断连回调不影响新连接。

二进制 bool 字段只接受字节 0 和 1，其他值抛 ParserException，保留输出值及读取位置；普通流和字段格式都做相同校验。HTTP 在完成、失败或超时时先释放请求和原始响应缓冲容量，再唤醒调用方；晚到的超时仅持有轻量状态，不保留大包。

停服时，ThreadPool 先发布 StopRequested，再在 poll 上运行初始化阶段注册的停止回调，最后通知工人退出并 join，之后才停止 poll。Redis 的停止回调以 Disconnected 结束等待并关闭连接；停止后同步/协程 Redis 新调用直接失败，不再排到已退出的网络循环。停止回调不能阻塞；业务自身的 TaskScope 仍须按协程文档在正常退出前 Join。

redisserver 的 `[Redis] max_pending_bytes`（默认 64MiB）和 `max_pending_commands`（默认 65536）按每条上游连接计费，字节数包含完整 RESP 请求；收到回执或上游断连时归还额度，不因客户端超时而提前归还。任一额度不足就保留未发送命令并暂停该下游读取；两项都降到各自限额一半后恢复，并先处理已有输入缓冲。恢复通过 `Poll::PostOnLoop` 排到后续事件循环，不再依赖时间轮；每轮按等待队列轮转，每个会话最多转发一条命令，未处理完的排队尾。额度不足的大请求保留队首，避免被小请求持续挤占；新到请求不能越过已有等待者。单条命令超过字节限额会关闭下游连接，解析长度时即拒绝，不能等待一个永远凑不齐的额度。这是请求积压上限，并非进程 RSS 上限：分配器容量、下游输入缓冲、内核缓冲及响应内存另计。

复测使用 `python3 tools/run_regressions.py --case redis-shutdown`、`redis-backpressure-bytes`、`redis-backpressure-count`；用例都在现有 RedisMgr，通过临时配置的 `[Regression] redis_shutdown / redis_pressure / redis_oversized` 启用。停服测试在同步等待时发送 SIGTERM；背压测试暂停并恢复本机 Redis，检查限额、内存增量及 64 条大响应逐条配对，还验证超大命令拒绝后的重连。`--case all` 包含这三项，需在两种线程模式分别运行。

`Poll::CallOnLoop` 在 poll 线程上可以立即执行；`PostOnLoop` 始终入队。每次只消费进入时已有的一批操作，回调中新投的操作留到下一次唤醒，防止递归执行及持续自投递饿死 IO。

新增 `--case redis-fairness`：RedisMgr 建立两条真实下游连接，在上游额度为 1、服务端时间轮 tick 为 1000ms 时发送 64+1 条命令；驱动通过 Redis MONITOR 核对后者最迟第三条执行，并检查全部回复和恢复耗时。HoldMgr 同时覆盖不 await 即启动、完成后再 await、首次/重复建连、32 个并发等待者、失败和延迟投递不内联。两项纳入 `--case all`，单、多线程分别复测。

共享 Redis 上游禁止 `SCRIPT DEBUG` 子命令（大小写不敏感，所有模式均拒绝；子命令内嵌 NUL 也拒绝，防止 C 字符串比较截断绕过），避免调试模式污染其他下游的 EVAL/PING；SCRIPT LOAD 和 EVALSHA 保留。普通 RedisMgr 回归强制使用一条上游，以两条下游验证调试命令被断开、另一条仍收到正常脚本结果，并验证重连后的 PING 与脚本加载/执行。

过滤同时覆盖 hiredis 自己识别的异步状态命令，包括 PMONITOR 和命令名内嵌 CRLF 的监控/订阅/退订变体；不能只依赖 Redis 返回未知命令错误，因为 hiredis 在收到回复前就已改变连接状态。这些命令按已有规则关闭发起的下游连接，共享上游及其他下游保持可用。
