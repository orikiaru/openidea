# 回复里的代码必须能跳转

回复里只要提到本仓库的文件、函数、类型或某段实现，必须附上可点击跳转的代码引用。不要只写路径或函数名。

引用格式只有这一种：

```12:15:cbaselib/proto/src/oi_rpc_call.cpp
// 对应行上的代码
```

要求：

- 第一行是 `起始行:结束行:相对仓库根目录的路径`，后面紧跟该范围里的代码。
- 行号按文件当前内容填写，范围盖住你正在说的那几行。
- 同一处逻辑引用一次即可。解释在引用外面，用中文写。
- 文件还不存在、或只是在讨论方案还没有落盘时，不要编行号。

# 提交就是提交并推送

用户说「提交」时，包含本地 `git commit` 和推送到当前分支的远程。推送用普通 `git push`，不要强推。远程拒绝或需要登录时，把验证信息交给用户。

# 回声和日志要分开测

`ipcclient` 里四个 Mgr 都要跑到：`EchoMgr`、`LogMgr`、`RedisMgr`、`HoldMgr`。开关在 `deploy/config/ipcclient.toml` 的 `[Test]`：`echo`、`log`、`redis`、`hold`。一次只开一个。单线程和多线程各跑一遍，缺一套不算测完。单线程和多线程指 `ipcclient` 和 `ipcserver`。`logd` 只能多线程。`redisserver` 按它自己的编译选项是单线程。

测的是 `logd`、`ipcserver`、`ipcclient` 这三个进程。不要另写一个日志客户端。日志从 `LogMgr` 发出，回声从 `EchoMgr` 发出。

产物是 Linux 程序，在 WSL 里编、在 WSL 里跑。构建目录是 `~/openidea-build-gcc13`，可执行文件在 `deploy/bin`。进程的当前目录必须是某层 `bin`，配置在同级的 `config`。`logd` 读 `../config/logd.toml`，另外两个读 `../config/<名字>.toml`。

跑之前把这三份 toml 拷到临时目录再改，不要改仓库里的那份。日志目录改到 `/tmp` 下面，不要写进仓库。

顺序是先 `logd`（端口 19995），再 `ipcserver`（等到输出 `ipc server listen`，端口 17981），最后 `ipcclient`。

回声看客户端退出码和这一行：`ok` 等于 `[TestEcho].total`，`mismatch=0`，`rpc_fail=0`。

日志看日志服落盘的文件字节，不要只看客户端打印的 `enqueue_rate`。那个数是塞进发送缓冲的速度。端到端用文件字节除以从启动客户端到文件不再增长的时间。文件总量不要超过 10GB，超过 2GB 就停。测完删掉临时日志目录，并停掉这三个进程。

Redis 也单独测。开关是 `[Test] redis`。先起本机 Redis（`maxmemory 1536mb`），再起 `redisserver`（等到 `redis server listen`，端口 16379），最后 `ipcclient`。客户端连的是 redisserver，不是 Redis。看退出码，以及 `redis integrity ok` 和 `redis stress` 那一行里 `bad=0`。测完停掉 `redisserver`。

`HoldMgr` 的开关是 `[Test] hold`。要同时起 `ipcserver` 和 `redisserver`。看退出码，以及 `hold done bad=0`。

# 名字空间

框架代码按功能放进 `OI`，不按目录名堆在一起。模块里的业务类先不放进去。

| 名字空间 | 放什么 |
| --- | --- |
| `OI::Base` | 字节缓冲、配置、单例 |
| `OI::File` | 路径 |
| `OI::Thread` | 线程池、任务、锁 |
| `OI::Net` | poll、套接字、通道、会话 |
| `OI::Timer` | 时间轮、时间辅助 |
| `OI::Log` | 日志客户端 |
| `OI::Redis` | Redis 客户端 |
| `OI::Http` | HTTP 客户端 |
| `OI::Rpc` | 序列化基类、协议和调用 |
| `OI::RpcData` | 生成出来的 rpcdata |

头文件名跟着功能走，框架头和对应的 cpp 都用 `oi_` 前缀，例如 `oi_rpc.h`、`oi_file.h`、`oi_thread.h`。三方库和 `port/win` 里的系统兼容头不用这个前缀。

# 应用层不要睡线程

应用层要空等，先打开 `docs/coroutine.md` 和 `modules/ipcclient/src/log_mgr.cpp` 的 `Pump`，确认写法是 `co_await coro::yield(毫秒)`，对上了再写。不要换成 `sleep`、`sleep_for`、`Sleep`、`usleep`。单线程只有一条线程，睡下去 poll 停转。

# 关键词对上文档

请求里出现下表的词时，先读对应文档，再按标准用例改。不要另写一套挂起、配对或绑定。

| 关键词 | 文档 | 标准用例 |
| --- | --- | --- |
| 协程、Task、StartTask、co_await、yield、让出、挂起、睡眠、sleep、sleep_for | `docs/coroutine.md` | `modules/ipcserver/src/ipcserver.cpp` 的 `HandleRpc`；`modules/ipcclient/src/echo_mgr.cpp` 的 `StartTask`；空等用 `modules/ipcclient/src/log_mgr.cpp` 的 `Pump` 里 `co_await coro::yield` |
| RPC、RpcCall、HandleRpc、回包、调用号 | `docs/coroutine.md` 的服务端和客户端 | 同上，推迟回包用同文件的 `Hold` |
| Redis、redisserver、RESP | `docs/cbaselib.md` 的 redis 段 | `modules/ipcclient/src/redis_mgr.cpp` 的 `Check` |
| 定时器、时间轮、AddTimer | `docs/timer.md` | `Hold` 里的 `co_await coro::yield` |
| 协议、proto.xml、rpcdata、模块 | `docs/module.md` | `modules/xmlconfig/proto.xml` 的 `AllEcho`、`Hold` |
| 端口、复用、bind、TIME_WAIT、监听 | `docs/cbaselib.md` 的 net 段 | `cbaselib/port/oi_net_os.cpp` 的 `NetOs_Bind` |
| Web、HTTP、JSON、urlencode、二进制 body | `docs/module.md` 的 Web 段 | `modules/webserver` 的四个路径；`modules/webclient` 的 `Run` |

