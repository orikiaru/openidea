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

# 锁用项目封装

- 自有代码（基础库、模块、测试）加锁和等待一律用 `cbaselib/base/inc/oi_lock.h`：互斥用 `OI::Thread::Mutex` + `MutexGuard`，自旋用 `SpinLock` + `SpinLockGuard`，等待通知用 `MutexCond`（`Wait`、`WaitUntil`、`Signal`、`Broadcast`）。
- 禁止直接写 `std::mutex`、`std::recursive_mutex`、`std::lock_guard`、`std::unique_lock`、`std::scoped_lock`、`std::condition_variable(_any)`。`oi_lock.h` 自身的实现和第三方源码除外。
- `MutexCond` 不带条件参数，等待一律写成 `while (条件不满足) { Wait(...); }`，带超时的用 `WaitUntil` 的返回值退出循环，防虚假唤醒。
- 单线程编译下这些锁和条件变量是空实现，不会真的阻塞；单线程路径不能靠它们等待，要转 poll。
- 封装里没有的能力（例如新的等待方式），先在 `oi_lock.h` 里补，单线程和多线程两套一起补，不在业务代码里绕过去用标准库。
- 改代码时看到存量的标准库锁，一并换掉。

# 杂活交给 subagent

主对话只做核心代码的分析、修改和对 subagent 结果的核对。下面这些费 token 但不难的活，一律派 subagent 做，不在主对话里自己动手：

- 文档同步：改完代码后更新 `docs/`、时序图、头文件注释以外的说明文字；
- 编写或补充测试用例、回归脚本里的检查项；
- 编译、回归矩阵、日志翻查；
- 批量的机械改名、格式类改动。

模型和编译回归一样：Cursor 里用 `grok-4.7-high`，Codex 里用 Terra。派活时把改动点、要遵守的规则和验收标准写清楚。subagent 回来后，主对话要看它的 diff，确认没改出代码范围以外的东西。这条也是长期偏好，不用每轮再问。

# Review 与开发优先级

