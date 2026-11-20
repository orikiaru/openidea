# 待办

## 近期审查：优先处理的正确性问题

以下是两轮审查中确认的代码路径；除明确说明外，结论来自代码核对，不代表已构造故障实测。每项完成时沿调用链核对调用方得到的结果和资源归属，并按测试规程验证。

- **TCP 监听后登记失败未回滚**（`cbaselib/net/src/oi_session_manager.cpp`、`oi_accept_channel.cpp`）：监听成功后 `Publish` 同步失败时，调用方收到错误，监听却仍占端口并接收连接，重试可能绑定失败。由管理器持有监听并在该失败路径关闭，验证错误返回、端口释放及重试。
- **TLS 密文入队 BUSY 被记成永久错误**（`cbaselib/net/src/oi_tls_session.cpp`）：SSL 已消费明文并产生密文后，提交密文返回 `OI_ERR_BUSY` 会写永久错误串，后续传输被判连接失败且 BIO 内数据无重试路径；发送前预检直接返回 BUSY 不触发这一情况。区分可重试背压和永久故障，保留密文并在可写时重试，实测原请求结果及额度恢复。
- **RPC 超时扫描异常中断同批完成**（`cbaselib/proto/src/oi_rpc_call.cpp` 的 `Sweep`/`FinishSlot`）：扫描先摘出多条到期槽，逐个完成时任一回调抛异常，会跳过后续已摘槽的完成和唤醒；后续扫描和断连也找不到这些槽。逐槽隔离异常并验证同批所有等待者及额度收尾。
- **RPC 收包异常可丢失同批后续数据**（`cbaselib/proto/src/oi_rpc_session.cpp`、`oi_rpc_call.cpp`）：接收缓冲先被换到局部批次，`Deliver` 只处理 `ParserException`；若其他异常逃出，尚未换回的批次及后续字节随栈销毁。标准 handler 和解码已有捕获，不能据此断言事件循环一定继续或具体业务必然触发；应保证异常收尾并关闭受影响连接，验证调用方结果和同批数据处理。
- **连接补发异常跳过待发项和重试**（`cbaselib/proto/src/oi_rpc_peer.cpp` 的 `PeerConnection::Finish`）：建连完成后先移走整批暂存请求，某项发送或唤醒抛异常会跳过后续请求与 `ScheduleRetry`。被跳过的无期限调用若无其他完成路径会一直等待。逐项收尾并保证重试决策执行，故障注入检查原请求及剩余项。
- **注册中心断线与整量重载交错**（`cbaselib/registry/src/registry_server.cpp`）：断线先删快照，已在途的旧 `Reload` 仍可把 Redis 中旧记录换回；撤销成功后才二次删除，期间 Pull 可读到断线地址，撤销未成功时可能留到后续重载。处理快照代次或撤销状态时须保留多来源数据，不能仅按本地连接过滤全局名单；构造真实交错检查 Pull、撤销失败和最终状态。
- **注册中心地址文本分隔未校验**（同文件）：登记时将 ip/port 与其他字段以空格编码，但地址含空格未被拒绝；后续解析可能拆错端口或分区，或丢失该实例。写入前校验并核对编解码往返，验证错误返回和原快照不变。
- **logd 跨日批次归档可能错日**（`cbaselib/logd/src/log_file_instance.cpp`）：写线程取批后选择活动文件，批次可能同时含跨日前后日志，却被统一写入单个日期目录；行标题仅含时分秒。应按日志时间或明确的轮转边界分派，不能简单地将整批一律写旧文件或新文件；跨日交错验证实际落盘。
- **logd 跨日文件操作阻塞 poll**（`cbaselib/logd/src/log_manager.cpp`、`log_file_instance.cpp`）：时间轮回调同步建目录和打开文件，磁盘卡住时 poll 停转、背压恢复通知也不能消费；操作失败会 `abort`。将阻塞文件操作移出 poll 并保留失败原因，实测慢磁盘及失败收尾。

## 后续处理：HTTP、生成器与测试工具

