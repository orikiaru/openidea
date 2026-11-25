# 待办

## 2026-11-25 全仓 review 新增待修项

- 分诊：静态审查 master `793b572`，8 路只读审查；**未发现新的可稳定触发 P1**。逐条证据、触发条件、置信度和已核对项见 [2026-11-25 全仓 review](docs/review-20261125.md)。本轮只记录，未改代码。
- 验证：未编译、未跑四路回归。修完按 `docs/testing.md` 跑四路 `--case all`（Windows/WSL Linux × 单线程/多线程），并补对应定向用例。

### 先做（5 条）

1. **libuv C 回调链统一异常边界**（系统性）。现状：`Poll::OnPoll`（`cbaselib/net/src/oi_poll.cpp:84-105`）、`Session::SwapInBuff`（`oi_session.cpp:88-106`）、`Poll::State::Drain`（`oi_poll.cpp:279-324`）直接调用，无 try；定时器路径（`oi_timer_manager.cpp:164-176`）已有保护可对照。可达点位：logd `WriteLog/DeferWrite`（`log_server_session.cpp:41-73` 的 try 只包 deserialize）、HTTP `Produce/OnPlaintext/RequestFrame::Read`（`oi_http_stream.h:295`、`oi_http.cpp:717,891`、`oi_http_server.cpp:280,324`）、redisserver `OnConnect/OnDisconnect`（`redis_server.cpp:238-272`）。后果：OOM 时 `bad_alloc` 穿 C 帧 → UB/terminate。修法：在 `OnPoll`/`Drain` 外层统一 `try/catch(...)` 转 `PassiveClose`/`ReportTaskError`。
2. **ServiceTable 观察者裸指针无注销**。位置：`cbaselib/base/src/oi_registry.cpp:88-98,246-253`、`oi_registry.h:76,80`；栈上观察者 `modules/ipcclient/src/blocking_mgr.cpp:311`。后果：被观察服务实例变化时回调已析构对象 → UAF（当前回归未触发）。修法：加 `Unwatch` 并在管理器析构时摘除，或改 `weak_ptr`/令牌。
3. **UDP EAGAIN 后立即重开写关注**。位置：`cbaselib/net/src/oi_udp_channel.cpp:181-226`。后果：UDP 常报可写的平台（Windows）发送缓冲满时 poll 线程 100% CPU。修法：EAGAIN 用短 timer 或本 tick 闸门退避。
4. **HTTP 客户端 chunked size 行完整时不限长**。位置：`cbaselib/http/src/oi_http.cpp:657-672`；服务端有 1024 上限（`oi_http_server.cpp:302-306`）。后果：超长全 `0` 行被当终止块，body 提前结束/状态错位。修法：对完整行同样校验长度。
5. **logd 本地日志丢 + 日志客户端已连接发送失败不回退入队**。位置：`log_manager.cpp:181-191`（注释「满时」与实际「有等待者即丢」不符）、`oi_rpc_session_manager.cpp:195-223`、`oi_log_client.cpp:45`。后果：诊断日志与断线/背压瞬间的日志静默丢失。

### 观察/暂不修（P2/P3，保留证据）

- **框架**：redisserver 把 `*-1` 改写成 `$-1`（`redis_server.cpp:179-181`）；poll 注册失败先关 fd 后 uv_close（`oi_poll.cpp:765-777`，需注入验证）；`sid_t` int32 回绕（`oi_header.h:43-44`、`oi_session_manager.cpp:672,748`）；发送上限只校验到 SIZE_MAX（`oi_session_manager.cpp:26-45`）；BlockingWorkChannel 控制管道破裂 EOF 空转（`oi_blocking_work_channel.cpp:158-191`）；协程 `await_suspend` noexcept 里堆分配（`oi_coro.h:197-238`）。
- **测试有效性**：wake_bench 求和断言恒真（`wake_bench.cpp:96-100,242-248`）；web IPv6 环境阻塞只打印不生效（`web.py:994-1001`）；hold 死配置 `max_clients/idle_ms`（`hold.py:92-96`）；`test_matrix --mode st --asan` 用错目录、Windows 静默忽略（`test_matrix.py:323-324`）；clock 注入不校验生效（`hold.py:189-195`）；`--platform both` 覆盖 Windows hook 标记（`CMakeLists.txt:71`）；`buckets=0` 断言（`hold.py:239`）；`run_web` 断言丢 stdout（`web.py:993`）。
- **不计为缺陷**：BlockingMgr 与 DbMgr 争抢 MySQL 阻塞池（需同时开 `Test.hold`+`Test.db`，规程禁止）；`Bytes::Overlaps` 跨对象指针比较（技术性 UB，无实务影响）。

