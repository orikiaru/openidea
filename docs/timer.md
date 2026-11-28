# 定时器

定时器是分层时间轮，由 `BaseTimerManager` 管。

| 文件 | 作用 |
| --- | --- |
| `oi_timer` | 一个定时器。`OnTimer` 返回 true 会按原间隔再挂上，返回 false 则不再挂 |
| `oi_timer_level` | 一层轮子 |
| `oi_timer_helper` | 跨天、跨周、跨月、跨年，以及按星期几切的一周。`TimeMark` 存时间和偏移，偏移 `86400` 再偏一天 |
| `oi_timer_manager` | 初始化、推进 tick、执行到期定时器 |

业务用回调登记，不自行管理定时器对象。框架内部对象登记可借用裸指针，或用 `unique_ptr` 把所有权交给时间轮：借用对象由调用方负责，必须活到回调结束或时间轮停服；时间轮不销毁它。拥有的对象在不再续期、异常或停服清理时由时间轮销毁，不在回调里自行删除。

## 配置

进程 toml 的 `[Timer]`：

- `offset_ms`：一跳多少毫秒，要能整除 1000
- `timer_lv_count`：层数，必须大于 2
- `timer_lvs`：每一层的格子数，长度等于层数，最后一级必须是 1

`Init` 读这些配置，并在 poll 循环上挂一个 libuv 定时器，按 `offset_ms` 叫醒。

## 谁来转轮子

libuv 到点调用 `OnSignal`，`OnSignal` 在 poll 线程上直接执行 `Update`，不交给工人。单线程和多线程都一样，时间轮只在 poll 线程上转。

`Update` 每一跳先在锁里取出到期回调，放锁后逐个执行。回调里可以登记其他回调，不会挡住别的线程挂定时器。回调跑在 poll 线程上，耗时期间网络收发停住；不能调 `CallRedisSync`、`Lookup`、`PullSync`、`Publish`。`CallRedisSync` 在这里会抛 `logic_error`。注册表的 `Lookup`、`PullSync`、`Publish` 在需要真正等待时同样抛 `logic_error`。运行期读 `Snapshot`。连接、登记、心跳和整量拉取由 `RegistryClient` 的单例秒更新 `OnSecondUpdate` 协程做：每秒一次，上一次没结束就跳过；`Snapshot` 未命中不立刻拉取，由下一次秒更新拉。要等结果就 `StartTask` 拉一个协程去做。

单个回调抛异常时不再续期，释放时间轮拥有的对象并报告任务错误处理器，再执行本批其他回调；`OnSignal` 另外兜住 `Update` 自身的异常，不让它穿出 libuv 回调。正常结束、不再续期时同样释放拥有的对象和回调捕获，借用对象仍由调用方管理。

正常停服或初始化回调失败后，框架等待 poll 和工人退出，再从时间轮移出待执行项并在锁外释放其拥有的对象和捕获，不执行尚未到期的回调。清理后再登记定时器返回 `OI_ERR_CANCELLED`；析构期间尝试重新登记也会被拒绝。借用对象不会被清理销毁。

`Init` 会检查上面三项配置，不合法就往 stderr 打原因并返回错误码，进程退出。

回调时间按 `offset_ms` 对齐，用 `steady_clock` 计时。系统校时不影响相对延时，日历时间单独从系统时钟更新。没走完的一跳留到下次；外部 `AddTimer` 向上取整到到期 tick，不会提前触发。线程调度和回调执行仍可能造成额外延迟。`AddTimer(lambda)` 的 lambda 在 `Update` 摘除定时器、释放轮锁后直接执行，不再二次入队；回调应保持短小，耗时工作交给协程。

层级边界上已到期的定时器在当前 tick 派发；高层先摘出整格，再将未到期的定时器重新挂入时间轮，避免同一格在本次推进中被反复处理。

业务侧用 `AddTimer(fn, interval_ms, count = 1)` 登记无参数回调，或接收 `OI::Timer::TimerContext&` 的回调。间隔毫秒数和执行次数均须为正，否则返回 `OI_ERR_INVALID_ARGUMENT`；重复定时器从回调结束时起等待原间隔，错过的周期不重放。默认执行一次，不支持登记或移除定时器对象。回调可移动捕获资源。上下文只在当前回调期间借用，不能保存引用或指针；`Remaining()` 返回本次执行之后剩余的次数，`SetRemaining(n)` 覆盖剩余次数（`n <= 0` 时不再触发），`Stop()` 停止后续触发。回调也可增加剩余次数。单例周期工作仍使用单例更新接口。

