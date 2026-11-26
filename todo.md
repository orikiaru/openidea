# 待办

## 当前事实边界（2026-11-26）

- **2026-11-26 四路全量通过**：Windows MT/ST 各 23 项通过、1 项 Unix 专项跳过；远程 Linux MT/ST 各 24 项通过，四路真 MySQL 通过。Linux localhost 双栈回退子检查因地址族环境未覆盖。这次 Windows MT 没有出现 WinError 5，但此前连续两轮的清理失败仍然算数，待办 1 不关。详见 [最近测试报告](<docs/lasttesting.md#全量2026-11-26四路通过>)。
- 全仓格式检查仍有 **13 份 HEAD 既有失败**；最新两轮未重跑 **ASan/coro**，历史专项不冒充最新验收。规范化/外围工具移到 [条件待办 D](<todonext.md#d-外围工具规则与检查覆盖>)。
- **2026-11-26 全仓审查（15cdbac）**：7 个分区只读审查，全部为代码证据、未实测。无新 P1；2 条框架 P2（Linux POLLERR 停读、零窗口关闭不回收）和 6 条断言补强列为第 9 组，正在修；UDP 来源无上限（P2，只记录）与约 10 组 P3 排进 [todonext](<todonext.md>)。详见 [11-26 review](<docs/review-20261126.md>)。
- 本轮新增**规范路由 hook 与自测**（工具脚本，这批改动未触及 C++，未跑编译回归），待验项见 [条件待办 D](<todonext.md#d-外围工具规则与检查覆盖>)。近期只保留下列八组，按顺序执行最小行动，不借机大重构；有行为改动后按 [测试规程](<docs/testing.md>) 跑规定平台/线程回归并加对应有效负例。
- 其他独立未完成事项与复现时间线合并到根目录 [todonext](<todonext.md>)：推迟不等于不存在、已修或已接受；达到各项触发条件再提级。完成历史见 [归档](<docs/fixed.md>)，原审查证据见 [11-24](<docs/review-20261124.md>) / [11-25](<docs/review-20261125.md>) / [11-26](<docs/review-20261126.md>)，不再复制三轮同一事项。

## 1. Windows MT registry 清理 WinError 5：失败瞬间取证/定位

- **价值**：最终验收已再次被真实清理失败阻挡，不再列「低优先偶发」；业务 PASS 不是整路通过。
- **证据/触发**：最新 21/22 与上述失败标志；失败瞬间锁持有者未知，事后归属清理残留 0、无持续锁/项目进程仅说明事后状态。
  - 仍留存 **25 文件、30,969,999 字节**，见 [失败和身份/留存证据](<docs/lasttesting.md#最终排版后复验2026-11-25历史记录>)。
  - 2026-11-25 晚复验再次复现：留存 `%TEMP%/oi-regression-p0-f5oql9zw`，25 文件、30,978,331 字节；事后同样无进程、可独占打开，瞬间占用者仍未取到。同一位置连续两轮失败，排除偶发，应作为下一项先做。
- **最小方向**：在再次失败瞬间抓删除时间点、exe/句柄与进程归属、PID 创建时间、清理顺序，先定位再修；不猜杀毒，不加无限 retry、不吞异常、不以重跑刷绿关闭。
- **验收**：原失败链证据可解释且对应修复有受控验证；保留业务结果和目录清理结果分别记账、退出码/矩阵状态正确，无未说明的留存。清理开始前先读走归属和步骤；Windows 上删除遇到 WinError 5 或 32 时，把时间、路径、Restart Manager 占用进程（含 PID 创建时间）、归属进程状态和清理步骤打到 stderr，并写到临时目录旁边的 `*.delete-denial.json`。异常照旧抛出，没有重试循环。受控负例是本进程独占打开的文件：快照里有本进程 PID 和创建时间，删除仍然失败。2026-11-25 22:36 本机 Windows 多线程只跑了一次 registry，退出码 0。23:23–23:29 再跑一次本机 Windows 多线程 `--case all`，22/22、退出码 0，`registry PASS seconds= 0.12` 之后清理没有中断。两次都没有 `delete denial`，也没有留下新的临时目录。这两次没复现；此前整路 21/22 的清理失败仍然算数，不能写成已定位或已修。2026-11-26 四路矩阵的 Windows MT 也是 22/22，同样没有 `delete denial`。

## 3. timer 公有入口未 Init 除零

- **价值**：确定的公开接口前置调用除零，修复面小，不必重做时间轮。
- **证据/触发**：[构造 offset 为 0](<cbaselib/timer/src/oi_timer_manager.cpp#L16-L21>)，[Update](<cbaselib/timer/src/oi_timer_manager.cpp#L137-L153>) / [milling2tick](<cbaselib/timer/inc/oi_timer_manager.h#L107-L113>)直接除；外部 Init 成功前调用即触发。
  - 正常 [框架顺序先 Init](<cbaselib/base/src/oi_app.cpp#L26-L46>)不触发，[AddTimer 已有保护](<cbaselib/timer/src/oi_timer_manager.cpp#L196-L215>)，不是正常启动必崩。
- **最小方向**：分别定义两个入口未初始化时返回/错误语义，最小守卫后再换算，不改正常 tick/级联算法。
- **验收**：两个入口各自 Init 前直接调用探针不崩且符合约定，Init 后正常换算与定时派发不变；现 AddTimer-before-init 断言不能替代这两个入口的覆盖。
- **本轮结果**：两个入口已加未初始化守卫；Windows 与远程 Linux 的单线程、多线程 `hold-timer` 四路定向用例通过。Linux 两路均输出 `hold timer uninitialized update=1 convert=1 tick=0 ticks=0 unchanged=1 bad=0`、`hold timer conversion offset=50 bad=0` 和 `hold timer boundaries phase=3 calls=18 bad=0`，分别耗时 5.103 秒、5.407 秒，测试前后清理成功。旧版两个入口在 Windows MT 分别触发整数除零（客户端退出 0xC0000094）。这是定向验收，不替代完整矩阵。

## 4. 日志失败交付语义与证据

- **价值**：让业务知道日志究竟被接纳、拒绝还是允许丢弃，并能从已有返回码/落盘证据定位；先定合同，不默认改变发送策略。
- **当前证据**：[WriteLog](<cbaselib/logd/src/log_manager.cpp#L170-L199>)为 **FIFO 公平性**，182 行已明确新网络帧/本地日志不能越过等待者，不能再写「满时注释与实现不符」。本地 `pSession=null` 遇 BUSY，若调用方忽略错误，文件可能未留存。
- **证据边界**：[日志入口先 fwrite stdout](<cbaselib/logger/src/oi_log.cpp#L108-L116>)，所以不是所有日志无声消失。
  - [已连接发送失败无回退](<cbaselib/proto/src/oi_rpc_session_manager.cpp#L195-L222>)与 [LogClient 返回码](<cbaselib/logger/src/oi_log_client.cpp#L31-L45>)使忽略错误的远端日志也可能未留存，不能把「未接纳」与「已接纳后丢失」混为一谈。
- **最小方向**：明确接受丢弃/拒绝与有限重试合同，核对调用方处理和已有诊断；不擅改 [既定直连断线不暂存策略](<docs/fixed.md#2026-11-21-已定策略>)，不无限缓存，不无端新增大型诊断系统。
- **若合同要求补发**：仅对确定未接纳帧有界排队，防重复；必须处理健康连接 BUSY 后恢复排空。[现 pending 只在 OnAddSession 排](<cbaselib/proto/src/oi_rpc_session_manager.cpp#L95-L113>)，不能光 push pending 就称修好。
- **验收**：真实背压/断连，分别核对调用返回码、stdout 与文件/远端实际落盘，验证容量、FIFO、恢复排空和无重复，不能只看 enqueue_rate 或后续日志恢复。
- **本轮定向结果**：Windows 与远程 Linux 的单线程、多线程 `log-contract` 四路通过。真实停止日志服并观察断线后，填满有界待发队列、恢复日志服，检查接纳与 BUSY 返回、stdout、停服后的真实文件顺序、拒绝项不落盘和最终项不重复。Linux 两路均输出 `log contract accepted=57 busy=1 stdout=all disk=fifo final=1 PASS`，定向耗时分别 2.768 秒、2.927 秒，前后清理成功。该场景在观察到断线后发送，**尚未覆盖活动会话发送与断连交错竞态**，不能由此推断该竞态的交付结果；本轮未跑完整矩阵。

## 5. UDP EAGAIN 写关注：先可控拥塞验证，再最小退避

- **价值**：排除发送背压时可能的 poll 空转，同时不破坏 UDP 队列与恢复唤醒。
- **证据/触发**：[Flush/DataOut](<cbaselib/net/src/oi_udp_channel.cpp#L181-L226>)静态确认 EAGAIN 后立刻重开写关注；[原 review](<docs/review-20261125.md#L43-L45>)明示是否自旋依赖平台 POLLOUT。
  - 旧 todo「Windows 真实负载 100% CPU」**无实测依据**，不当已实测结论。
- **最小方向**：先用真实、可控发送拥塞观察 poll 回调频率/CPU及之后可写恢复；证实问题后短 timer/本轮闸门做最小退避，不改成大调度重构。
- **验收**：拥塞期间无失控回调/CPU，恢复后继续发送，队首不丢、不重复，FIFO 与唤醒不丢；区分静态风险和实测结果并记录平台条件。

## 7. 配置路径从进程参数读取

- **需求**：每个进程都能用启动参数指定配置文件，不写死路径。
- **现状**：只有 [ipcserver 读第一个参数](<modules/ipcserver/src/main.cpp#L10-L15>)；ipcclient、webserver、webclient、redisserver 都写死 `../config/<进程>.toml` 传给 [StartFramework](<cbaselib/base/src/oi_app.cpp#L13-L19>)。ipcclient 第一个参数已被 `--thread-mode`、`--test-hooks` 等探针占用。
- **方向**：统一一种参数写法（例如 `--config <路径>`），缺省仍用原路径，放进框架公共入口，不在每个 main 各写一份；和现有探针参数不冲突。参数名和是否放进 `StartFramework` 待定。
- **验收**：每个进程分别用缺省路径和指定路径启动；指定不存在的文件时启动失败并报出路径。回归脚本复制配置的方式不变也能通过。

## 8. 日志文件改名：`.log.txt` 后缀、带时间、放同一个目录

- **需求**：日志文件后缀改成 `.log.txt`，所有日志文件名带时间，全部放在一个目录里。
- **现状**：logd 按天建子目录 `根目录/yy_mm_dd/`（[UpdateLogPath](<cbaselib/logd/src/log_manager.cpp#L161-L168>)），每个级别一个文件，文件名只有级别、没有后缀（[GetLogLvName](<cbaselib/logd/src/log_manager.cpp#L124-L143>) 的 `trace`、`debug`、`info`、`warning`、`error`）；跨天 [OnCrossDay](<cbaselib/logd/src/log_manager.cpp#L106-L122>) 换目录轮转。
- **方向**：根目录下直接放 `<级别>_<时间>.log.txt` 这类文件，跨天轮转改为换文件名而不是换目录。时间精度（只到日期，还是带启动时刻）、是否带进程名、「所有日志」是否包含 stdout 重定向和测试脚本产出的日志，都待定。
- **验收**：跨天轮转后新旧文件都在同一目录、名字带对应时间、内容不丢不重；回归脚本和文档里按旧目录/文件名找日志的地方一并改。

## 9. Linux POLLERR 停读与零窗口关闭不回收（进行中）

- **状态**：修复已落代码，四路矩阵通过；Linux 两路真实套接字专项和回收证据见最近测试报告。
- **价值**：两条都会让连接、缓冲和 fd 一直占着，业务侧只看到超时或资源慢慢涨；断言补强让这类回归在旧行为下真能失败。
- **证据/触发**（代码证据，主对话已核对，未实测），详见 [11-26 review §1](<docs/review-20261126.md#1-p2本轮修复中>)：
  - POLLERR：libuv [遇 POLLERR 自停并只回调一次](<cbaselib/3rd/libuv/src/unix/poll.c#L47-L52>)，[DataIn 额度满或单轮 4 MiB 提前返回](<cbaselib/net/src/oi_tcp_channel.cpp#L19-L32>)，旧 `SetEvents` 掩码不变就返回。对端灌入超过单轮的未读数据后 RST，只收不发的连接永不关闭。只影响 Linux。
  - 零窗口：[AllowSend](<cbaselib/net/src/oi_channel.cpp#L18-L31>) 对已关闭会话只开写关注；写关注早已打开、没有读写事件时 `TryClose` 永不执行，通道、最多 64 MiB 未发数据和 fd 一直占着。
  - 断言偏弱：收包超限只判非 0、延迟 `CallMany` 不查条数、BLPOP 超时与回包未断言互斥、HTTP 不复用依赖客户端自己的 `Connection` 头、事务回滚分不清插入后回滚和 BEGIN 失败、db 负例不核对 1062 / 1054。
- **最小方向**：`OnPoll` 出错后 handle 不活跃且读关注开着时按原掩码重开，`SetEvents` 掩码相同但 handle 已停时也重新 start；`SetEvents` 对已关闭会话挂 10 秒一次性定时器兜底 `TryClose`（新增 `Channel::SessionClosed()`）。断言按上面六处逐条收紧，不放宽超时。
- **验收**：两条 P2 各有真实套接字回归——POLLERR 用对端灌数据后 RST，零窗口用对端不读、不发——在旧行为下失败（连接不关、fd 不释放），修复后通过，并用可观察事件确认会话已回收；补强后的断言要能被对应的错误结果打失败。之后按 [测试规程](<docs/testing.md>) 跑 Windows / Linux、单线程 / 多线程四路矩阵通过。