- **服务端流在整个超时周期没有进度时被当残包关闭**（`cbaselib/http/src/oi_http_server.cpp`）：source/sink 持续返回 BUSY 或持续无产出/消费达半帧超时时限后连接关闭；持续缓慢但有进度不属于此触发条件。确认空闲策略并与真正未完成帧的超时分开，覆盖长时间无进度和持续慢进度。
- **HTTP 服务端缺少连接空闲回收**（同文件及 `modules/webserver`）：完整请求/响应之后的 keep-alive 空闲连接，以及连接后始终不发首字节的客户端，不触发半帧计时，可长期占用 fd。按需求实现独立空闲策略并覆盖两种状态。
- **客户端缓冲式 chunked trailer 缺独立上限**（`cbaselib/http/src/oi_http.cpp`）：非流式读取 0 块之后的 trailer 时未单独执行 64 KiB 头部上限，受整体缓冲预算约束，可能接收远超 64 KiB 的 trailer；sink 路径已有该限额。累计 trailer 大小并验证边界和超限失败。
- **客户端跳过无冒号响应头**（同文件）：畸形响应头行被跳过，其余响应仍可能按成功解析；应明确拒绝并验证调用方错误和连接后续归属。
- **合法 HTTP body 与接收缓冲容量耦合**（`cbaselib/http/src/oi_http_server.cpp`、`cbaselib/net/src/oi_tcp_channel.cpp`）：普通缓冲路径等待 body 收齐后消费；合法 body 大于 `max_receive_buffer_bytes` 时可能先因接收缓冲不足断连，而不是按该 body 的合法性处理。超过 body 上限的请求已在正文读满前按长度或 chunk 大小拒绝并回 413，不把它与合法正文受缓冲限制混为一项。考虑增量消费或明确配置约束，分别实测合法受限与超限 413。
- **生成器字段和声明输入校验不足**（`cbaselib/proto/proto_tool/field_naming.py`、`serialize_data_file_gen.py`）：自由类型 `long` 会原样进入生成代码，宽度跨平台不一致（Linux x64 8 字节、Windows x64 4 字节），跨平台协议不兼容；具体编码路径仍待复现——`proto_tool` 里没有按 `sizeof(T)` 编码的实现，更可能在生成或编译阶段就失败，修前先按实际生成结果确认。历史负例还包括声明名和枚举值越界。收紧允许类型和声明范围，保留有效协议兼容；`size_t` 不能笼统称为 Windows x64 的 4 字节。
- **限定名 Bytes 被重复替换**（`serialize_data_file_gen.py`）：已写 `OI::Base::Bytes` 时词语替换仍命中 `Bytes`，生成重复命名空间而无法编译。按完整类型名处理并覆盖裸名和已限定名。
- **component 声明未填生成表**（`module_data_parse.py`）：文本入口没有调用组件解析，带 component 的输入以 `NoComponent` 失败；现有不含组件的模块不暴露该问题。接入解析或同步移除不支持的入口和文档。
- **回归清理可能漏报服务崩溃**（`tools/regression/procs.py` 的 `cleanup_owned`）：用例完成后、清理前服务已非零退出，现有清理忽略退出码，可能误报通过；应逐进程核对，并保留 `disk-full` 等明确预期非零退出的例外。
- **单独构建的 webclient 线程模式不一致**（`tools/build.py`）：`--mode mt` 关闭 IPC 单线程但未同步 `OI_WEBCLIENT_SINGLE_THREAD`，可能产出单线程 webclient；矩阵自身已设置。按构建模式同步配置并验证。
- **双平台矩阵丢失 Windows 自定义参数**（`tools/test_matrix.py`）：`--platform both --define` 仅传入 Linux 子命令，Windows 构建未取得相同配置。两边同时传递并检查最终配置。
- **并行矩阵中途失败可能遗留子进程**（`tools/parallel_matrix.py`）：启动和等待之间无完整 `try/finally`，后一路启动失败或中断时，已启动的 lane 可能继续占用端口或目录。仅终止和回收本次持有的子进程，并测试失败和中断收尾。
- **反向 Hold 分支缺业务覆盖**（`modules/ipcserver/src/ipcserver.cpp`）。特定标记会启动独立的反向调用，原请求继续按自己的处理结果回包。五秒是反向调用的超时上限。现有用例未触发该标记，也未断言反向回复。若保留分支，需让客户端处理反向调用，分别检查两个方向的结果。

## 待定策略

- **UDP 长期不可写时的空闲回收策略**（`cbaselib/net/src/oi_udp_channel.cpp`）：持续 `EAGAIN` 超过 `idle_timeout_ms` 时，对端待发队列可能非空但会话被回收，已入队包被释放；`OI_OK` 仅承诺入队，不承诺最终送达，因此不能直接定性为协议违约。若改为队列非空永不回收，又可能使持续不可写的会话常驻；需明确丢弃、期限与容量策略后再验证。
- **重复的 Content-Length**。客户端接受数值相同的重复长度，拒绝冲突长度。是否采用与服务端相同的严格策略留作协议兼容性取舍。