## 单例更新

单例要周期干活（心跳、定时拉取、过期清理），在类里写公有的 `OI::Coro::Task OnSecondUpdate(time_t)`，要比秒更细才用 `OnTickUpdate(uint64_t)`，两个只能写一个。框架按时间轮启动协程，上一次没 `co_return` 就跳过这一轮，所以函数体里直接顺序 `co_await` RPC、Redis，不用在途标志。不要自己挂 `BaseTimer`、`AddTimer` 循环、`CallOnLoop` 催促或回调链来做周期任务。到点判断用 `GetTime`/`SteadyMilliseconds` 记下次时间；别的线程要催一轮，只置一个原子标志，等下一次 Update 处理。

单例跟着时间轮更新：在 `DECLARE_SINGLETON` 的类里写公有的 `OI::Coro::Task OnTickUpdate(uint64_t iNowMilliseconds)` 或 `OI::Coro::Task OnSecondUpdate(time_t iNow)`。两个只能写一个，都写会编译失败；返回类型不是 `OI::Coro::Task` 也会 `static_assert` 失败。不写就不注册，等于空 Update。毫秒参数是秒时间戳乘 1000 再加毫秒；秒参数是 `time_t`。推进了 tick 才启动 `OnTickUpdate`；日历秒变化才启动 `OnSecondUpdate`，错过的秒不补。时间戳只在 `Update` 里写。

注册在单例第一次 `GetInstance` 构造完成后自动进行，析构前自动注销。注销后不再启动新的，已经在跑的协程不取消。注册表比所有单例先构造、后析构，析构顺序不会访问已经销毁的对象。`RunFramework` 开始前不分发，`main` 里的 `Init` 都在第一次 Update 之前做完；之后才第一次取到的单例从下一跳开始收到。时间轮在 poll 线程推进 tick 后，为每个注册的单例 `StartTask` 一条独立协程，只启动、不等待。单线程放进本线程的运行队列，多线程放进工人队列。一个单例慢，不会挡住别的单例，也不会挡住时间轮。同一单例的上一条协程没 `co_return` 之前不会再启动，期间到点的跳或秒直接跳过，不补，也不排队。更新里可以 `co_await`（RPC、Redis、`yield` 等），不能调 `CallRedisSync`、`Lookup`、`PullSync`、`Publish`、`sleep`。抛出的异常交给任务错误处理器报告，这条更新的在跑标志会清掉，下一跳或下一秒照常启动。多线程下，更新协程和工人、和其他单例的更新并发，成员要按需自己保护。框架自己用这条更新扫 RPC 超时：`RpcCallManager` 的 `OnTickUpdate` 每跳扫一次。

## 让出与时长

协程不等在 poll 回调里。`OI::Coro::yield` 接受毫秒整数，也接受 `OI::Timer::MS`。`MS` 在挂起前换成毫秒。大于 0 时先切到 poll 线程 `AddTimer`，到点再把协程句柄放回调度队列。小于等于 0 不进时间轮，只把句柄放回队列。

应用层时长只有 `OI::Timer::MS`。`Milliseconds`、`Seconds`、`Minutes`、`Hours`、`Days` 在构造时换成毫秒。单调钟用 `SteadyMilliseconds()`，不受系统校时影响。

Redis 的超时不走这条时间轮，另有一个 200 毫秒的周期定时器扫等待队列，超时来自 `RedisClient.timeout_ms`。`RedisExpire` 的参数也是 `MS`，发给 Redis 的 `EXPIRE` 用整秒。RPC 超时由 `RpcCallManager::OnTickUpdate` 每跳扫描：分桶仍按 200 毫秒刻度，桶内按精确截止时间判断，计时用 `OI::Timer::SteadyMilliseconds`。多线程下扫描协程在工人线程，单线程在 poll 线程的运行队列。超时精度取决于时间轮一跳（`offset_ms`，正常 10–50 毫秒）。