## 2026-11-24 全仓 review 待修项

- 分诊：静态审查 master `5d6cf82`，10 路分模块；上轮 3 条先做项仍未修，本轮新增 2 个 P2（批量 RPC 回滚挂起、容器解码内存放大）、2 个测试有效性 P2（Windows HTTP 释放检查恒通过、Linux hook 标记路径不一致），其余为 P3。逐条证据、触发链、已核对项和未覆盖项见 [2026-11-24 全仓 review](docs/review-20261124.md)。
- 验证：本轮未编译、未跑四路回归；机械检查中继承检查通过（272 个自有文件），字段命名和格式检查因环境阻塞。修完按 `docs/testing.md` 跑四路 `--case all`（Windows/WSL Linux × 单线程/多线程），并补本条要求的定向用例。
- 上轮 2026-11-21 的 3 条先做项已并入本节跟踪；旧节保留历史证据。

### 先做（7 条）

1. **logd 跨日打开失败后的策略与重试**（上轮未修，先拍板）。位置：`cbaselib/logd/src/log_file_instance.cpp:220-239`、`cbaselib/logd/inc/log_file_instance.h:15-32`。现状：`OpenLogFile` 失败只 `fprintf` 后 `continue`，轮转条目随 `vWriting.clear()` 丢弃；`LogFileInstance` 没有 pending-rotate 成员，`Rotate` 只在 `LogManager::OnCrossDay` 每天零点调用一次（`cbaselib/logd/src/log_manager.cpp:106-122`）。触发：零点建目录/打开失败（磁盘满、权限、瞬时 IO），之后恢复。后果：当天五个级别的日志继续写旧日期文件，按当天目录找不到；调用方仍是 `OI_OK`，无错误码/计数。注意：`docs/fixed.md:22` 已把「失败继续写当前文件、不退出」记为已修，但 11-21 todo 要求的「保留 pending、后续每批重试」仍未做；先确认是否还要求重试。修法：失败时把路径存成员（受 `m_stCacheLocker`），每批写完先重试打开，成功再清并留限频诊断。验收：Linux 注入次日路径为普通文件，首次失败后恢复，断言当天目录出现文件、失败期间日志不丢、stderr 有原因；若决定不重试，把策略写进文档并从 todo 删除。
2. **`coro` 回归断言空转**（上轮未修）。位置：`modules/ipcclient/src/coro_mgr.cpp:219-228`、`tools/regression/cases/coro.py:14-20`。现状：日志行把 `bad=0` 写成字面量（224 行），结束时直接 `RequestStop(0)`（228 行），没有 `TestBoard`；脚本只断言出现 `"coro live="` 和字面 `"bad=0"`，不解析 `live/churn/live_ns/wake_ns`。触发：`--case coro`。后果：协程数量、唤醒计数或基准口径退化到 0 时仍 exit 0、脚本仍 PASS，月度基准不可信。修法：应用输出真实 `bad`（`HeldCount`/`WakeDone`/`ChurnDone` 与期望不符即非 0）；脚本解析 `live/churn/live_ns/wake_ns` 并断言 `>0`；`coro` 走 `TestBoard::Arm/Done` 或明确的非 0 退出。验收：用一次 `live` 或 `wake` 少 1 的旧实现确认脚本会失败，再验证修复后通过。
3. **configure 改写已跟踪的 `.clangd`**（上轮未修）。位置：`framework.cmake:171-206`（201 行写 `${CMAKE_SOURCE_DIR}/.clangd`，205 行还会写框架目录）。现状：`.clangd` 是跟踪文件；Ninja/Makefiles 构建把 `CompilationDatabase` 写成构建目录相对路径并丢掉 `Compiler: clang-cl`，配置一次工作区就脏。修法：只写构建目录，或内容一致时保留；至少加入 `.gitignore` 并在交付说明里写清。验收：Linux/Ninja 与 Windows VS 各 configure 一次，`git status` 干净，clangd 仍能读到编译数据库。
4. **批量 RPC 构建失败后的回滚永久挂起**（本轮新增）。位置：`cbaselib/proto/inc/oi_rpc_coro.h:825-841`（回滚）、`cbaselib/proto/inc/oi_rpc_coro.h:378-398`（`Dispatch` 先置 `bDispatched` 再 `Bind`）、`cbaselib/proto/inc/oi_rpc_coro.h:817-819`（构造并 `Submit`）。触发：`CallAll/CallMany/Notify*` 构造或 `Submit` 时 `make_shared`/`Bind`/请求拷贝抛 `bad_alloc`，`Send` 记下 `stBuildError`。后果：回滚只判断 `bDispatched`，于是 `co_await` 一个 `iCallId == 0`、gate 仍为 `Idle`、从未登记到 `RpcCallManager` 的等待器；`Arm` 永久挂起，`std::rethrow_exception` 到不了，协程帧、请求副本和已提交结果泄漏。修法：回滚条件加 `!stCall->bDispatched || stCall->iCallId == 0` 跳过；或 `Dispatch` 失败时先把 gate 置 `Ready`、写 `stSubmissionError` 再抛。验收：加一次可注入的分配失败，`CallAll/CallMany` 在超时内以原异常返回，`WaitStats` 和帧数回基线；旧实现应表现为挂起/超时。
5. **容器解码内存放大**（本轮新增）。位置：`cbaselib/base/inc/oi_bytes_stream.h:243-264`（vector）、`280-304`（map）；现有协议 `modules/include/oi_rpcdata_alltypesdata.h:22-24` 已有 `vector<Inner>`、`map<string, Inner>`。触发：对端声明大 `count`、每元素只放 4 字节空 body；校验只有 `iCount > RemainSize()/4`（map 为 8），未约束本地 `sizeof(T)`。后果：16MB 字段可展开出数百 MB 的 `Inner`，64MB 帧可到 GB 级；对端可控，小内存机被 OOM killer 杀或长时间停顿。修法：解码加累计分配预算（字段长度 + 元素数 + 嵌套深度），或按 `iCount * sizeof(T)` 设上限分段拒绝；不要用 `sizeof(T)` 做线格式下界，会误杀合法的空元素数组。验收：构造 16MB 字段、`count = RemainSize()/4`、空 body 的包，断言在预算内失败且 RSS 有界；100 万个 `int32_t` 的合法数组仍能解；旧实现对照应吃内存。
6. **Windows HTTP 大响应释放检查恒通过**（本轮新增）。位置：`modules/ipcclient/src/http_mgr.cpp:104-116`、`modules/ipcclient/src/http_mgr.cpp:118-139`（判定 133 行）。触发：Windows 单/多线程 `hold` 的 `hold http release before=... after=...`。现状：非 glibc 分支 `AllocatedBytes()` 直接 `return 0`，`iBefore == iAfter == 0`，`iAfter > iBefore + 8MB` 永不成立；32×1MB 响应留在堆上也报 `bad=0`。后果：Windows 两路对该性质没有覆盖，报告却显示通过。修法：用 `GetProcessMemoryInfo` 的 `PrivateUsage`/工作集，或让 harness 明确标记该检查在 Windows 未覆盖、不能算通过。验收：Windows 上 `before/after` 非 0，旧实现保留 32MB 能让 `bad=1`；或脚本输出 `skipped` 且四路汇总记为未覆盖。
7. **Linux 手动 hook 构建下 blocking-wake 断言静默跳过**（本轮新增）。位置：`CMakeLists.txt:71`、`tools/regression/cases/db.py:27-32`、`tools/test_matrix.py:292-294`。触发：Linux 以 `-DOI_ENABLE_TEST_HOOKS=ON` 构建后按 `docs/testing.md` 用 `--case blocking-wake --bin-dir deploy/bin` 运行。现状：标记写到 `${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/oi_test_hooks.txt`，Linux 设 `CMAKE_BUILD_TYPE=Release` 后落在 `deploy/bin/Release/`；`bin_dir` 是 `deploy/bin`，`stamp.exists()` 为假，`trace_submit_to_worker` 等 7 项断言整段跳过，测试仍 PASS。矩阵在 Linux 强制 hooks=OFF，所以不影响 `--case all`。修法：去掉 `$<CONFIG>` 子目录或让 runner 显式接收标记路径；请求 hook 而标记缺失时 fail。验收：Linux hooks=ON 跑 `--case blocking-wake --bin-dir deploy/bin` 必须出现 7 行 `wake benchmark trace_*`；删掉标记后同一命令必须失败。

