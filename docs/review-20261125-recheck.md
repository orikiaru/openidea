# 2026-11-25 晚全仓复核（087a697）

- 日期：2026-11-25
- 对象：`master 087a697`，工作区干净
- 方法：8 个分区独立只读审查 + 主对话回读证据；未编译 C++，未跑四路矩阵
- 已有基线：`docs/review-20261125.md`（793b572）、`todo.md`、`todonext.md`；本轮以当前代码为准，已知项只核对状态

## 0. 结论

- **未发现新的 P1。**
- **4 条 P2**：HTTP 服务端收包额度未对齐解析上限；registry 周期重载绕开 Update 且失败后可能永久停摆；registry owner 键单独丢失会误判占用并永久停登；Linux 矩阵的 HTTP 释放探针未编译，检查仍恒通过。
- **约 20 条 P3**，集中在生成器非法输入、端口层错误路径、测试断言口径和诊断信息。
- **已知 P1 未修**：RPC 半关闭丢回包。本轮复核状态不变，不能按“0 P1”表述。
- Python 单测（`test_agent_route`、`test_preflight_ports`、`test_test_safety`、`test_matrix_outcomes`、`test_regression_checks`）共 107 例实测通过；C++ 未重编，矩阵以提交记录为准（Windows ST、Linux MT/ST 22/22，Windows MT 21/22，registry 清理 WinError 5）。

## 1. P2

### 1.1 HTTP 服务端默认收包额度 64 MiB，没有像客户端那样对齐 8 MiB + 64 KiB 解析上限

- 位置：`modules/webserver/src/web_server.cpp:243-248`、`cbaselib/net/src/oi_session_manager.cpp:32-49`、`cbaselib/http/src/oi_http.cpp:1000-1001`、`cbaselib/http/src/oi_http_server.cpp:550-554`
- 触发：keep-alive 连接上 handler 未返回（`m_bBusy`，例如 `/query` 等慢上游），客户端继续发送流水请求或大 body；`OnDataIn` 直接 return，TCP 通道仍按会话额度读入并追加到 `stInputBuffer`。
- 现状：客户端管理器把自己的额度压到 `kMaxHeader + kMaxBody`（8 MiB + 64 KiB）；服务端走 `LoadSessionLimits()`，`deploy/config/webserver.toml` 没有 `max_receive_buffer_bytes`，拿到全局默认 64 MiB + 8。
- 后果：单连接在 handler 慢时可常驻约 64 MiB，且 HTTP 监听侧没有连接数上限；多个慢连接可叠加。反向收紧也不直观：配到小于在途流水数据量时会被 ENOBUFS 直接断连。
- 修复：在 `WebServer::Init` 把服务端接收额度夹到 `kMaxHeader + kMaxBody`（允许显式更小，但校验下限），或至少把该键写进 `deploy/config/webserver.toml` 并注明下限来源。

### 1.2 registry 周期重载自挂 BaseTimer，异常/启动失败后整量刷新永久停摆

- 位置：`cbaselib/registry/src/registry_server.cpp:128-136`（ReloadTimer）、`:269-287`（Boot 注册）、`:419-446`（m_bReloading）
- 触发：`Reload()` 在第一次 `co_await` 之前抛异常（拷贝 `m_vWithdrawnOwners`、构造 EVAL 参数等，分配失败即可）；或 `FrameworkTimer::AddTimer` 返回失败（返回值未检查）。
- 现状：`StartReload` 先把 `m_bReloading` 置真，唯一清零点在 `Reload`/`ReloadMysql` 协程内；协程异常被 promise 吃掉，标志永远为真，下一次 timer 直接返回。首次失败后 `m_bSnapshotOk` 永远为假，注册中心对所有拉取回 FAIL；中途失败后过期实例永远摘不掉。
- 规则：`AGENTS.md:123` 明确单例周期任务必须 `OnSecondUpdate`/`OnTickUpdate`，不许自挂 `BaseTimer`/`AddTimer`。
- 修复：改成 `OnTickUpdate(uint64_t)`，用 `SteadyMilliseconds` 记下次时间，删掉 `m_bReloading`；保留现方案则必须检查 `AddTimer` 返回值并保证异常时复位。

### 1.3 registry owner 键单独丢失被当成“被占用”，客户端永久停登

