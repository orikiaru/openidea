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

# 要用户拍板就给选项

需要用户做决定时（方案取舍、范围、是否继续），用选择题工具给出选项，推荐项放第一个并标明推荐，不要只在正文里列问题让用户打字回答。每个问题单独能看懂，选项写清楚各自的后果。

- 能从代码、配置、git 历史自己查出来的，查完写进选项，不要反问用户。
- Review 或修复建议要用户决定改不改时，选项里必须有一项「不修改」；用户没选之前不动代码。
- 选项依据（代码位置、文档原句、实测结果）写进问题描述，不要只给结论。

# 技能放在 .agents/skills

流程类技能（提交、远程测试等）统一放 `.agents/skills/<name>/SKILL.md`，Claude Code、Codex、Cursor 都从这里找，不在 `.cursor/skills`、`.claude/skills` 再放副本。`name` 与目录名一致，只用小写字母、数字、连字符，中文触发词写进 `description`。经验性内容优先补进已有技能，不为一个小技巧新开目录。技能只写「事情怎么做」；代码怎么写仍以本文件和 `docs/` 为准，不在技能里复制一份。

技能里不写死个人路径、机器地址、账号和凭据。机器信息放本机 `.local/`（不进仓库），缺了先问用户；能自查的（工具路径、构建目录、端口占用）自己查。

# 提交就是提交并推送

用户说「提交」时，包含本地 `git commit` 和推送到当前分支的远程。推送用普通 `git push`，不要强推。远程拒绝或需要登录时，把验证信息交给用户。

# 锁用项目封装

- 自有代码（基础库、模块、测试）加锁和等待一律用 `cbaselib/base/inc/oi_lock.h`：互斥用 `OI::Thread::Mutex` + `MutexGuard`，自旋用 `SpinLock` + `SpinLockGuard`，等待通知用 `MutexCond`（`Wait`、`WaitUntil`、`Signal`、`Broadcast`）。
- 禁止直接写 `std::mutex`、`std::recursive_mutex`、`std::lock_guard`、`std::unique_lock`、`std::scoped_lock`、`std::condition_variable(_any)`。`oi_lock.h` 自身的实现和第三方源码除外。
- `MutexCond` 不带条件参数，等待一律写成 `while (条件不满足) { Wait(...); }`，带超时的用 `WaitUntil` 的返回值退出循环，防虚假唤醒。
- 单线程编译下这些锁和条件变量是空实现，不会真的阻塞；单线程路径不能靠它们等待，要转 poll。
- 封装里没有的能力（例如新的等待方式），先在 `oi_lock.h` 里补，单线程和多线程两套一起补，不在业务代码里绕过去用标准库。
- 改代码时看到存量的标准库锁，一并换掉。
- 可变资源按对象归属管理，普通事件掩码由 IO 线程变更；跨线程锁只用于真实共享状态和后台通知，先核对读写线程与生命周期。

# 杂活交给 subagent

主对话只做核心代码的分析、修改和对 subagent 结果的核对。下面这些费 token 但不难的活，一律派 subagent 做，不在主对话里自己动手：

- 文档同步：改完代码后更新 `docs/`、时序图、头文件注释以外的说明文字；
- 编写或补充测试用例、回归脚本里的检查项；
- 编译、回归矩阵、日志翻查；
- 批量的机械改名、格式类改动。

模型和编译回归一样：Cursor 里用 `grok-4.7-high`，Codex 里用 Terra。派活时把改动点、要遵守的规则和验收标准写清楚。subagent 回来后，主对话要看它的 diff，确认没改出代码范围以外的东西。委托测试时主对话还须亲审测试输入、验收条件与失败路径，确认错误结果确实会使测试失败。这条也是长期偏好，不用每轮再问。

# Review 与开发优先级