## 尚未证实的疑点

- **UDP ICMP 与 poll 停止疑点**。未确认监听口因 ICMP 停转。需同时观察绑定口收到的 poll 状态、读监听是否仍有效，以及后续合法数据报能否收到。
- **旧 Windows 单线程 admission 崩溃**：两项用例旧增量产物曾复现 `0xC0000005`，带符号对照与最终矩阵通过，但原崩溃无匹配 PDB、dump 或二进制哈希，根因及与现版本关系未确认；证据和复测结果见 [故障记录](windows-st-admission-20261121.md)。

## 唤醒路径延迟拆分

分别实测 submit→worker、worker done→post、post→poll、poll→业务恢复，并与同机双线程竞争互斥锁交接的相同边界对照。空任务约 10.69 微秒是整链耗时，不能据此归因于 libuv 或断言已证明比锁慢。

## Redis 迟到回包偶发问题：未确认，等待复现

本轮静态核对发现超时项仍留队认领回包、断连按会话清理，未确认旧回复错配。现有 Hold 覆盖 BLPOP 超时、迟到回包、后续 PING 及超时边界竞争；偶发问题尚无确认根因，复测通过本身不能证明问题消失。

- 复现时按 `docs/testing.md` 的单项复现和清理规程运行 `--case hold-redis`，指定 `--artifacts` 与 `--redis-timeline`，保留结果、各进程输出及 Redis 时间线。记录平台、线程模式、构建版本、Redis 程序、端口偏移、失败轮次、退出码、客户端超时和耗时、PING 的状态和回包类型，以及前后清理结果。
- 对照时间线中的 PING、测试 BLPOP 执行时间和上游连接地址；MONITOR 只能说明命令执行，不能证明回包到达。若需判断丢包或错配，还要采集代理和客户端收发时间，避免记录完整请求和敏感值。
- 关闭条件：复现并确认根因，修复后用对应场景验证且完成规定的平台和线程模式回归；若无法复现，保留观察，不以一次通过关闭。

## Windows 测试进程清理：低优先级观察

Windows 多线程注册发现用例完成了业务检查，删除临时目录内的 ipcserver.exe 时遇到 WinError 5；复测连同清理通过。再出现时保留完整路径、进程退出时间、清理报错及归属核验结果，用来区分进程仍在退出和文件被其他程序占用。

## 定时极端溢出

暂缓，待有明确复现或实际需求后再分析。


## HTTP 消息能力后续分期

六项能力（Cookie、重复 query、URL 编码表单、multipart、流式收发、客户端 HTTPS）均已完成，验收项移到 [已完成记录](fixed.md)。服务端 TLS 仍由反向代理终止；libhv 的文件/JSON/类型化值接口是否引入按实际需求另行登记。

## 2026-11-19 全仓审查

P2 六项与 P3 历史记录均已修复或核对完成，记录移到 [已完成记录](fixed.md)；仍有效的活动项已并入上面的优先级清单，不再重复维护。

## 2026-11-21 分诊结论与排期

本轮已处理，记录见 [已完成记录](fixed.md)：工具类四项（清理校验退出码、`build.py` 的 webclient 开关、`test_matrix --platform both` 的 `--define`、`parallel_matrix` 的 `try/finally`）、`StartServer` 的 `Publish` 失败回滚、反向 Hold 的客户端接收与双向断言；生成器 `long` 条目的机理描述也按实际代码改写（未改生成器行为）。

仍按原顺序排期：

1. **产品类补触发**：`StartServer` 回滚需要构造一次真实的 `Publish` 失败（registry 登记返回错误）才能验证端口释放与重试。
2. **要故障注入或构造交错**：RPC `Sweep` 的逐槽异常隔离与收包异常收尾；TLS 密文入队 BUSY 的重试；HTTP 服务端流无进度超时、连接空闲回收、chunked trailer 上限、合法 body 与接收缓冲耦合；registry 断线与整量重载交错、地址文本空格校验；logd 跨日批次归档、跨日文件操作阻塞 poll。
3. **生成器输入校验**：`long` 等自由类型先按实际生成路径复现（`proto_tool` 没有 `sizeof` 编码路径），再收紧允许类型；`Bytes` 二次限定和 component 文本入口已按代码确认为真，随该模块改动一并修。