### 先确认（改前拍板，不直接排期）

- **RPC 半关闭丢回包**（P1，已知）。FIN 到达即 `PassiveClose(0)` 并 `CloseSession` 摘会话，处理协程稍后 `SendFrame` 按 sid 查不到，回包丢失；仓库内客户端不发 FIN。二选一：明确不支持半关闭并写进文档/回归，或按 `ServerSession::PassiveClose` 延迟摘会话，补 `shutdown(SHUT_WR)` 后仍收到回包的用例。
- **`m_bLost` 只置不清**。被同名同 id 接管后永久停止登记；占用方退出、键过期也不恢复，只能重启。先定策略：允许恢复就定期 `RegPull` 判断空出后清零重登记，否则把状态显式暴露给应用。
- **Redis late ping 偶发超时**。`hold-redis` 的 BLPOP 后 PING 偶发在 2 秒预算内没回包；客户端 FIFO 认领与 `m_iSwept` 账目自洽，需按 `docs/todo.md` 抓一次失败时 redisserver/Redis 两侧时间线，再决定修客户端/redisserver 还是放宽用例余量。

### 观察/暂不修（P3，保留证据）

- **规则/机械项**：`rpc_pending_mgr` 三处默认捕获 `[&]`（`modules/ipcclient/src/rpc_pending_mgr.cpp:133-142,539-546,566-569`）；生成器 `module_file_gen.py:176` 与三个模块 `main.cpp` 的 `[&]`；两处 `||` 里串多个 `co_await`（`rpc_output_mgr.cpp:146`、`rpc_service_mgr.cpp:252`）；`--timer-before-init` 的 `[&bCalled]`；`PostCoroutine` 单线程空实现、`oi_lock.h` 两套接口不对称；`timer_lvs` 单层槽数无上限；`Bytes::Insert` 越界只 assert；Windows pthread 兼容层递归属性缺失。
- **错误路径**：HTTP 服务端缓冲响应超额度时静默断连（`oi_http_server.cpp:526-535`）；redisserver 大回包丢弃且 `AppendReply` 可能 bad_alloc 穿过 hiredis C 回调（`redis_server.cpp:147-197,206-235`）；HTTP trailer 每段重扫（`oi_http_server.cpp:333-344`）；HTTP 客户端流式源结束后 2ms 泵空转（`oi_http.cpp:469-501`）、请求超额度整包 BUSY（`1236-1241`）；UDP sendto 失败静默丢包、绑定口无释放（`udp_channel.cpp:190-196`）；`PickOne/ToDbMap` 的 `ToJsonText` 异常穿透（`oi_db_row.h:436-457`）；JSON `double` 接受 inf（`oi_json.h:148-152`）；registry 一条坏行放弃整表（`registry_server.cpp:538-560`）；logger 静默丢日志、`LogMgr` 持续失败死循环；`HoldMgr::Pump` 无兜底 try/catch；`PollUnregisterProbe` 二次 close fd；`CheckSyncOnLoop` 无超时。
- **测试覆盖**：`CheckRpcRace` 只累计 Bad、全超时也过；`CallAll` 部分失败、UDP 丢包、域名解析次数、webtest 响应源取消/释放均缺用例；`hold`/`udp` 线程模式断言接受任一模式；`kill_test_procs` CIM 缺创建时间时静默跳过、`registry` proxy close 覆盖原始异常、log-backpressure 探针复用 `logd.out`、`test_matrix` 只扫默认 VS 路径。
- **文档**：`docs/cbaselib.md:126` 的 `StartFramework` 返回码 -2/-1 错误；`docs/cbaselib/notify.md:66` 的 UDP `RpcNotify` 返回 `OI_ERR_FAIL` 与实现不符；`docs/testing.md:175` 与同文件 210 行的 hooks 恢复说法矛盾；`docs/cbaselib.md:16-25` 等行号锚点漂移；`project.md` 的 XML、`Protocol`/`Rpc` 基类、时序图 README 的字节序、`time.md` 的续期措辞过期。
- **已知项状态不变**：`res_id` BUSY 错配（不可达）、非法 `[Registry]` 配置 3 秒 TIMEOUT、旧会话迟到 `OnClientDisconnect`/`OnPeerDisconnect`、TLS `SendResult` 跨线程、停服 `uv_loop_close` EBUSY、`StopAccept` fd 未真关、`PauseEvents` 覆盖新增位、Redis 空队列回执丢弃、chunked 8MiB/413 RST、线程池 `threadcount` 无上限、继承/格式检查漏 `tools` 下 C++、`sync_linux_remote` 前导点；redisserver 64MiB 首参复制已不成立，可更新 todo。