- 位置：`cbaselib/registry/src/registry_server.cpp:44-51`（kRefreshLua）、`:714-722`（REG_TAKEN）、`cbaselib/proto/src/oi_registry_client.cpp:494-500`（m_bLost）
- 触发：`owner:<name>:<id>` 单独消失，而 `svc:<name>:<id>` 仍是本进程登记的值。正常路径两个键同 SET/PEXPIRE/DEL，不会分离；外部因素（共享 Redis 配 `maxmemory` 淘汰、运维误删、恢复一半）可造成。
- 现状：`kRefreshLua` 只判断 `v` 是否存在；`v` 存在但 owner 键为空时返回 0，服务端按 REG_TAKEN 把本连接标 `superseded` 并从快照隐藏，回 `OI_ERR_EXIST`；客户端置 `m_bLost` 后不再登记，后续 Register 也因 superseded 继续回 EXIST。
- 后果：健康进程静默下线，日志还显示“被别的进程占用”；只能重启恢复。
- 修复：Lua 区分“owner 键不存在”和“owner 键不同”：地址是自己的且 owner 不存在时补写 owner 并返回 RENEWED；只有 owner 存在且不等于本令牌才返回 0。

### 1.4 Linux 矩阵的 HTTP 释放探针未编译，新增检查仍恒通过

- 位置：`modules/ipcclient/src/http_mgr.cpp:146-172`、`CMakeLists.txt:67-71`、`tools/test_matrix.py:300`、`tools/regression/cases/hold.py:307`
- 触发：Linux 两路矩阵（`OI_ENABLE_TEST_HOOKS=OFF`）跑 `hold`。
- 现状：`OI_HTTP_RELEASE_TEST_HOOK` 只在 `OI_ENABLE_TEST_HOOKS=ON`（Windows 矩阵）时定义；Linux 下探针分支不编译，`#elif !defined(__GLIBC__)` 在 glibc 上也不成立，只剩 `mallinfo` 的 8 MiB 总增量启发式。`hold.py` 对 `bad=0` 的断言无条件通过，Windows 才断言 `probe count=32`。
- 后果：32 条 1 MiB 响应里若只漏释放 1–7 条（<8 MiB），Linux 仍 PASS；476f229 声称用真实探针消除假绿，实际只消除了 Windows 一侧。
- 修复：让 Linux 测试构建也定义 `OI_HTTP_RELEASE_TEST_HOOK`（与测试钩子解耦），或把缺探针分支改成 `#else` 记未覆盖，不能继续用它判绿。

## 2. P3

### 2.1 base / timer / bytes

- **解码预算 4× 帧上限覆盖不了高计费比容器**：`vector<string>` 按 `sizeof(std::string)` 计费、线上每元素最小 4 字节，约 32 MiB 帧即可超 256 MiB 预算；`map<uint32,uint32>` 约 5×，约 51 MiB 帧超预算；高 `sizeof` 的 rpcdata 更早。文档已声明 4× 策略，但提交说明称“大帧不再被预算误拒”，现有回归只覆盖约 1.9× 的字符串。触发后 `ChargeDecode` 抛 `ParserException`，`ProtoSession` 直接断连，不给错误回包。建议按最坏计费比调系数或按容器类型计费，并补 `vector<大结构>`/`map` 边界回归。位置：`cbaselib/proto/src/oi_rpc_session.cpp:87-91`、`cbaselib/base/inc/oi_bytes_stream.h:285-290`。
- **默认 64 MiB 解码预算外溢到 Redis rpcdata 解码**：`BytesStream(stReply.str, stReply.len)` 用默认预算；超过 64 MiB 逻辑解码的存量值从“可解码”变成 `OI_ERR_DECODE`，与数据损坏同码，无配置/诊断。位置：`cbaselib/redis/inc/oi_redis_value.h:216-228`。
- **`SleepMs` 忽略 `AddTimer` 返回值**：失败时 `WakeLater` 已 delete，协程永不唤醒；`await_suspend` 还是 `noexcept`，`new`/`CallOnLoop` 分配失败直接 terminate。位置：`cbaselib/base/src/oi_coro.cpp:201-225`。
- **timer 未 Init 时 `GetTime`/`GetTm` 读未初始化成员**：构造函数只初始化 `m_stCurrentTimeValue`/`m_iCurrentTick`/`m_iTickOffsetMilliseconds`；`m_iCurrentTime`、`m_stCurrentCalendarTime` 未初始化。位置：`cbaselib/timer/src/oi_timer_manager.cpp:16-21`、`cbaselib/timer/inc/oi_timer_manager.h:184-189`。

### 2.2 net / port