- 逻辑稳定是第一优先级：结果正确、生命周期安全、并发状态一致、异常收尾完整；其次是应用层调用简单直接，以及故障有据可查。
- 已确认的崩溃、UB、数据错误、回包错配、丢失唤醒等正确性问题应及时修复，不等线上出事故。Review 必须说明触发条件、代码依据、实际后果和最小修复方向，推测风险单独标明。
- 可以接受少量性能损耗；没有实际瓶颈或测量证据，不因额外拷贝、锁、调度优先级、队列或连接分配方式而默认重构。性能优化等真实问题出现并定位后再做。
- 近期优化集中在模板、编译效率和运行效率的折中，不单追一边。能不用多层嵌套模板就不用，协议生成的编译量随协议条数上涨。运行更快、编译更贵的方案要提供给使用方，由他们自己选，不替他们定死。字段分发的测量在 `docs/pb-dispatch.md`：宏字段表编译快，`switch` 解码大约快 1 微秒、编译大约慢到 6 倍，`map` 两边都没有更好。本仓库生成的协议解码目前用宏。
- 异常应能通过已有日志和错误码定位：按需保留操作、服务/会话/调用标识、失败阶段及原始原因。返回统一错误码时不能让关键诊断信息无处可查；不默认增加大型监控体系，也不为每次成功调用刷日志。
- 诊断日志不直接输出完整请求、凭据或业务敏感值；外部错误文本按需脱敏、限长，高频重复故障可限频，但不能完全丢失首次原因和发生情况。
- 优先维护现有稳定接口和架构。只有当前实现妨碍正确性或应用层安全使用时才考虑重构；没有实质问题可以明确报告未发现，不为凑优化项制造改动。
- `RunBlocking` 的同步回调及其自有调用链禁止调用 `co_await`、`yield`、`StartTask`、`TaskScope` 和 RPC、Redis、HTTP 异步接口。返回 `int32_t` 的签名不能证明内部没有异步调用；Review 必须沿自有调用链核对。框架完成通知和主协程调度不属于用户同步回调。
- `RunBlocking` 的 lambda 只能按值捕获临时变量，初始化捕获把临时对象移进闭包，对象活到任务结束。禁止 `[&]`、引用捕获、裸 `this`、成员，以及指向调用方栈上局部对象的指针或引用。要带进后台的具名局部变量先做成临时对象再捕获，或作为右值参数交给 `RunBlocking`。输出仍用左值引用参数，调用方保证活到任务完成。Review 必须看捕获列表，不能只看函数签名。
- `co_await` 和 `yield` 挂起期间别的协程会运行，多线程恢复还可能换工人。挂起前取得的迭代器、容器元素引用或指针、`string_view`、`span`，恢复后继续用，必须保证这段时间容器没有插入、删除、重分配、重哈希、移动或销毁。本协程帧独占的局部容器、按值放进帧的容器，以及范围 for 把临时对象延长到整个循环的情形，挂起期间没有任何代码改它，迭代器才算稳定。成员、共享容器，以及别的协程或线程拿得到的对象，不算稳定。不稳定时先拷出 key、下标或值，恢复后再查找。`vector<bool>` 的代理引用和 JSON 迭代器同样不能跨挂起。范围 for 的隐藏迭代器按同一条核对。`RpcContext` 由框架覆盖整段处理协程，处理函数里可以跨挂起使用。
- `MutexGuard`、`SpinLockGuard`、`NativeMutexGuard` 必须在 `co_await` 或 `yield` 之前结束。多线程 `Mutex` 由加锁的那条线程持有，挂起后可能在别的工人上恢复，锁却一直占到守卫析构。单线程下 `Mutex` 和 `SpinLock` 是空实现，挂起期间别的协程仍会改数据。`NativeMutex` 在单线程构建里也是真锁。先放开守卫，恢复后重新加锁，再查条件。
- 单线程模式的任务创建和启动必须在 poll 线程；跨线程调用会输出警告并抛出 `std::logic_error`，拒绝创建或启动。多线程模式不限制启动线程。故障注入负例允许刻意跨线程验证拒绝，不作为业务调用范例。
- 阻塞回归须使用真实同步阻塞读取或同步 SDK，前台用 timer 和 RPC 验证活性后解阻塞，不能只靠返回值、sleep 或轮询冒充真实阻塞。引用入参和输出须活到任务真正完成，不能提前超时释放。

# 回声和日志要分开测

第三方测试工具（Redis、编译器、SDK 等）安装在持久目录并复用，严禁把停止进程理解为删除可执行文件、源码安装目录或卸载工具。清理只移除本次测试的临时配置、日志和数据；不要把工具安装在测试临时目录或 `/tmp` 中。缺依赖先核对已有安装及 `--redis-binary` 路径，不要直接重复下载安装。

每次测试前后都把本项目相关进程全部停掉重来，关不掉的直接强杀：按本仓库/本路副本的完整路径或 `.oi-test-owner.json` 归属标记匹配，Windows 记录的 PID 还必须核对进程创建时间；端口、通用脚本名和临时目录前缀不能单独作为归属依据。系统自带同名的 `Registry` 进程不能碰；WSL 只清本项目测试进程，保留用户 6379 上的 Redis。上一轮的残留进程不能带进下一轮。清理用 `python tools/kill_test_procs.py`（并行时加 `--lane`），目录和 WSL 用户参数与矩阵保持一致；清理失败或有残留必须报错，不要另写清理命令。

编译和回归一律交给 subagent 跑，主对话不直接跑矩阵。Cursor 里 subagent 固定用 `grok-4.7-high`（Grok 4.7）。Codex 里固定用 Terra，不用 Astra。这是已确认的长期偏好，不用每轮再问；只有用户要求更换时再确认。默认四路并行，按 `docs/testing.md` 第 7 节的范本同时派四个 subagent；subagent 回报每个平台、线程模式和用例的通过、失败、环境阻塞，以及失败日志的关键行。