## 2026-11-21 全仓 review 待修项

> 本节 3 条先做项截至 2026-11-24 仍未修，已并入上一节跟踪；本节保留原始证据。

- 分诊：原审查确认 6 个 P1、7 个 P2、10 个 P3。按“现实触发 × 后果 × 修复成本”重排，只留 6 条先做；其余移到「先确认」「观察/暂不修」，不删除，保留证据。
- 对象：master `3bc339b`，静态通读 cbaselib 自有代码、modules、tools；未编译、未跑四路回归。7 路 subagent 分模块审查，主对话逐条回读代码核对；审查期间 subagent 越权改动的 5 个文件已还原，工作区干净。
- 验收：修完按 `docs/testing.md` 跑四路 `--case all`（Windows/WSL Linux × 单线程/多线程）。以下先做项中，logd 轮转、coro 断言要补或扩回归，不能只靠现有用例。

### 先做（3 条）

1. **logd 跨日打开新文件失败后不再重试**。现状：`OpenLogFile` 失败只打印并 `continue`（`cbaselib/logd/src/log_file_instance.cpp:222-239`），轮转标记被消费，下一次投递要等下一个跨日。触发：零点建目录/打开失败（磁盘满、权限、临时 IO 错误），之后恢复。后果：整天日志落在旧日期目录，按当天目录找不到。方向：失败时保留 `m_sPendingRotate`，后续每批写入前重试打开。
2. **`coro` 回归断言空转**。现状：脚本只要求日志出现 `coro live=` 和字面量 `bad=0`（`tools/regression/cases/coro.py:14-19`），应用日志把 `bad=0` 写死（`modules/ipcclient/src/coro_mgr.cpp:224`）。触发：`--case coro`。后果：`live=0` 或协程出错仍 PASS，月度基准不可信。方向：解析 `live/churn/live_ns/wake_ns` 并断言大于 0；应用输出真实 bad 计数。
3. **CMake 配置改写已跟踪的 clangd 索引文件**。现状：`framework.cmake:176-186` 直接写源码根目录；Linux/Ninja 构建后与仓库里跟踪的 Windows 版本不同，工作区变脏。方向：写构建目录，或只在内容一致时保留；至少把该文件加入忽略。