- **Windows UDP 绑定没有 `SO_EXCLUSIVEADDRUSE`**：TCP 已设置，UDP 分支只 bind；文档说“UDP 两个进程抢不到同一个口”，Windows 上不成立。位置：`cbaselib/port/oi_net_os.cpp:411-423`。
- **`WSAStartup` 失败被吞**：忽略返回值并置 `bStarted=true`，之后所有 socket 以 WSANOTINITIALISED 失败且不重试，日志失去原始原因。位置：`cbaselib/port/oi_net_os.cpp:22-34`。
- **非阻塞设置失败被忽略**：`NetOs_OpenSocket`/`NetOs_Accept`/`NetChannel` 都不检查 `NetOs_SetNonblock`；失败后阻塞 fd 进入 poll，最后一次读/写可能挂住整个 poll 线程。位置：`cbaselib/port/oi_net_os.cpp:44-50`、`:195-205`。

### 2.3 proto / rpc

- **`[RpcRoute]` 非法配置 fail-open**：`RouteManager::Group` 的 `call_once` 丢弃 `LoadSessionLimits()` 返回码，非法时仍按 64 MiB 默认值建路由连接；组件管理器此时会启动失败，路由不会。位置：`cbaselib/proto/src/oi_rpc_route.cpp:64-70`。
- **未发出请求就返回 `OI_ERR_TIMEOUT`**：选路预算耗尽、尚未 `Dispatch` 时把状态映射成 Timeout；文档写的是“已经发出之后超时”，且该分支无 `stage=route` 诊断，业务无法判断能否安全重试。位置：`cbaselib/proto/src/oi_rpc_route.cpp:372-383`、`cbaselib/proto/inc/oi_rpc_coro.h:773-783`。

### 2.4 HTTP

- **非法回复/204/304 丢弃 `pBodySource` 但不 Cancel**：`bValid`/`bBody` 为假时 source 没进 `m_pSource`，随 `Completion` 析构释放，流对象收不到取消信号。位置：`cbaselib/http/src/oi_http_server.cpp:480-486`、`:521-525`。
- **回复兜底成 500 无诊断**：handler 返回非法状态码/头/body+source 时静默 500，没有 method/path/原因。位置：`cbaselib/http/src/oi_http_server.cpp:484`。

### 2.5 redis / db / logd / redisserver

- **进程内共享 `RedisClient` 不拦截改变连接状态的命令**：`CallRedis({"MONITOR"})`/`SUBSCRIBE`/`CLIENT REPLY OFF` 会把后续回执整体串位或全部超时；redisserver 代理已拦截同类命令，客户端没有。位置：`cbaselib/redis/src/oi_redis.cpp:414-425`、`modules/redisserver/src/redis_server_session.cpp:163-175`。
- **logd 越界 level 静默丢帧**：`iLevel` 越界时既不落盘也不断连、不记日志，帧照常消费。位置：`cbaselib/logd/src/log_server_session.cpp:74-89`。
- **redisserver 两处额度缺交叉校验**：`Redis.max_pending_bytes` 大于 `RedisServer.max_receive_buffer_bytes - 8` 时，`ScanCommand` 认为合法并继续等数据，接收缓冲满后 ENOBUFS 断连，绕过“解析即拒绝”的错误日志。位置：`modules/redisserver/src/redis_server.cpp:304-313`、`redis_server_session.cpp:227-234`。

### 2.6 registry / 生成器

- **rpcdata 文件名小写，重名检查大小写敏感**：`Foo` 与 `foo` 生成同一个 `oi_rpcdata_foo.h`，后写覆盖前者，生成成功、编译期才报不完整类型。位置：`cbaselib/proto/proto_tool/serialize_data_parse.py:154-155`、`proto_parse.py:412-416`。
- **浮点 map 键/浮点 key 成员被接受**：NaN 破坏 `std::map` 严格弱序，±0 被当等价；文档只列整数/string/key 结构。位置：`cbaselib/proto/proto_tool/proto_parse.py:16-22`。
- **XML 协议入口已不可用但文档仍称支持**：XML 分支不解析 `<protocol>`，字段缺 `seq` 会报错；`cbaselib/CMakeLists.txt:42-48` 仍调用旧 XML 且忽略失败。位置：`cbaselib/proto/proto_tool/proto_parse.py:136-143`、`docs/module.md:102-104`。
- **组件名不做 C++ 声明名校验**：`Bad-Name`/`9Lives` 生成非法类名；大小写不同的组件写同一小写文件，后写跳过，另一个类消失。位置：`cbaselib/proto/proto_tool/module_data_parse.py:169-179`、`component_file_gen.py:116-123`。

### 2.7 ipcclient