完整操作规程统一维护在 `docs/testing.md`，跨平台构建/测试/恢复使用 `tools/test_matrix.py`，原生用例入口为 `tools/run_regressions.py`。测试脚本必须留在工程内；报告中区分实际通过、失败和环境阻塞。

有行为的改动，Linux 和 Windows 都要跑。默认还要单线程、多线程各跑一遍，缺一套不算测完。只有改动明确跟线程无关、并且是纯逻辑时，可以不切单线程和多线程；这种例外要在回复里写明。协程、挂起、调度、`Task`、`StartTask`、`co_await`、`yield` 一定涉及单线程和多线程，不能当成纯逻辑跳过。

单线程和多线程指 `ipcclient` 和 `ipcserver`。`logd` 只能多线程。`redisserver` 按它自己的编译选项是单线程。Linux 优先用本机 `.local/linux-hosts.json` 里的远程机器，这个文件不进仓库：4 核那台编译 Release 并跑多线程，2 核那台跑单线程，用户名以文件为准，只走公钥，不把密码写进代码或命令。文件不存在，或里面缺 IP、用户名时，先问用户。用户选择不提供，就改用 WSL，构建目录 `~/openidea-build-gcc13`；之后一直用 WSL，直到用户再次明确说用远程机器。Windows 用 `build-win`（Visual Studio 17 2022，x64），同样两种线程模式各编一次。两边的可执行文件都在 `deploy/bin`，换平台或换线程模式之前先编出要跑的那一套。本机 Windows 和 WSL 单线程测完把该平台切回多线程再编一次，不要把 `deploy/bin` 留在单线程；2 核远程机器固定只跑单线程，矩阵加 `--no-restore`，不编回多线程。回归入口是 `python3 tools/run_regressions.py --case all`。Linux 无论在远程还是 WSL，都用 `-DOI_IPC_SINGLE_THREAD=ON` 和 `OFF` 各编一次。

`ipcclient` 里四个 Mgr 都要跑到：`EchoMgr`、`LogMgr`、`RedisMgr`、`HoldMgr`。开关在 `deploy/config/ipcclient.toml` 的 `[Test]`：`echo`、`log`、`redis`、`hold`。一次只开一个。

测的是 `logd`、`ipcserver`、`ipcclient` 这三个进程。不要另写一个日志客户端。日志从 `LogMgr` 发出，回声从 `EchoMgr` 发出。

Linux 产物在 `.local/linux-hosts.json` 指定的远程机器上编、在那台机器上跑；用户没提供地址时改在 WSL 里编、在 WSL 里跑。构建目录是 `~/openidea-build-gcc13`，可执行文件在 `deploy/bin`。进程的当前目录必须是某层 `bin`，配置在同级的 `config`。`logd` 读 `../config/logd.toml`，另外两个读 `../config/<名字>.toml`。

跑之前把这三份 toml 拷到临时目录再改，不要改仓库里的那份。日志目录改到 `/tmp` 下面，不要写进仓库。

顺序是先 `logd`（端口 19995），再 `ipcserver`（等到输出 `ipc server listen`，端口 17981），最后 `ipcclient`。

回声看客户端退出码和这一行：`ok` 等于 `[TestEcho].total`，`mismatch=0`，`rpc_fail=0`。

日志看日志服落盘的文件字节，不要只看客户端打印的 `enqueue_rate`。那个数是塞进发送缓冲的速度。端到端用文件字节除以从启动客户端到文件不再增长的时间。文件总量不要超过 10GB，超过 2GB 就停。测完删掉临时日志目录，并停掉这三个进程。

Redis 也单独测。开关是 `[Test] redis`。先起本机 Redis（`maxmemory 1536mb`），再起 `redisserver`（等到 `redis server listen`，端口 16379），最后 `ipcclient`。客户端连的是 redisserver，不是 Redis。看退出码，以及 `redis integrity ok` 和 `redis stress` 那一行里 `bad=0`。测完停掉 `redisserver`。