### 先确认（改前需要拍板，不直接排期）

- **RPC 半关闭丢回包**。现状：FIN 到达即 `PassiveClose(0)`（`cbaselib/net/src/oi_tcp_channel.cpp:59-68`），`NetSession::PassiveClose` 无条件 `CloseSession`（`cbaselib/net/src/oi_net_session.cpp:25-28`），`CloseSession` 先 erase 会话（`cbaselib/net/src/oi_session_manager.cpp:769-774`）。触发：客户端发完请求后 `shutdown(SHUT_WR)`，处理协程稍后回包。仓库内客户端不发 FIN。先定：明确不支持半关闭并写进文档，还是补半关闭 RPC 回归再按 `ServerSession::PassiveClose` 延迟摘会话。
- **`res_id != id` 时 BUSY/通知回执用错协议号**。现状：BUSY 回执用请求协议号 `iId` 编码（`cbaselib/proto/src/oi_rpc_call.cpp:634-644`），客户端按 `RpcProtoId<Res>::iValue` 等待（`cbaselib/proto/inc/oi_rpc_coro.h:397-400`）。当前仓库没有协议使用不同 `res_id`，属于生成器“支持但未使用”的能力；等真有协议用 `res_id` 再修。
- **`m_bLost` 只置不清**。旧进程被接管后永久停止心跳/登记；占用方退出、键过期后也不会恢复，只能重启。代码注释写的是“停止登记”，属于有意策略还是恢复缺口需确认。
- **停服时挂起的 DNS 解析**。`uv_loop_close` 返回 EBUSY 后仍释放 loop（`cbaselib/net/src/oi_poll.cpp:399-405`），`uv_getaddrinfo` 没有取消/完成登记（`cbaselib/net/src/oi_socket.cpp:435-439`）。先确认能否在停服窗口复现崩溃，再决定是否登记为必修。