- 逻辑稳定是第一优先级：结果正确、生命周期安全、并发状态一致、异常收尾完整；其次是应用层调用简单直接，以及故障有据可查。
- 已确认的崩溃、UB、数据错误、回包错配、丢失唤醒等正确性问题应及时修复，不等线上出事故。Review 必须说明触发条件、代码依据、实际后果和最小修复方向，推测风险单独标明。
- 每次框架 review（含提交前自审）都要沿受影响的调用链，从触发事件追到业务调用方，核对线程边界、返回码、输出内容和资源最终归属。即使 diff 只改一层，也要读上下游怎样解释结果。例如正文收到一半时，要分别核对正常 FIN 和异常 RST 最终让调用方拿到什么。
- 网络设计有疑问且本地参考目录 `F:\杂货\n2world\ue\net` 存在时，对照其实现核对线程边界。
- 对涉及回调、挂起、复用或重连的路径，按实际执行顺序检查事件交错。例如最后一批数据和 FIN 在同一轮到达时，连接在排队请求取得它之前是否已标为不可复用；新会话接管后，旧会话迟到的清理是否仍有权删除当前登记。共享状态要对照检查写端、读端，以及锁释放后仍被持有的引用。
- 对能实测的疑点，构造触发条件，用业务结果判定成败。断连用真实套接字，并确认断开前收到了哪些数据；异步清理用可观察事件确认已执行。修复回归尽量让同一用例在旧行为下因预期的错误而失败，再验证修复后通过。后续请求恢复正常，不能代替对原请求结果的断言。
- 每轮结论写清核对了哪条关键路径、证据来自代码还是实测、哪些条件仍未覆盖。已有回归通过只说明其中的断言成立；发现证据不足时保留疑点，也可以明确报告本轮未发现问题。
- 可以接受少量性能损耗；没有实际瓶颈或测量证据，不因额外拷贝、锁、调度优先级、队列或连接分配方式而默认重构。性能优化等真实问题出现并定位后再做。
- 近期优化集中在模板、编译效率和运行效率的折中，不单追一边。能不用多层嵌套模板就不用，协议生成的编译量随协议条数上涨。运行更快、编译更贵的方案要提供给使用方，由他们自己选，不替他们定死。字段分发的测量在 `docs/pb-dispatch.md`：宏字段表编译快，`switch` 解码大约快 1 微秒、编译大约慢到 6 倍，`map` 两边都没有更好。本仓库生成的协议解码目前用宏。
- 异常应能通过已有日志和错误码定位：按需保留操作、服务/会话/调用标识、失败阶段及原始原因。返回统一错误码时不能让关键诊断信息无处可查；不默认增加大型监控体系，也不为每次成功调用刷日志。
- 诊断日志不直接输出完整请求、凭据或业务敏感值；外部错误文本按需脱敏、限长，高频重复故障可限频，但不能完全丢失首次原因和发生情况。
- 协程结果和多人等待先复用现有任务机制；分别核对连接配置域名与每次拨号地址的归属。
- 优先维护现有稳定接口和架构。只有当前实现妨碍正确性或应用层安全使用时才考虑重构；没有实质问题可以明确报告未发现，不为凑优化项制造改动。
- 协程硬底线：`RunBlocking` 的同步回调及其自有调用链禁止 `co_await`、`yield`、`StartTask`、`TaskScope` 和 RPC、Redis、HTTP 异步接口；Lambda 只许具名捕获；`MutexGuard` 等锁守卫不跨 `co_await`、`yield`；挂起后迭代器、元素引用、`string_view`、`span` 按失效处理，除非容器挂起期间保持稳定。捕获寿命、单线程 poll 线程限制、阻塞回归等细则见 `docs/coroutine.md` 的硬性约束。

流式 source/sink 回调不得阻塞 poll 或保存借用的 span/视图；返回 `OI_ERR_BUSY` 表示当前片未消费，重试前不得产生消费副作用。验证流取消与释放的回归须有旧状态取消并释放的可观察证据；新请求健康另行验证。普通协程 handler 可在客户端断开后完成并丢弃响应。