- **`coro_mgr` 新 bad 中多项恒真**：`HeldCount()` 只 push 不摘，`(iHeld != m_iLive)` 在正常规模下恒假；`*Ns <= 0` 也不可能成立；“永久不恢复”表现为无限空转，不是 `bad=1`。位置：`modules/ipcclient/src/coro_mgr.cpp:224-227`。
- **`udp rpc calls=` 打印配置总量而非实际调用数**：`fnReport` 参数顺序把 `m_iTotal` 填到 `calls=`，回归又把 `calls=1000` 写成字面断言。位置：`modules/ipcclient/src/udp_mgr.cpp:628-633`、`:707`。
- **`timer_mgr` 在 poll 线程 `sleep_for`**：定向注入没有隔离标注，违反“应用层不要睡线程”，容易被后续代码复制。位置：`modules/ipcclient/src/timer_mgr.cpp:199-218`。

### 2.8 tools / 回归

- **`web-coro` 预检漏 UDP**：web-coro 会起 ipcserver，`[IpcUdp]` 仍绑 17983，但 `scene_ports` 和 UDP 跳过集合把它一起排除；端口被占时表现为用例启动失败而非预检失败。位置：`tools/regression/ports.py:40-57`。
- **`--platform both` 的 summary 缺 `completed`**：both 分支只写 passed，不写 completed/restored，交接按文档检查 completed 会取不到键。位置：`tools/test_matrix.py:207-211`。
- **`parallel_matrix` 收尾杀不到孙进程**：只 terminate lane 驱动（Windows 不跑 test_matrix 的 finally，WSL 只杀 wsl.exe 外壳），产品进程/编译进程可能残留；没有残留检测。位置：`tools/parallel_matrix.py:123-134`。
- **`web_stop_probe.py` 未接入任何回归且无 `-O` 保护**：手工 `python -O` 跑时断言全部剥离，截断/RST/非零退出都会以 0 退出。位置：`tools/web_stop_probe.py:101-118`。

## 3. 已知未修项状态

- **P1：RPC 半关闭丢回包**。`TCP_Channel::DataIn` 交完最后数据后 `PassiveClose`，会话在 handler 回包前被摘，迟到响应按 sid 查不到。仓库客户端不使用半关闭，故未在正常回归暴露；合同仍未定。位置：`cbaselib/net/src/oi_tcp_channel.cpp:60-69`、`cbaselib/net/src/oi_net_session.cpp:9-29`。
- 根目录 `todo.md` 的六组近期行动仍开放：Windows MT registry 清理 WinError 5、关键回归假绿、timer 公有入口未 Init 除零、日志失败交付语义、UDP EAGAIN 写关注、configure 改写 clangd。
- `todonext.md` 的条件项（ServiceTable 短命 observer、RPC half-close 合同、`m_bLost` 恢复、logd 跨日重试、`res_id` 协议号、TLS 聚合限额、`sid_t` 位宽、poll 注册失败 fd 顺序等）状态未变，按原触发条件排期。

## 4. 验证与边界

- Python 单测：`test_agent_route` 27 例、`test_preflight_ports` 2 例、`test_test_safety` 45 例（1 跳过）、`test_matrix_outcomes` 18 例、`test_regression_checks` 15 例，全部通过（合计 107 例）。
- C++ 未重编；矩阵采用既有记录：Windows ST、Linux MT/ST 22/22，Windows MT 21/22（registry 业务 PASS 后删除临时 exe 报 WinError 5，整路失败，未吞错）。
- 本轮所有 C++ 结论为静态阅读；OOM、Redis 键淘汰、真实慢 handler 内存堆积、Windows 抢绑、非阻塞设置失败等未做故障注入。
- `deploy/bin` 不完整，不能用旧二进制冒充本轮结果；完整验证仍需按 `docs/testing.md` 在 Linux/Windows、单线程/多线程各跑一遍。

## 5. 建议顺序

1. **P2-1 HTTP 服务端额度**：配置/默认值一处收口，直接降低慢连接内存上限。
2. **P2-3 registry owner 键丢失**：Lua 区分“不存在”和“不同”，避免永久停登。
3. **P2-4 Linux 探针假绿**：让测试构建定义探针或把缺失标成未覆盖。
4. **P2-2 registry 周期重载**：改 `OnTickUpdate`，同时消除 `m_bReloading` 卡死和 AddTimer 返回值问题。
5. **P3 快修**：`[RpcRoute]` fail-open、未发出的超时码、HTTP source Cancel/500 诊断、logd 越界 level、redisserver 额度交叉校验。