### 观察/暂不修（不占先做位，保留证据）

- **线程池 `threadcount` 无上限、时间轮 `timer_lvs` 乘积回绕**：需要故意写极端配置；加一行保护可以顺手做，不单独跟踪（`cbaselib/base/src/oi_thread.cpp:70-75、302-306`；`cbaselib/timer/src/oi_timer_manager.cpp:61-73`）。
- **redisserver 64 MiB 首参复制**：内部测试进程、可信客户端；改 `string_view` 很便宜，但不是现实故障（`modules/redisserver/src/redis_server_session.cpp:115-129`）。
- **HTTP chunked 线上字节计入 8 MiB、413 后 RST**：只有极小 chunk 贴上限或大正文上传才触发，平台相关（`cbaselib/http/src/oi_http.cpp:877-883`；`cbaselib/http/src/oi_http_server.cpp:665-671`）。
- **Redis 空队列收到完整回执时静默丢弃**：当前没有可达路径，等出现额外回执/多回执命令再按协议错误处理（`cbaselib/redis/src/oi_redis.cpp:583-590`）。
- **`PauseEvents` 恢复覆盖暂停期间新增事件位**：只有测试探针暂停 UDP 绑定口时触发（`cbaselib/net/src/oi_poll.cpp:705-717`）。
- **`StopAccept` 返回时监听 fd 尚未真正关闭**：只有 Publish 失败后同进程立刻重试才触发（`cbaselib/net/src/oi_accept_channel.cpp:129-136`）。
- **纯选路进程里 `[Registry]` 配置非法表现为 3 秒 TIMEOUT**：属于诊断性/错误码问题，非崩溃（`cbaselib/proto/src/oi_registry_client.cpp:173-179`；`cbaselib/proto/src/oi_rpc_route.cpp:394-406`）。
- **文档说矩阵自动恢复 test hooks、实际不恢复**：文档/工具一致性问题（`docs/testing.md:171`；`tools/test_matrix.py:327-328`）。
- **多继承检查漏掉 `tools` 下自有 C++**：目前两个文件恰好单继承，属检查器覆盖缺口（`tools/check_inheritance.py:36-45`）。
- **`sync_linux_remote --require-text` 吃掉前导点**：只影响隐藏路径的同步检查（`tools/sync_linux_remote.py:239`）。
- **数据库 `PickOne` 的编码异常穿透**：只在 `[dbjson]` 字段含不可表示值时触发（`cbaselib/db/inc/oi_db_row.h:437-456`）。
- **旧会话迟到的析构仍回调 `OnClientDisconnect`/`OnPeerDisconnect`**：业务层需自己比对 sid 才能免疫（`cbaselib/proto/src/oi_rpc_session_manager.cpp:143-159`）。
- **TLS `SendResult` 跨线程不安全**：当前仓库 HTTPS 客户端只在 poll 线程发送，未触发（`cbaselib/net/src/oi_tls_session.cpp:410-427`、290-315）。

### 已排除，不再作为缺陷跟踪

- `TaskScope::Start` 传左值 lambda 不会把引用放进协程帧：`Run` 形参按值推导为 `F = X`，帧内是拷贝；外层 lambda 的 `&fnFunction` 只在 `InvokeTask` 同步调用期间使用。
- HTTP 客户端拒绝 304 的重复 `Content-Length` 是 2026-11-21 已定策略（数值相同也拒绝），不是遗漏。
- `hold` 的 BLPOP 后 PING 超时未找到回执串位路径；超时项留在队列认领自己的回执、`m_iSwept` 账目自洽。剩余风险是回执永远不来时超时项永久占用 `max_pending` 名额，保留上面「hold 的 redis late ping 偶发超时」继续跟踪。

## hold 的 redis late ping 偶发超时