# 回声和日志要分开测

有行为的改动，Linux 和 Windows 都要跑，单线程、多线程各跑一遍，缺一套不算测完。编译和回归一律交给 subagent 跑，主对话不直接跑矩阵。每次测试前后按进程归属清理，用 `python tools/kill_test_procs.py`（并行时加 `--lane`），清理失败或有残留必须报错。平台与构建目录、纯逻辑例外、四个 Mgr 的开关、启动顺序、判定行和全量记录等细则见 `docs/testing.md` 的硬性约束。

# Redis 单 key 接口

Redis 标准接口只提供单 key 接口，禁止隐式多 key 查询或自动合并成 MGET/MSET，确需多 key 的走原生 `CallRedis`。细则见 `docs/cbaselib.md` redis 段的「单 key 接口约束」；输出引用接口的 `co_await` 写法见 `docs/coroutine.md` 的硬性约束。

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

# 单次 RPC 用带回复的那套

单次 RPC 按服务名用 `RouteManager::GetInstance().Call(服务名, 请求, 回复, 时长)` 带回复调用，先看返回码再读回复；直连只用于有状态的对端。细则见 `docs/coroutine.md` 的硬性约束。

# 应用层不要睡线程

应用层空等只用 `co_await OI::Coro::yield(毫秒)`，不要 `sleep`、`sleep_for`、`Sleep`、`usleep`，单线程睡下去 poll 停转。细则见 `docs/coroutine.md` 的硬性约束。

# 单例周期任务走 Update

单例周期任务在类里写公有的 `OI::Coro::Task OnSecondUpdate(time_t)` 或 `OnTickUpdate(uint64_t)`，两个只能写一个，不自己挂 `BaseTimer`、`AddTimer` 循环或 `CallOnLoop` 催促。细则见 `docs/timer.md` 的单例更新段。

# 关键词对上文档

请求里出现下表的词、改到下表规范管的路径或代码时，先读对应文档，再按标准用例改。不要另写一套挂起、配对或绑定。

表由 `.agents/rules/index.toml` 生成，改路由只改那份索引，再跑 `python tools/agent_route.py --write`。Claude Code 由 `.claude/settings.json` 的 hook 强制执行：用户消息和派给 subagent 的提示词命中就注入必读文档；编辑命中路径或写入对应代码时，本会话没读过该文档会被拦下，读完重试。Codex、Cursor 没有这套 hook，按本表自觉先读。

<!-- route-index:begin 由 tools/agent_route.py --write 生成，改 .agents/rules/index.toml -->