`HoldMgr` 的开关是 `[Test] hold`。要同时起 `ipcserver` 和 `redisserver`。看退出码，以及 `hold done bad=0`。

# Redis 单 key 接口

- 常用 Redis 命令提供明确的单 key 接口，key 使用 std::string；值支持 string 和 rpcdata，按模板序列化/反序列化，统一返回 int32_t 错误码。
- 禁止隐式多 key 查询、自动合并成 MGET/MSET 或隐藏跨 key 操作。标准接口可以不提供多 key 能力；不得假设 Redis 集群的不同 key 位于同一槽。
- PING、Lua 及确需多 key 的特殊命令保留原生 CallRedis，由调用方显式指定命令和参数并处理集群约束。

- Redis/RPC 等输出引用接口按“单条语句 co_await、检查返回码、使用输出”顺序调用；依赖上一步输出时不要在一个 `&&` / `||` 表达式里串多个 co_await。Redis 整数标记数组用 `vector<int64_t>`（Redis 整数回执是 64 位，不是因为 0/1 标志需要 64 位）。`vector<bool>` 是标准库特化，禁止作为模板结果；`WhenAll` 汇合 `TaskResult<bool>` 时返回 `vector<int32_t>` 的 0/1。回归用例同样遵守，失败日志要能定位具体检查。

# 严禁多继承

- 全仓库自有 C++ 代码（基础库、模块、测试及生成器产物）严禁多继承，一个类或结构体最多只能有一个直接基类。
- 纯接口、空基类、mixin、CRTP、`std::enable_shared_from_this` 等辅助基类均不例外；不得为了代码复用或适配接口叠加基类。
- 需要组合能力时使用成员对象、普通辅助函数或显式回调；共享对象的所有权由外部管理，不通过增加基类实现。
- 修改时一并排查已有多继承，生成代码从生成器修正。第三方原始源码不擅自改写，但自有包装不得引入多继承。
- 检查入口：`python tools/check_inheritance.py`；检查器自身用例：`python tools/test_inheritance.py`。完整矩阵自动执行这两项。
- 会话管理器（包括路由管理器）按进程常驻管理，不按目标地址动态创建/回收管理器；动态生命周期放在连接状态和会话上，使用组合保存发送策略与待发请求。

# 名字空间归属

框架代码按功能放进 `OI`，不按目录名堆在一起。模块里的业务类先不放进去。

| 名字空间 | 放什么 |
| --- | --- |
| `OI::Base` | 字节缓冲、配置、单例 |
| `OI::File` | 路径 |
| `OI::Thread` | 线程池、任务、锁 |
| `OI::Coro` | 协程任务、等待、让出。头文件 `oi_coro.h`，在 base 里，不依赖 RPC |
| `OI::Net` | poll、套接字、通道、会话 |
| `OI::Timer` | 时间轮、时间辅助 |
| `OI::Log` | 日志客户端 |
| `OI::Redis` | Redis 客户端 |
| `OI::Http` | HTTP 客户端 |
| `OI::Rpc` | 序列化基类、协议和调用 |
| `OI::RpcData` | 生成出来的 rpcdata |

头文件名跟着功能走，框架头和对应的 cpp 都用 `oi_` 前缀，例如 `oi_rpc.h`、`oi_file.h`、`oi_thread.h`。三方库和 `port/win` 里的系统兼容头不用这个前缀。

# 应用层不要睡线程

应用层要空等，先打开 `docs/coroutine.md` 和 `modules/ipcclient/src/log_mgr.cpp` 的 `Pump`，确认写法是 `co_await OI::Coro::yield(毫秒)`，对上了再写。不要换成 `sleep`、`sleep_for`、`Sleep`、`usleep`。单线程只有一条线程，睡下去 poll 停转。