- 现状：`hold` 的 `CheckRedisLateReply` 先发 `BLPOP hold-late 3`，客户端 2 秒判超时，接着在同一条连接上发 PING。正常 PONG 排在 BLPOP 的 nil 后面，约 1 秒到（`late ping ok=1 wall_ms≈900~1100`）。linux-mt 上偶发 `late ping ok=0 status=1 reply_type=-1 wall_ms≈2010`，PONG 在 PING 自己的 2 秒超时内没回来，`hold done bad=1`。2026-11-11 单独重跑三次挂一次，无并行负载时也会挂；Windows 和 linux-st 没见过。
- 诊断：2026-11-11 在隔离的 Linux MT 路复跑四次均通过，迟到 PING 分别耗时 1040、910、844、849 毫秒。后三次通过 Redis MONITOR 观察到 BLPOP 和 PING 在同一上游连接，PING 在 BLPOP 后约 3.01～3.08 秒执行；这不能证明回复已送达客户端。未找到原失败日志，尚未抓到失败时的时间线，根因未确认。
- 方向：先抓一次失败时 redisserver 和 Redis 两边的时间线，确认 PONG 是晚到、被丢，还是被记到超时那条命令上。再决定是修 Redis 客户端/redisserver 的迟到回复处理，还是放宽用例的时间余量。

## Windows ST disk-full 清理通知 helper 偶发退出 120（2026-11-12）

- 现状：本轮两次 Windows ST 全量均为 20/21；disk-full 已成功检测 ENOSPC，但清理通知 helper 退出码为 120。增加 stdout/stderr 捕获和失败诊断后定点运行通过，模拟退出 120 仍会报错；根因尚未修复或确认。
- 证据：本地结果 `windows-st-latest` 和 `windows-st-disk-diagnostic`。
- 方向：保留现有证据，下次失败时捕获 stderr，核实控制台与标准流生命周期。禁止吞掉退出码 120 或放宽断言。

## C++ 实际命名 AST 扫描缺依赖（2026-11-12）

- 现状：缺少 Python clang/libclang，实际命名 AST 扫描受阻。命名规则自身 9 项测试通过，继承检查和格式检查通过，但不能代替实际扫描。
- 方向：持久安装所需依赖，使用现有 `tools/check_field_names.py` 根据编译数据库执行扫描，并区分存量问题。

## 底层框架少用嵌套模板，协议生成优先（2026-11-13）

- 需求：底层框架能不用多层嵌套模板的地方不用。协议生成是大头：每条 rpcdata 的每个字段都要做一次重载决议和模板实例化，容器和嵌套结构再往下套一层。协议越多，编译量越大。数据库字段表已改为同类型描述及普通循环。对外字节、JSON、列顺序、主键、`[dbtype]`、`[dbjson]`、NULL 和按成员取列的结果保持不变。
- 现状：生成器已经按字段写出直线调用，但每个调用都落到头文件模板上。`GetPbSerialize` 写出 `os.WritePb(序号, 字段)`，`GetPbDeserialize` 写出 `os.ReadPb`。定宽类型走 `enable_if<PbFixedTrait<T>>`，`vector<T>` 和 `map<K, V>` 再递归实例化元素类型；JSON 的整数用 `enable_if_t`，rpcdata 用 `decltype(FromJson)` 做 SFINAE，`vector` / `map` 同样递归。字段每多一层容器或嵌套结构，就多一套实例。数据库字段表使用 `Field<行>` 数组，编码解码回调接收整行，成员指针查找由生成的 `DbFindField` 单独处理。样例在 `modules/include/oi_rpcdata_dbsample.h` 的 `serialize`、`FromJson` 和 `DbFields`。对应实现在 `cbaselib/base/inc/oi_bytes_stream.h`、`cbaselib/proto/inc/oi_json.h`、`cbaselib/db/inc/oi_db_row.h`、`cbaselib/db/inc/oi_db_table.h`，生成在 `cbaselib/proto/proto_tool/serialize_data_file_gen.py`。
- 方向：生成器已经知道每个字段的具体类型，按标量、string、Bytes、vector、map、嵌套 rpcdata 直接写出读写，容器在生成代码里展开。框架侧对这几种类型留普通重载，不再每个字段做 `enable_if`、`decltype` 和递归模板。其余框架新增和改动按同一条来，不靠再套一层类型推断复用。改生成器和对应头，生成头不要手改。字节、JSON 和 mysql 行映射用现有回归锁住。
- 字段分发：2026-11-13 测过 switch、map 和现在的宏，记录在 `docs/pb-dispatch.md`。宏编译快，switch 解码大约快 1 微秒、编译大约慢到 6 倍。两套都留给使用方自己选。本仓库生成的协议解码目前用宏。

## 多线程协程批量创建/入队偏慢（2026-11-14）

