# 定时器

定时器是分层时间轮，由 `BaseTimerManager` 管。

| 文件 | 作用 |
| --- | --- |
| `oi_timer` | 一个定时器。`OnTimer` 返回 true 会按原间隔再挂上，返回 false 则不再挂 |
| `oi_timer_level` | 一层轮子 |
| `oi_timer_helper` | 跨天、跨周、跨月、跨年，以及按星期几切的一周。`TimeMark` 存时间和偏移，偏移 `86400` 再偏一天 |
| `oi_timer_manager` | 初始化、推进 tick、执行到期定时器 |

返回 false 前要销毁这个定时器，或者先从管理器摘掉再销毁。

## 配置

进程 toml 的 `[Timer]`：

- `offset_ms`：一跳多少毫秒，要能整除 1000
- `timer_lv_count`：层数，必须大于 2
- `timer_lvs`：每一层的格子数，长度等于层数，最后一级必须是 1

`Init` 读这些配置，并在 poll 循环上挂一个 libuv 定时器，按 `offset_ms` 叫醒。

## 谁来转轮子

libuv 到点调用 `OnSignal`，`OnSignal` 在 poll 线程上直接执行 `Update`，不交给工人。单线程和多线程都一样，时间轮只在 poll 线程上转。

`Update` 每一跳先在锁里把到期的定时器摘下来，放锁后再逐个调 `OnTimer`。`OnTimer` 里可以 `AddTimer`，不会挡住别的线程挂定时器。返回 true 的定时器再加锁挂回去。回调跑在 poll 线程上，耗时期间网络收发停住；不能调 `CallRedisSync`、`Lookup`、`PullSync`、`Publish`。`CallRedisSync` 在这里会抛 `logic_error`。注册表的 `Lookup`、`PullSync`、`Publish` 在需要真正等待时同样抛 `logic_error`，协程里用 `PullAsync`。要等结果就 `StartTask` 拉一个协程去做。

单个回调抛异常时记录到任务错误处理器，并继续执行本批其他回调；`OnSignal` 另外兜住 `Update` 自身的异常，不让它穿出 libuv 回调，不能因异常让整套时间轮停转。lambda 定时器仍自动释放自身。自定义 BaseTimer 的异常路径须由其实现管理对象寿命，管理器不会猜测回调是否已经自行销毁对象，也不会在异常后自动续期。

`Init` 会检查上面三项配置，不合法就往 stderr 打原因并返回 false，进程退出。

回调时间按 `offset_ms` 对齐，用 `steady_clock` 计时。系统校时不影响相对延时，日历时间单独从系统时钟更新。没走完的一跳留到下次；外部 `AddTimer` 向上取整到到期 tick，不会提前触发。线程调度和回调执行仍可能造成额外延迟。`OnTimer` 返回 true 再挂上的周期定时器按逻辑 tick 续，停顿之后错过的跳会补上。`AddTimer(lambda)` 的 lambda 在 `Update` 摘除定时器、释放轮锁后直接执行，不再二次入队；回调应保持短小，耗时工作交给协程。

业务侧用 `AddTimer(fn, millisecond)`，或者传入一个 `BaseTimer`。

单例跟着时间轮更新：在 `DECLARE_SINGLETON` 的类里写公有的 `OI::Coro::Task OnTickUpdate(uint64_t iNowMilliseconds)` 或 `OI::Coro::Task OnSecondUpdate(time_t iNow)`。两个只能写一个，都写会编译失败；返回类型不是 `OI::Coro::Task` 也会 `static_assert` 失败。不写就不注册，等于空 Update。毫秒参数是秒时间戳乘 1000 再加毫秒；秒参数是 `time_t`。推进了 tick 才启动 `OnTickUpdate`；日历秒变化才启动 `OnSecondUpdate`，错过的秒不补。时间戳只在 `Update` 里写。

注册在单例第一次 `GetInstance` 构造完成后自动进行，析构前自动注销。注销后不再启动新的，已经在跑的协程不取消。注册表比所有单例先构造、后析构，析构顺序不会访问已经销毁的对象。`RunFramework` 开始前不分发，`main` 里的 `Init` 都在第一次 Update 之前做完；之后才第一次取到的单例从下一跳开始收到。时间轮在 poll 线程推进 tick 后，为每个注册的单例 `StartTask` 一条独立协程，只启动、不等待。单线程放进本线程的运行队列，多线程放进工人队列。一个单例慢，不会挡住别的单例，也不会挡住时间轮。同一单例的上一条协程没 `co_return` 之前不会再启动，期间到点的跳或秒直接跳过，不补，也不排队。更新里可以 `co_await`（RPC、Redis、`yield` 等），不能调 `CallRedisSync`、`Lookup`、`PullSync`、`Publish`、`sleep`。抛出的异常交给任务错误处理器报告，这条更新的在跑标志会清掉，下一跳或下一秒照常启动。多线程下，更新协程和工人、和其他单例的更新并发，成员要按需自己保护。

协程不等在 poll 回调里。`OI::Coro::yield` 接受毫秒整数，也接受 `OI::Timer::MS`。`MS` 在挂起前换成毫秒。大于 0 时先切到 poll 线程 `AddTimer`，到点再把协程句柄放回调度队列。小于等于 0 不进时间轮，只把句柄放回队列。

应用层时长只有 `OI::Timer::MS`。`Milliseconds`、`Seconds`、`Minutes`、`Hours`、`Days` 在构造时换成毫秒。单调钟用 `SteadyMilliseconds()`，不受系统校时影响。

RPC 和 Redis 的超时不走这条时间轮。它们各有一个 200 毫秒的周期定时器扫等待队列。RPC 按毫秒分桶；传裸整数的旧重载仍把这个整数当成秒，再乘 1000。Redis 超时来自 `RedisClient.timeout_ms`。`RedisExpire` 的参数也是 `MS`，发给 Redis 的 `EXPIRE` 用整秒。