| 关键词 | 文档 | 标准用例 |
| --- | --- | --- |
| 协程、Task、StartTask、TaskScope、co_await、yield、让出、挂起、睡眠、sleep、sleep_for、RunBlocking、lambda、捕获、WhenAll | `docs/coroutine.md` 的硬性约束 | `modules/ipcserver/src/ipcserver.cpp` 的 `RegisterRpcHandler`；`modules/ipcclient/src/echo_mgr.cpp` 的 `StartTask`；空等用 `modules/ipcclient/src/log_mgr.cpp` 的 `Pump` 里 `co_await OI::Coro::yield` |
| RPC、RpcCall、RegisterRpcHandler、RouteManager、回包、调用号、CallAll、CallMany | `docs/coroutine.md` 的服务端和客户端 | 同上，推迟回包用同文件的 `Hold` |
| 单向消息、Notify、NotifyAll、NotifyMany | `docs/cbaselib/notify.md` | `modules/ipcserver/src/ipcudp.cpp` 的 `RegisterNotifyHandler`；按服务名见 `docs/registry.md`；协程侧见 `docs/coroutine.md` 的单向消息 |
| Redis、redisserver、RESP、CallRedis | `docs/cbaselib.md` 的 redis 段 | `modules/ipcclient/src/redis_mgr.cpp` 的 `Check` |
| 定时器、时间轮、AddTimer、BaseTimer | `docs/timer.md` | `Hold` 里的 `co_await OI::Coro::yield` |
| 周期任务、Update、心跳、定时拉取、单例循环、OnSecondUpdate、OnTickUpdate | `docs/timer.md` 的单例更新段 | `cbaselib/proto/src/oi_registry_client.cpp` 的 `RegistryClient::OnSecondUpdate`；`modules/ipcclient/src/timer_mgr.cpp` 的 `OnTickUpdate` |
| 协议、proto.xml、.proto、rpcdata、模块 | `docs/module.md` | `modules/protoconfig/rpc.proto` 的 `AllEcho`、`Hold` |
| 端口、复用、bind、TIME_WAIT、监听 | `docs/cbaselib.md` 的 net 段 | `cbaselib/port/oi_net_os.cpp` 的 `NetOs_Bind` |
| Web、HTTP、JSON、urlencode、二进制 body | `docs/module.md` 的 Web 段 | `modules/webserver` 的五个路径；`modules/webclient` 的 `Run` |
| 分层、Channel、Session、会话管理器、逻辑 mgr、裸包、转发、CloneSession | `docs/develop.md` 的第 1 节 | `modules/ipcserver/src/ipcudp.cpp` 的 `IpcUdp` |
| 目录、类外函数、全局变量、静态变量、友元、单线程/多线程选型 | `docs/develop.md` 的第 2、6 节 | `RegisterRpcHandler(&类::成员)`、`RegisterNotifyHandler(&类::成员)` |
| 错误码、iCode、errordef、自定义结构、手写序列化 | `docs/develop.md` 的第 3、4 节 | `cbaselib/include/oi_errno.h` |
| 时间、计时、GetTime、毫秒、过期、ttl、缓存 | `docs/develop.md` 的第 5 节 | `cbaselib/timer/inc/oi_timer_helper.h` 的 `GetTime` |
| 测试、回归、补用例、全量、矩阵、验收、clangd、logd、EchoMgr、LogMgr、RedisMgr、HoldMgr、run_regressions、test_matrix | `docs/testing.md` 的硬性约束 | `modules/ipcclient/src/udp_mgr.cpp` 的 `UdpMgr`；另见 `docs/develop.md` 第 7 节，远程 Linux 另读 `.agents/skills/linux-remote-test/SKILL.md` |
| 提交、推送、子模块、todo | `docs/develop.md` 的第 8 节 | AGENTS.md「提交就是提交并推送」 |

<!-- route-index:end -->


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
- 迭代器、元素引用和范围 for 的隐藏迭代器跨 `co_await`、`yield` 的条件见 `docs/coroutine.md` 的硬性约束：容器在挂起期间必须保持稳定，否则先拷出 key、下标或值。

# C++ 大括号与排版

- 覆盖基类虚函数（析构函数除外）时不写前导 `virtual`，在声明末尾写 `override`；`-Wsuggest-override` 强制检查，clang 把未使用的 lambda 捕获视为编译错误。
- 自有手写 C++（基础库、模块及自有平台兼容层）使用 Allman 风格，代码块左花括号单独换行。
- `if`、`else`、`for`（含范围 for）、`while`、`do` 的语句体必须使用 `{}`，单条语句和空语句也不例外；禁止省略大括号。
- 函数、类、名字空间、lambda 等代码块也展开，不压成一行；不改变初始化列表的语义。
- 使用仓库 `.clang-format`，固定 clang-format 18.1.8；执行 `python3 tools/format_cpp.py`，只检查用 `--check`，工具不在 PATH 时传 `--clang-format`。
- 第三方源码不格式化；自动生成文件由生成器维护，不手改生成产物。
- 格式化后检查 diff，并按本文件的平台和线程要求完成回归，不能只以格式检查代替编译和真实收发。