- 现状：协程压力改成端到端 `live` / `wake` / `churn` 后，本机 WSL 单线程 `live` 约 69 纳秒/条（约 1450 万条/秒），多线程 3 工人约 4339 纳秒/条（约 23 万条/秒），大约慢 60 倍。`churn` 单线程约 1299 万条/秒，多线程约 373 万条/秒，也明显掉。数字在 `docs/lasttesting.md`。多线程 `StartTask` 经 `PostCoroutine` 进共享队列，工人抢同一把 `stMutex`，`Hold` 挂起列表还有一把锁；批量创建时入队和执行叠在一起。
- 方向：回头优化多线程任务队列与挂起登记，减少批量 `StartTask` 的锁竞争；可考虑分片队列、批量入队或挂起列表无锁/分片。先用现有 `coro` 用例锁住单线程数字，再比多线程 `live`/`churn`。不先改语义。

## 其他语言的序列化

- 需求：序列化出去的标准数据结构统一为小端。非 C++ 的对端按这份标准读写 `modules/protoconfig` 里的协议。HTTP 对接先用已有 JSON；要做 RPC，或把 rpcdata 放进 Redis 的对端，必须用同一套二进制。先只生成一种真正要接的语言。现在的小端机器上，已经写出的包和 Redis 里的 rpcdata 字节必须保持不变。
- 现状：二进制由 `proto_tool` 生成，不是标准 Protobuf，不能用 `protoc` 解析。一条字段是 `uint32` 字段号、`uint32` 长度、裸内容，每个字段只占一条，重复字段号拒绝。`vector`/`map` 的字段体是数量加元素，嵌套 rpcdata 的字段体是自己的整段序列化。不认识的字段号按长度跳过，缺字段保持原值。序列化层已统一小端（字段头、长度前缀、定宽整数、枚举、float、double），大端主机按字节翻转，`mysql` 用例的离线行映射检查和 `hold` 的 binary container 用固定字节样例锁住输出。MySQL 客户端已经接入，数据库行映射见 `docs/database.md`；schema migration 还没有。其余 rpcdata 已有 JSON；标了 `key` 的结构不生成 JSON。Web 的 TLS 不进框架，需要时在外面用 nginx 反代到现有 HTTP。
- 方向：然后把这份标准写成不依赖 C++ 的说明。schema 仍是 `protoconfig`，在 `proto_tool` 上加一种语言的后端，不另起第二套协议。只做 Web 时先生成 JSON 编解码；要接 RPC 或 Redis 里的 rpcdata，再生成二进制。

## 异步 webserver / 其他语言接入的转发层（2026-11-14）

- 需求：做一个异步的 webserver，或者等其他语言的对端按协议接入，由它做转发层：外部请求进来，转成框架内的 RPC 发给后端服务，回包再转回去。
- 现状：`modules/webserver` 使用框架 HTTP 服务端，会话挂在 poll 上，处理函数可通过协程调用框架 RPC、Redis；详见 `docs/module.md` 的 Web 段。其他语言的二进制序列化还没生成，见上面「其他语言的序列化」。
- 方向：按实际业务接入和路由需求完善转发层；其他语言按 `protoconfig` 的二进制协议直接接入时，再确定协议转换和路由方式。先确认要接的对端和协议。

- 序列化格式变更要写明：`b269fb7` 起 vector/map 改为每字段一条、字段体为数量加元素，裸 `<<`/`>>` rpcdata 加长度前缀，`ReadFields` 对重复字段号抛 `ParserException`。旧版编出的报文和旧版存进 Redis / MySQL 的 PB blob 全部读不回来，`FromDbBlob` 显式拒绝多条目。项目尚无跨语言对端时可以接受，要在文档里记成不兼容变更，避免误以为平滑升级。
- `db` 用例从 `all` 移除后连接池异常路径无覆盖：fake 驱动能注入的驱动抛异常→不健康连接剔除、失败调用不改输出、事务回滚语义，现在没有任何用例跑；Windows 端也没有任何 DB 行映射检查（`mysql` 只在 Linux 显式跑）。决定是补回一个轻量 fake 用例，还是在 `docs/testing.md` 记成有意取舍。PB 编码黄金字节已由 `hold binary container` 补进 `all`，DB 特有部分仍只在 mysql。
- `ThreadPool::AddStopHandler` 停止后返回 `OI_ERR_CONNECT` 语义不准，换个对得上的错误码或加注释。
