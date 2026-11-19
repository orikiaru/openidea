# 待办

## Redis 迟到回包偶发问题：等待复现

现有 Hold 覆盖 BLPOP 超时、迟到回包、后续 PING 及超时边界竞争；偶发问题尚无确认根因，复测通过本身不能证明问题消失。

- 复现时按 `docs/testing.md` 的单项复现和清理规程运行 `--case hold-redis`，指定 `--artifacts` 与 `--redis-timeline`，保留结果、各进程输出及 Redis 时间线。记录平台、线程模式、构建版本、Redis 程序、端口偏移、失败轮次、退出码、客户端超时和耗时、PING 的状态和回包类型，以及前后清理结果。
- 对照时间线中的 PING、测试 BLPOP 执行时间和上游连接地址；MONITOR 只能说明命令执行，不能证明回包到达。若需判断丢包或错配，还要采集代理和客户端收发时间，避免记录完整请求和敏感值。
- 关闭条件：复现并确认根因，修复后用对应场景验证且完成规定的平台和线程模式回归；若无法复现，保留观察，不以一次通过关闭。

## Windows 测试进程清理：低优先级观察

Windows 多线程注册发现用例完成了业务检查，删除临时目录内的 ipcserver.exe 时遇到 WinError 5；复测连同清理通过。再出现时保留完整路径、进程退出时间、清理报错及归属核验结果，用来区分进程仍在退出和文件被其他程序占用。

## 定时极端溢出

暂缓，待有明确复现或实际需求后再分析。


## HTTP 消息能力后续分期

