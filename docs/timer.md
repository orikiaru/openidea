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

`Update` 每一跳先在锁里把到期的定时器摘下来，放锁后再逐个调 `OnTimer`。`OnTimer` 里可以 `AddTimer`，不会挡住别的线程挂定时器。返回 true 的定时器再加锁挂回去。回调跑在 poll 线程上，耗时期间网络收发停住；不能调 `CallRedisSync`、`Lookup`、`PullSync`、`Publish`。`CallRedisSync` 在这里会抛 `logic_error`。要等结果就 `StartTask` 拉一个协程去做。

单个回调抛异常时记录到任务错误处理器，并继续执行本批其他回调；`OnSignal` 另外兜住 `Update` 自身的异常，不让它穿出 libuv 回调，不能因异常让整套时间轮停转。lambda 定时器仍自动释放自身。自定义 BaseTimer 的异常路径须由其实现管理对象寿命，管理器不会猜测回调是否已经自行销毁对象，也不会在异常后自动续期。

`Init` 会检查上面三项配置，不合法就往 stderr 打原因并返回 false，进程退出。

回调时间按 `offset_ms` 对齐，用 `steady_clock` 计时。系统校时不影响相对延时，日历时间单独从系统时钟更新。没走完的一跳留到下次；外部 `AddTimer` 向上取整到到期 tick，不会提前触发。线程调度和回调执行仍可能造成额外延迟。`OnTimer` 返回 true 再挂上的周期定时器按逻辑 tick 续，停顿之后错过的跳会补上。`AddTimer(lambda)` 的 lambda 在 `Update` 摘除定时器、释放轮锁后直接执行，不再二次入队；回调应保持短小，耗时工作交给协程。

业务侧用 `AddTimer(fn, millisecond)`，或者传入一个 `BaseTimer`。

协程不等在 poll 回调里。`OI::Coro::yield` 接受毫秒整数，也接受 `OI::Timer::MS`。`MS` 在挂起前换成毫秒。大于 0 时先切到 poll 线程 `AddTimer`，到点再把协程句柄放回调度队列。小于等于 0 不进时间轮，只把句柄放回队列。

应用层时长只有 `OI::Timer::MS`。`Milliseconds`、`Seconds`、`Minutes`、`Hours`、`Days` 在构造时换成毫秒。单调钟用 `SteadyMilliseconds()`，不受系统校时影响。

RPC 和 Redis 的超时不走这条时间轮。它们各有一个 200 毫秒的周期定时器扫等待队列。RPC 按毫秒分桶；传裸整数的旧重载仍把这个整数当成秒，再乘 1000。Redis 超时来自 `RedisClient.timeout_ms`。`RedisExpire` 的参数也是 `MS`，发给 Redis 的 `EXPIRE` 用整秒。
