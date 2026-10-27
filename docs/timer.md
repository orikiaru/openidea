# 定时器

定时器是分层时间轮，由 `BaseTimerManager` 管。

| 文件 | 作用 |
| --- | --- |
| `base_timer` | 一个定时器。`OnTimer` 返回 true 会按原间隔再挂上，返回 false 则不再挂 |
| `base_timer_level` | 一层轮子 |
| `base_timer_helper` | 跨天、跨周、跨月、跨年，以及按星期几切的一周。`TimeMark` 存时间和偏移，偏移 `86400` 再偏一天 |
| `base_timer_manager` | 初始化、推进 tick、执行到期定时器 |

返回 false 前要销毁这个定时器，或者先从管理器摘掉再销毁。

## 配置

进程 toml 的 `[Timer]`：

- `offset_ms`：一跳多少毫秒，要能整除 1000
- `timer_lv_count`：层数，必须大于 2
- `timer_lvs`：每一层的格子数，长度等于层数，最后一级必须是 1

`Init` 读这些配置，并在 poll 循环上挂一个 libuv 定时器，按 `offset_ms` 叫醒。

## 谁来转轮子

libuv 到点只调用 `OnSignal`。`OnSignal` 把 `Update` 丢进线程池，不在 poll 回调里转时间轮。单线程在主循环里执行 `Update`，多线程在有序队列线程执行。

`Update` 里到期的 `OnTimer` 可以再次 `AddTimer`。时间轮上的锁是可重入的 `Mutex`。

回调时间按 `offset_ms` 对齐。没走完的一跳留到下次，不会把周期越拖越长。外部 `AddTimer` 按墙上时间算到期 tick，不会因为下一跳马上就到而提前触发，最多晚一个 tick。`OnTimer` 返回 true 再挂上的周期定时器按逻辑 tick 续，停顿之后错过的跳会补上。`AddTimer(lambda)` 的 lambda 进有序队列，紧跟这次 `Update`，不排在普通任务后面。

业务侧用 `AddTimer(fn, millisecond)`，或者传入一个 `BaseTimer`。

协程不等在 poll 回调里。`coro::yield(ms)` 先切到 poll 线程 `AddTimer`，到点再把协程句柄放回调度队列。