对照 [libhv HttpMessage.h](https://github.com/ithewei/libhv/blob/master/http/HttpMessage.h) 的消息接口，当前提供请求/响应、原始正文和可重复 header；不以接口名称相同或数量相等作为兼容承诺。以下按需求独立实施并保留既有接口。

- [x] **Cookie**：提供解析与构造能力；以多个 Set-Cookie、大小写与空值、转义往返验收。Cookie 属性至少覆盖 domain、path、expires、max-age、secure、httponly 和 SameSite；自动 cookie jar 单独决策，不隐式跨请求保存。
- [x] **重复 query**：提供解析与构造能力，明确多值顺序及缺失值语义；以重复参数、空值和转义往返验收。
- [x] **URL 编码表单**：支持 `application/x-www-form-urlencoded`，明确重复字段、空值及尺寸上限；以真实服务往返、边界拆包和空字段验收。
- [x] **multipart 表单**：支持文本及文件字段，明确重复字段、二进制内容和尺寸上限；以真实服务往返、边界拆包、空字段和文件内容字节一致验收。JSON 继续使用现有 JsonCpp 和 rpcdata 能力。
- [x] **流式收发**：提供分段读取、写入及背压，定义取消、超时、断连和资源生命周期；以大文件分段收发、慢消费者、断连和内存上限验证。流式路由按片消费正文；普通缓冲路径仍按完整正文处理。
- [x] **客户端 HTTPS**：提供证书链和主机名校验；本地可信 CA、错误主机名、不受信任证书、握手超时与连接关闭由 Web 回归覆盖。证书管理与部署单独设计。

服务端 TLS 由 Nginx 等反向代理终止，代理与后端之间走 HTTP，本项目不实现服务端 TLS。

libhv 消息还提供文件、JSON、类型化值等便捷接口；是否引入以实际需求为准，不影响上述阶段的验收。

## 2026-11-19 全仓审查：修复进度

2026-11-19 对 HEAD `52dd507` 做了全仓只读审查（cbaselib、modules、tools 第一方代码；未编译、未跑回归；生成器异常输入在临时目录实测）。确认 0 个 P1、6 个 P2、12 个 P3，本轮先登记、不修改代码。P2 第 1、3、4、6 项已修，第 2 项的回归预检已修但生产监听待修，第 5 项待修；修复后的完整矩阵仍待验证。P3 随相关模块下次改动一并收口。

### P2

- **已修：明文 HTTP 传输错误被当成正常 EOF**。明文传输结束时保留底层错误，RST 等读错误返回连接错误，避免无长度响应被静默截断。
- **部分修复：Windows 端口占用预检失效**。回归预检使用 `SO_EXCLUSIVEADDRUSE`，已有普通监听或复用监听均会被识别；生产监听仍使用 `SO_REUSEADDR`，待修。
- **已修：注册中心同值重复登记后旧会话删除在线登记**。新登记接管旧绑定，Redis 租约使用独立 owner 令牌，撤销时仅删除自身租约；回归检查被接管连接的心跳和重复登记均返回占用，接管方心跳正常；旧连接关闭后地址、owner 和快照仍在，新连接关闭后清除，全新连接可以重新登记。
- **已修：定时器时间快照跨线程读写竞争**。四个 getter 均在锁内读取，时间结构按值返回。
- **logd 延后帧缺单帧限额，可永久卡住队列并让 Stop 退不掉**（`cbaselib/logd/src/log_manager.cpp` 的 `DeferWrite`/`QueueWaiter`/`ResumeWaiting`，`cbaselib/logd/src/log_server_session.cpp` 的延后分支）。触发：`Log.max_pending_bytes` 取最小值附近（回归配成 8MiB+8），客户端一次读里第二帧的 `sText` 超过 `max - 标题长度` 且仍在 `LOG_FRAME_MAX+64` 内。现状：该帧仍入队，但队首永远放不下；本地日志持续 BUSY，`Stop()` 等待一个不会再被 Signal 的条件。修复：入队前做与 `WriteLog` 相同的单帧检查（含 `sText < LOG_FRAME_MAX`），超限按协议错误关闭会话。
- **已修：HTTP 连接池在 EOF 处理前复用会话**。读到 EOF 时先标记连接不可复用，再交付已收到的数据，避免排队请求使用即将关闭的连接。

### P3

- **生成器输入校验不足**（`cbaselib/proto/proto_tool/field_naming.py`、`serialize_data_file_gen.py`）：`long`/`size_t` 能通过命名校验并按 `sizeof(T)` 序列化（Linux 8 字节、Windows 4 字节，同一协议两边不兼容）；`rpcdata class` 生成 `class class`；超大 enum 值原样生成；`OI::Base::Bytes` 被 `QualType` 正则拼成 `OI::Base::::OI::Base::Bytes`。已用临时目录实测。修复：字段类型白名单、声明名校验、enum 范围校验，`QualType` 改整类型匹配。
- **组件生成路径不可达**（`cbaselib/proto/proto_tool/module_data_parse.py`）：`ParseComponent` 无调用方，module 里写 `component` 必然报 `NoComponent`，文档仍描述组件生成。修复：接入文本组件的解析，或删除组件生成与文档。
- **HTTP 服务端缓冲 body 与接收缓冲耦合**（`cbaselib/http/src/oi_http_server.cpp`、`cbaselib/net/src/oi_tcp_channel.cpp`）：非 chunked/chunked 都等整段 body 到齐才消费；`max_receive_buffer_bytes` 小于 body 时被 `ENOBUFS` 断连，既无 200 也无 413（web 回归把接收上限配成 131072）。修复：增量消费，或初始化时要求上限覆盖 header+body 并明确回 413。
- **服务端流空闲被当残包超时**（`cbaselib/http/src/oi_http_server.cpp` 的 `PumpStream`）：source 返回 `OI_ERR_BUSY` 不算进度，连续 10 秒无产出即断连；长轮询/SSE/慢生产者会被误杀。需确认是否设计意图；若要限制空闲，单独提供 `stream_idle_timeout` 并写入文档。
- **服务端 keep-alive 无空闲回收**（`modules/webserver/src/web_server.cpp`）：请求完成后空闲会话一直占着会话和 fd，缺少 `idle_timeout_ms`。建议补空闲计时器。
- **客户端非流式 chunked trailer 无 64 KiB 限长**（`cbaselib/http/src/oi_http.cpp` 的 `ConsumeChunks`）：只受整体缓冲上限约束，恶意对端可让客户端缓存约 8 MiB trailer；sink 路径有 64 KiB 限制。修复：累计 trailer 字节，超过 `kMaxHeader` 失败。
- **TLS `Flush` 把可重试的 BUSY 当永久错误**（`cbaselib/net/src/oi_tls_session.cpp`）：置 `m_sError` 后 `StartTransport` 一律返回 `OI_ERR_CONNECT`，SSL 已消费的明文留在 BIO 无重试路径。修复：只在不可恢复时置错误，BUSY 保留 BIO 并在可写时重试。
- **`StartServer` 登记失败不回滚监听**（`cbaselib/net/src/oi_session_manager.cpp`、`cbaselib/net/src/oi_accept_channel.cpp`）：accept 通道创建后不保存也不返回，Publish 失败时监听仍在 accept，重试因端口占用失败。修复：管理器持有 accept 通道，失败/析构时关闭。
- **RPC 完成回调抛异常时槽已摘除、协程永久挂起**（`cbaselib/proto/src/oi_rpc_call.cpp` 的 `Sweep`/`Deliver`）：`Sweep` 的 `FinishSlot` 循环没有逐槽 `try/catch`，`FailSession` 已有。触发需完成回调抛异常（概率低），后果是等待方永久挂起、同批其余到期项不收尾。修复：对齐 `FailSession` 的逐槽保护。
- **选路用旧名单回写 `ServiceGroup`**（`cbaselib/proto/src/oi_rpc_route.cpp`）：每次 `Group` 都 `Apply` 调用方缓存的名单，可能与秒更新下发的新名单竞争，短暂退役新实例或复活下线实例（≤1s）。修复：名单只由秒更新一个写者下发，或给快照加版本号拒绝旧版本。
- **`IpcUdp` 会话计数只增不减**（`modules/ipcserver/src/ipcudp.cpp`）：`UDP_PROBE_SESSIONS` 返回历史累计值而非活跃会话数；当前回归查询顺序绕开。修复：删除时减一并按 sid 去重，或改活跃集合。
- **测试工具与规则类**：
  - `tools/run_regressions.py` 清理阶段不校验服务退出码，最后一次窗口检查后崩溃的服务仍可能 PASS；建议对 `procs` 逐个校验 `returncode`。
  - `tools/build.py` 不传 `OI_WEBCLIENT_SINGLE_THREAD`，默认 MT 构建产出单线程 webclient，与矩阵 MT 不一致；按 mode 同步传该开关。
  - `tools/test_matrix.py --platform both` 的 `--define` 只加给 Linux 侧；同步加到 Windows 侧。
  - `tools/parallel_matrix.py` 启动与等待之间没有 `try/finally`，异常/中断会把已启动 lane 留在后台。
  - `cbaselib/registry/src/main.cpp`、两个模块 `main.cpp`、`modules/ipcclient/src/rpc_pending_mgr.cpp` 及生成器模板仍用默认捕获 `[&]`，违反 `docs/09-key-rules.md` 第 6 节；统一改显式捕获。
  - `modules/ipcserver/src/ipcserver.cpp` 的 `iMark == -2147483598` 反向 RPC 分支全仓库无客户端触发，属于空覆盖；补回归或删除。

验证边界与关闭条件：本轮为静态审查，未编译、未跑回归；P2 修复后按 `docs/testing.md` 跑 Linux/Windows、单线程/多线程回归，P3 随相关模块改动验证。历史 2026-11-04 的三个 P2 已核对为已修（分区路由、清理循环、矩阵中断），不重复登记。