# 关键词对上文档

请求里出现下表的词时，先读对应文档，再按标准用例改。不要另写一套挂起、配对或绑定。

| 关键词 | 文档 | 标准用例 |
| --- | --- | --- |
| 协程、Task、StartTask、co_await、yield、让出、挂起、睡眠、sleep、sleep_for | `docs/coroutine.md` | `modules/ipcserver/src/ipcserver.cpp` 的 `HandleRpc`；`modules/ipcclient/src/echo_mgr.cpp` 的 `StartTask`；空等用 `modules/ipcclient/src/log_mgr.cpp` 的 `Pump` 里 `co_await OI::Coro::yield` |
| RPC、RpcCall、HandleRpc、回包、调用号 | `docs/coroutine.md` 的服务端和客户端 | 同上，推迟回包用同文件的 `Hold` |
| Redis、redisserver、RESP | `docs/cbaselib.md` 的 redis 段 | `modules/ipcclient/src/redis_mgr.cpp` 的 `Check` |
| 定时器、时间轮、AddTimer | `docs/timer.md` | `Hold` 里的 `co_await OI::Coro::yield` |
| 协议、proto.xml、.proto、rpcdata、模块 | `docs/module.md` | `modules/protoconfig/rpc.proto` 的 `AllEcho`、`Hold` |
| 端口、复用、bind、TIME_WAIT、监听 | `docs/cbaselib.md` 的 net 段 | `cbaselib/port/oi_net_os.cpp` 的 `NetOs_Bind` |
| Web、HTTP、JSON、urlencode、二进制 body | `docs/module.md` 的 Web 段 | `modules/webserver` 的四个路径；`modules/webclient` 的 `Run` |
| 分层、Channel、Session、会话管理器、逻辑 mgr、裸包、转发、CloneSession | `docs/develop.md` 第 1 节 | `modules/ipcserver/src/ipcudp.cpp` 的 `IpcUdp` |
| 目录、src、类外函数、全局/静态变量、友元、StartTask、单线程/多线程选型 | `docs/develop.md` 第 2、6 节 | `OI_RPC_BIND_SELF`、`OI_NOTIFY_BIND_SELF` |
| 错误码、iCode、errordef、自定义结构、手写序列化 | `docs/develop.md` 第 3、4 节 | `cbaselib/include/oi_errno.h` |
| 时间、计时、GetTime、毫秒、过期、ttl、缓存 | `docs/develop.md` 第 5 节 | `cbaselib/timer/inc/oi_timer_helper.h` 的 `GetTime` |
| 测试、回归、补用例、全量、clangd | `docs/develop.md` 第 7 节、`docs/testing.md`；远程 Linux 另读 `.cursor/skills/linux-remote-test/SKILL.md` | `modules/ipcclient/src/udp_mgr.cpp` 的 `UdpMgr` |
| 提交、推送、子模块、todo | `docs/develop.md` 第 8 节 | 本文件「提交就是提交并推送」 |


# 字段、参数和局部变量命名规范

适用于自有 C++ 类成员、普通函数/方法的参数和局部变量。类的私有成员（含静态成员）使用 `m_` + 类型前缀 + 大驼峰业务词；公开/保护成员、参数和局部变量使用类型前缀 + 大驼峰业务词。`struct`、`rpcdata` 成员保留现状，不加 `m_`；生成协议代码的参数和局部变量不参与本轮规范化。第三方库、系统 ABI、标准库固定接口、函数名、类型名、枚举项和宏名不在本条范围内。

