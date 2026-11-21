# 待办

## 2026-11-21 全仓 review 待修项

- 分诊：原审查确认 6 个 P1、7 个 P2、10 个 P3。按“现实触发 × 后果 × 修复成本”重排，只留 6 条先做；其余移到「先确认」「观察/暂不修」，不删除，保留证据。
- 对象：master `3bc339b`，静态通读 cbaselib 自有代码、modules、tools；未编译、未跑四路回归。7 路 subagent 分模块审查，主对话逐条回读代码核对；审查期间 subagent 越权改动的 5 个文件已还原，工作区干净。
- 验收：修完按 `docs/testing.md` 跑四路 `--case all`（Windows/WSL Linux × 单线程/多线程）。以下先做项中，logd 轮转、coro 断言、HTTP Expect 要补或扩回归，不能只靠现有用例。

### 先做（6 条）

1. **配置整数越界导致启动 terminate**。现状：`GetAs` 不捕获 cpptoml 的 `underflow_error`/`overflow_error`（`cbaselib/base/inc/oi_config.h:29-40`；cpptoml 只 catch `out_of_range`），各 main 的 `StartFramework` 不在 try 内。触发：`threadcount=3000000000`、`Timer.offset_ms=-50` 等越界整数。方向：`GetAs`/`GetArrayAs` catch `std::exception` 返回 false，或启动阶段统一转 `OI_ERR_INVALID_ARGUMENT` 并打印键名。
2. **WSL 清理可能误杀用户 6379 Redis**。现状：`matches` 只认参数里的 `:6379`（`tools/cleanup_linux_tests.py:14-26`），`--port 6379` 或配置文件写法不匹配，随后按 cwd 归属命中 `APPLICATIONS` 分支。方向：解析 `--port`/配置文件端口，或直接拒绝任何 6379 的 `redis-server`；cwd 不能作为唯一判据。
3. **logd 跨日打开新文件失败后不再重试**。现状：`OpenLogFile` 失败只打印并 `continue`（`cbaselib/logd/src/log_file_instance.cpp:222-239`），轮转标记被消费，下一次投递要等下一个跨日。触发：零点建目录/打开失败（磁盘满、权限、临时 IO 错误），之后恢复。后果：整天日志落在旧日期目录，按当天目录找不到。方向：失败时保留 `m_sPendingRotate`，后续每批写入前重试打开。
4. **`coro` 回归断言空转**。现状：脚本只要求日志出现 `coro live=` 和字面量 `bad=0`（`tools/regression/cases/coro.py:14-19`），应用日志把 `bad=0` 写死（`modules/ipcclient/src/coro_mgr.cpp:224`）。触发：`--case coro`。后果：`live=0` 或协程出错仍 PASS，月度基准不可信。方向：解析 `live/churn/live_ns/wake_ns` 并断言大于 0；应用输出真实 bad 计数。
5. **CMake 配置改写已跟踪的 clangd 索引文件**。现状：`framework.cmake:176-186` 直接写源码根目录；Linux/Ninja 构建后与仓库里跟踪的 Windows 版本不同，工作区变脏。方向：写构建目录，或只在内容一致时保留；至少把该文件加入忽略。
6. **HTTP 服务端对 `Expect: 100-continue` 一律回 417**（webserver 对外时优先）。现状：`ParseHeaders` 遇到任何 `expect` 都返回 417（`cbaselib/http/src/oi_http_server.cpp:189-192`）。触发：curl 大于 1 KiB 的 `-d`/`--data-binary`、.NET `Expect100Continue` 默认开启。后果：不重试的客户端带大正文的 POST 全部失败。方向：识别 `100-continue` 后先写 `100 Continue` 再读正文，或忽略该头；只对未知 expectation 回 417。

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
