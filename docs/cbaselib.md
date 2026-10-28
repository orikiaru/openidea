# 基础库

`cbaselib` 编成两套静态库。多线程是 `base_m_static`、`net_m_static`、`proto_m_static`、`timer_m_static`、`logger_m_static`、`redis_m_static`。单线程是不带 `_m_` 的同名库，编译时定义 `_SINGLE_THREAD_`。

`_SINGLE_THREAD_` 下 `Mutex` 和 `SpinLock` 的加锁是空操作。多线程的 `Mutex` 是 `std::recursive_mutex`。

## 目录

`base` 是进程里的公共件。`Bytes` 是二进制缓冲，`BytesStream` 做序列化。`Config` 读 toml。`ThreadPool` 跑普通任务和协程句柄。应用侧用 `AddTask(lambda)` 或协程，不要自己继承 `Task` 再 `new`/`delete`。`Task` 的分配留在线程池里。`DECLARE_SINGLETON(T)` 提供 `T::GetInstance()`，返回唯一实例的引用，不进入继承链。`FilePath` 检查路径在不在，并补齐中间目录。

`net` 是网络。`Poll` 包 libuv 的事件循环。`NetSocket`、`AcceptChannel`、`ConnectChannel`、`TcpChannel` 负责监听、连接和收发。`Session` 和 `SessionManager` 管一类连接，组件配置里的 ip、port、type 从这里读。监听口在 `bind` 之前打开 `SO_REUSEADDR`。进程退出后旧连接还会在 `TIME_WAIT` 里待一会儿，但不再挡住下一次监听。客户端连出去不走 `bind`。

`proto` 是自有协议。`Protocol` 是协议基类，`Rpc` 是远程调用。`rpc_coro.h` 是协程任务和 RPC 等待。`ProtoSession`、`ProtoStubManager` 管会话和协议号注册。`parser` 做解码。`proto_tool` 是 cmake 时跑的 Python 生成器。

`timer` 是分层时间轮，见 [定时器](timer.md)。

`logger` 是日志客户端。业务进程用它把日志发到 `logd`。`logd` 自己的服务端在 `modules/logd`。`SendLog` 在日志服还没连上时先攒着，待发超过 4MB 就返回 false，调用方自己决定要不要再试。

`redis` 是连 `redisserver` 的客户端，字节是 RESP，不进 proto。一条连接上的命令按发送顺序对回包。超时后这条命令仍留在队列里，等它自己的回包来认领，避免后面的应答串位。回包如果再也没来，而连接还在，后面的命令会一直顶在它后面。超时单位是毫秒，来自 `RedisClient.timeout_ms`，由 200 毫秒的周期定时器扫描。先到的那个算数：`timeout` 为真是超时，`reply` 非空是回执。

`port` 是套接字的平台差异。`port/win` 给 MSVC 补头文件。

`3rd` 放进来一起编的第三方代码：`cpptoml`、`libuv`、`wepoll`、`hiredis`。HTTP 的 query、JSON 和二进制 body 用 header-only 的 `cpp-httplib` 和 `nlohmann`，不编进上面的静态库。

`include` 是给业务包含的汇总头：`header.h`、`base_header.h`、`net_header.h`。

## 进程里谁先起来

生成出来的 `main` 按这个顺序：

1. `Config` 读 `deploy/config/<进程名>.toml`
2. `ThreadPool::Init`
3. `BaseTimerManager::Init`
4. `Poll::Init`
5. `Log::Init`
6. 各组件 `Init`，服务端再 `StartServer`，客户端再 `StartClient`
7. `ThreadPool::StartWork` 进入主循环