- 类型前缀示例：`iCount`、`vRewardList`、`mItems`、`stBaseInfo`、`sName`、`bIsActive`。私有成员示例：`m_iCount`、`m_sName`、`m_stMutex`；不保留其他旧式前导下划线。
- 参数和局部变量都用类型前缀加大驼峰，如 `iCount`、`stLaunch`、`fnFunction`。名字本身已经有语义，不再加 `Param`。去掉后缀后只剩类型前缀、不成词的，保留 `Param`，如 `iParam`、`sParam`、`stParam`。和成员或已有局部变量撞名时，参数改用 `Local` 后缀。RPC 宏固定参数名为 `stRequest`、`stResponse`，需要上下文时为 `stContext`。
- 整数（含定宽整数、size_t、ID、计数、枚举值）用 `i`；vector 用 `v`；map 用 `m`；结构体/类值对象用 `st`；string 用 `s`；bool 用 `b`。
- 补充类型：float 用 `f`，double 用 `d`；裸指针/智能指针用 `p`；函数指针/可调用对象用 `fn`；array/list/deque/set 等容器沿用 `v`，unordered_map 等映射沿用 `m`。atomic 按内部值类型命名，引用按被引用类型命名。
- 模板中的未知值类型、optional、迭代器、时间点、协程句柄、锁等值对象用 `st`，不得凭变量名猜成指针或整数。
- 见名知意，避免单字母和不明确缩写；时间字段写出单位（如 `iTimeoutMilliseconds`），业务词汇沿用项目约定，阵容统一使用 `Arrange`。
- 开发和修改代码前阅读并遵守 [关键规则](docs/09-key-rules.md)，业务词汇按其中第 9 节执行。
- 修改字段时同步修改定义、构造初始化、访问点、协议源文件、生成器和文档；不要只改生成产物。
- 二进制协议字段号不随命名变化。已有配置键和外部 JSON 键保持兼容，协议可用 `[json 原键名]` 指定 JSON 名称。
- 去掉旧下划线后检查参数/局部变量遮蔽成员，尤其 setter 不能变成自赋值。构造函数初始化列表里，冒号左边是成员、括号里是同名参数，这是允许的。函数体里不能靠同名自赋值，改名区分，不用 `this->` 掩盖。重命名必须跑真实收发回归，不能只看编译。
- 命名检查：`python3 tools/test_field_names.py`；C++ 成员/参数/局部变量扫描使用安装了 libclang 的 Python 运行 `tools/check_field_names.py --build-dir ~/openidea-build-gcc13`。完整回归使用 `python3 tools/run_regressions.py --case all`，按上面的平台和线程要求跑，包含原四类测试以及注册发现、Web 收发。

# 容器遍历与迭代器

- 不需要删除元素、位置操作或手动推进时，使用范围 for：只读 `const auto&`，修改元素 `auto&`；map 优先使用引用结构化绑定。
- 迭代器属于特殊命名，允许 `it`、`nextIt` 等不带类型前缀；解引用得到的元素和结构化绑定按实际类型命名，例如 `const auto& [sKey, stValue]`。
- 需要 erase 后继续、迭代器位置或 JSON 成员名等迭代器专属接口时保留显式迭代，声明用 `auto`；不得为了改范围 for 改变遍历/删除语义。
- 迭代器、元素引用和范围 for 的隐藏迭代器跨 `co_await`、`yield` 的条件见上面的 Review：容器在挂起期间必须保持稳定，否则先拷出 key、下标或值。

# C++ 大括号与排版

- 自有手写 C++（基础库、模块及自有平台兼容层）使用 Allman 风格，代码块左花括号单独换行。
- `if`、`else`、`for`（含范围 for）、`while`、`do` 的语句体必须使用 `{}`，单条语句和空语句也不例外；禁止省略大括号。
- 函数、类、名字空间、lambda 等代码块也展开，不压成一行；不改变初始化列表的语义。
- 使用仓库 `.clang-format`，固定 clang-format 18.1.8；执行 `python3 tools/format_cpp.py`，只检查用 `--check`，工具不在 PATH 时传 `--clang-format`。
- 第三方源码不格式化；自动生成文件由生成器维护，不手改生成产物。
- 格式化后检查 diff，并按本文件的平台和线程要求完成回归，不能只以格式检查代替编译和真实收发。
