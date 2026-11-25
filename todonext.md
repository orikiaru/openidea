# 条件待办（暂不占当前排期）

> 「不值得当前做」不等于问题不存在、已修或已验收。近期仅做根目录 [活动待办](<todo.md>) 六组；本页合并三轮重复记录，保留每个独立未完成事项、延期理由和重新提级条件。已完成历史见 [归档](<docs/fixed.md>)，复现导航见 [补充入口](<docs/todo.md>)。
>
> 证据源：[11-24 review](<docs/review-20261124.md>)（含 11-21 遗留）、[11-25 review](<docs/review-20261125.md>)。它们是完整历史来源，文内行号、优先级与「未修」均为当时快照，不以旧建议顺序覆盖本次分诊。下文代码行号也仅作历史定位；本轮仅文档整理，无新测试、源码或测试改动。

## A. 等实际接入或先定合同

- **通用 TCP 候选回退**：当前真实拨号路径只选一个 IPv4、否则一个 IPv6，失败结束本代；不是「未使用路径」。证据：[StartConnect](<cbaselib/net/src/oi_session_manager.cpp#L235-L267>)、[Resolve](<cbaselib/net/src/oi_socket.cpp#L425-L441>)、[连接失败](<cbaselib/net/src/oi_connect_channel.cpp#L70-L81>)。延期：当前部署需要多候选/双栈的触发需求未证，本轮无实测；[HTTP 已有顺拨](<cbaselib/http/src/oi_http.cpp#L1099-L1179>)，不能混为一谈。提级：真实双栈/IPv6-only 接入失败；同一代次顺试候选，真实 localhost IPv4 失败后原调用连 IPv6，并验证取消、过期代次不接管新连接。
- **通用 TLS 聚合与 wire 限额边界**：[喂入与 Read](<cbaselib/net/src/oi_tls_session.cpp#L234-L257>)、[跨轮解密入口](<cbaselib/net/src/oi_tls_session.cpp#L431-L461>)可能把前轮半个 record 的产出计入本轮明文额度而误拒。[HTTP 独立 8 MiB + 64 KiB](<cbaselib/http/src/oi_http.cpp#L999-L1001>)已避免本轮回归，不等于通用问题已修。延期：通用限额/record 边界尚无业务复现；真正接入时再用受控跨轮 record 验证，设计分批交付和 BUSY 恢复，不丢、不重复消费。
- **ServiceTable 短命 observer**：[历史机制](<docs/review-20261125.md#3-新增-p2框架--协议>)是裸指针无注销；当前管理器按进程常驻，两个测试 observer 为 [static](<modules/ipcclient/src/multi_mgr.cpp#L466-L467>)，栈探针在 ST 外线程注册被拒且已有断言。延期：没有合法短命接入；若未来支持动态生命周期，必须连同锁外已复制的在途通知一起注销/保活，不能只 erase 列表；验收析构后绝不回调及并发通知收尾。
- **RPC half-close 合同**：[历史链](<docs/review-20261124.md#31-rpc-半关闭丢回包>)为 FIN 摘会话、延后响应按 sid 查不到；仓库客户端不用半关闭，已知后果不因延期消失。提级：外部客户端要求 `shutdown(SHUT_WR)` 后读响应；先明确不支持并文档/回归锁定，或最小延迟摘会话并真实套接字验证原响应与资源收尾，不预先大改连接生命周期。
- **被接管后的恢复**：`m_bLost` 只置不清，旧进程停止登记，占用方退出/租约过期也只能重启，见 [历史策略疑点](<docs/review-20261124.md#7-已知项状态复核后不变>)。延期：接管究竟永久退让还是允许恢复未定；有自动恢复需求后先定合同，再做空出检测/恢复或显式状态通知及接管交错验收。
- **logd 跨日失败后是否重试**：[轮转历史](<docs/review-20261124.md#21-logd-跨日打开新文件失败后仍不重试未修策略待确认>)中打开失败后继续旧日期文件、轮转标记消费，下一次要等跨日；[已完成历史](<docs/fixed.md#2026-11-21-审查问题修复>)只保证失败继续写且不退出，并非 pending 重试已做。延期：先等运营明确按日文件恢复要求；若要求恢复，保留 pending 并后续批次重试、限频诊断，Linux 注入次日路径为普通文件后恢复，检查当日文件、失败期间不丢及 stderr 原因；若接受现策略，明确写入合同再关闭，不把未决事项当已接受。
- **逐协议大小限制**：当前仅会话级 `max_frame_bytes`；逐协议非 0 超限丢弃、0 不额外限制尚为设计。延期：无具体协议需求及调用方错误合同；有需求再定配置、拒绝/丢弃结果与边界回归，不称已实现。
- **跨语言序列化、转发层、schema migration**：等待真实对端/路由/数据库升级需求再做，不另起协议。当前不是标准 Protobuf，字段为小端 `uint32` 号/长度/内容、每号一条、未知跳过、缺失保持原值，vector/map 为数量加元素，嵌套为整段编码；[生成器](<cbaselib/proto/proto_tool/serialize_data_file_gen.py>)是唯一 schema 后端入口。HTTP 先用 JSON；RPC/Redis 接入时先生成一种真正要用的语言并用黄金字节互通。[Web 已有异步 HTTP→RPC/Redis 能力](<docs/module.md>)，按真实业务再补路由/转换；[数据库映射](<docs/database.md>)已有，schema migration 未有。**现有格式不兼容交付说明已列近期第 2 组，不等跨语言再写。**
- **尚无使用前提的协议/接口差异**：[历史已知项](<docs/review-20261124.md#7-已知项状态复核后不变>)的不同 `res_id` BUSY/通知回执协议号不匹配（当前无此协议），Redis 空队列完整回执静默丢弃（当前无额外回执路径）；[ST PostCoroutine 空实现、锁两套接口不对称、Windows pthread 递归/join/静态初始化缺口](<docs/review-20261124.md#61-规则机械项>)当前无自有调用方。延期：先有实际接口使用需求；接入前定合同并加负例，不能据「当前不可达」声称已修。

## B. 需要抓到复现证据

### hold 的 redis late ping 偶发超时

- 历史：2026-11-11 Linux MT 的 BLPOP 3 秒在客户端 2 秒超时后，同连接 PING 偶发 `ok=0 status=1 reply_type=-1 wall_ms≈2010`、`hold done bad=1`；单独三次挂一次，无并行负载也出现，Windows/Linux ST 当时未见。正常约 900–1100ms；隔离诊断四次通过耗时 1040/910/844/849ms，后三次 MONITOR 显示同上游连接在 BLPOP 后约 3.01–3.08 秒执行 PING，**不能证明回包送达**。未找到原失败日志、未抓到失败时间线。
- 静态核对：超时项仍留队认领自己的回包、断连按会话清理，FIFO 与 `m_iSwept` 账目自洽，未确认旧回复串位；**永不回包仍可能永久占用 `max_pending`**。延期：根因未证，复跑通过不能关闭，也不先放宽时间预算。
- 复现按 [测试规程](<docs/testing.md>) 的单项/清理步骤运行 `--case hold-redis`，指定 `--artifacts` 与 `--redis-timeline`。保留结果、各进程输出、Redis 时间线及前后清理结果，记录平台、线程、构建版本、Redis 程序、端口偏移、失败轮次、退出码、客户端超时/耗时、PING 状态/类型。
- 对照 PING/BLPOP 执行时间与上游地址；再抓代理/客户端收发时刻，区分晚到、丢失、错误归属，脱敏且不录完整请求。提级/关闭：复现确认根因后修复，以原失败场景及规定平台/线程模式验证；不能复现就继续观察，不以一次通过关闭，不吞失败或放宽断言。

### 其他待复现疑点

- **UDP ICMP 与 poll 停止**：原 [补充疑点](<docs/todo.md#尚未证实的疑点>)未证实监听因 ICMP 停转。延期：缺故障时间线；复现时同时抓绑定口 poll 状态、读监听有效性和后续合法报文是否到达，确认失活再提级，不能只凭一次 ICMP 定根因。
- **旧 Windows ST admission 崩溃**：[故障记录](<docs/windows-st-admission-20261121.md>)保留两项旧增量产物 `0xC0000005`、带符号对照/最终矩阵通过；原崩溃无匹配 PDB、dump、二进制哈希，不能关联现版本或视为已修。提级：用匹配身份和符号重新捕获崩溃；保留原失败及对照，不用后来通过抹去。
- **Windows ST disk-full helper 退出 120（2026-11-12）**：旧两次全量 20/21，ENOSPC 已检测、清理通知 helper 120；增加 stdout/stderr 诊断后定点通过，模拟 120 仍失败，根因未修。证据结果标识 `windows-st-latest` / `windows-st-disk-diagnostic`。提级：再次失败抓 stderr、控制台/标准流生命周期；保留诊断，禁止吞 120 或放宽断言。
- **停服 DNS/其他句柄 EBUSY**：[历史链](<docs/review-20261124.md#7-已知项状态复核后不变>)涉及挂起 DNS、残包、流式 timer，`uv_loop_close` EBUSY 后释放 loop；缺停服窗口真实复现。提级：受控挂起 DNS/句柄后停止，抓资源归属及实际崩溃/泄漏证据，再定最小取消/等待收尾，不猜唯一根因。
- **poll 注册失败与 fd 复用**：[11-25 证据](<docs/review-20261125.md#3-新增-p2框架--协议>)为注册失败后先 close fd、延后 uv_close；旧 fd 被复用时误摘新注册是待注入后果。延期：窄错误路径无复现；注入注册失败+快速复用并证明新连接受影响再提级。[已排除的注销探针重复 close](<docs/fixed.md#已核对排除不是修复>)不是本条修复。
- **Blocking 控制管道 EOF 空转**：[历史点位](<docs/review-20261125.md#3-新增-p2框架--协议>)为写端破裂且 in-flight 非零时 EOF 持续可读；延期：需要罕见管道损坏。提级：真实断写端、有在途任务时抓 poll 频率/CPU及最终任务收尾，再最小修正注销策略。

## C. 低内存、极端配置与调用约束

- **C 回调异常边界**：[11-25 系统性报告](<docs/review-20261125.md#21-libuv-c-回调链缺统一异常边界>)保留 logd WriteLog/DeferWrite、HTTP Produce/OnPlaintext/RequestFrame、redisserver OnConnect/OnDisconnect/AppendReply；当前缺口主要是 string/Bytes/队列分配 OOM，不等于已测崩溃。一处外层 catch **不能**覆盖所有独立 C 入口：DNS 与 StreamPump 另有入口，Drain 应逐项隔离且异常后继续后项。延期：低内存专项、非低成本一处 catch；在支持低内存合同或出现真实逃逸异常时提级，逐入口注入并验收资源归属、诊断、同批后项不丢，不将所有异常一律静默吞掉。
- **会话/发送整数边界**：[sid_t int32 累计 2^31 回绕、有符号溢出、SIZE_MAX 发送配置及 Windows sendlen 截 int](<docs/review-20261125.md#3-新增-p2框架--协议>)需超长运行或 >INT_MAX 堆积。延期：极端可达性、位宽可能影响帧兼容；有相应规模或接入需求前先构造边界探针再限幅/定无冲突分配，不能草率改协议位宽。
- **极端 threadcount/timer_lvs**：[历史配置项](<docs/review-20261124.md#61-规则机械项>)包括工人数无上限/创建失败异常、时间轮单层可至 `0xFFFFFFFF` 槽导致 OOM。旧报告的 timer 乘积回绕与已加乘积校验须区分，仍保留边界复核，不重复登记已完成。延期：故意极端配置；放开不可信配置或专项触发时提级，验证启动受控拒绝且不分配巨量资源，不重做时间轮。
- **await_suspend noexcept 分配**：[历史链](<docs/review-20261125.md#3-新增-p2框架--协议>)中 Schedule/Post/DetachFinished 的分配失败可 terminate；延期：OOM-only，须先定失败合同再专项注入，不承诺通用 catch 能跨 noexcept 恢复。
- **Bytes 可移植性/调用约束**：[Overlaps 跨对象指针比较](<docs/review-20261125.md#7-未覆盖与限制>)是原报告「技术性 UB」未修可移植性疑点，不新称已测错误；[Insert 越界仅 assert、Release 追加](<docs/review-20261124.md#61-规则机械项>)当前无调用方。延期：缺实际业务错误；新平台/编译器或新增调用前提级做边界与可移植性核对。
- **SQL/JSON badjson 与保守连接策略**：[历史错误路径](<docs/review-20261124.md#62-错误路径资源>)保留 PickOne/ToDbMap/AppendRowParameters 的 ToJsonText 异常穿透、double 接受 inf 后写 JSON 才抛、prepare/bind 明确业务失败也剔除连接。延期：不可表示 dbjson 或低频错误路径，无当前使用证据；业务接入这些值/出现连接抖动时复核当前实现（不能忽略已落地的单表语句保护），按错误码、非 0 输出不变及诊断合同补定向验证。驱动 C++ throw 覆盖另属近期第 2 组，真实 SQL 错误不等价。
- **其他边界增长**：[Redis.connections 仅下限、PeerNote 按 tag 增长、ServiceTable 动态/拼错名字永久 wanted](<docs/review-20261124.md#61-规则机械项>)。延期：内部测试/固定服务名规模下缺实际影响；接入不可信配置、动态名字或 profile 证实增长时提级，先确认资源预算和清理合同。

## D. 外围工具、规则与检查覆盖

- **13 文件存量格式失败与 AST 缺依赖**：事实边界见 [最新报告](<docs/lasttesting.md#最终排版后复验2026-11-25当前最终源码未全绿>)；2026-11-12 缺 Python clang/libclang，规则自身 9 项通过不能替代实际 AST 扫描。延期：规范化不混近期正确性定位；下次规则批次前持久安装依赖，按 [命名检查入口](<tools/check_field_names.py>)及编译数据库扫描、区分存量，固定 clang-format 18.1.8 并审核批量 diff，不能称全仓格式已过。
- **机械规则**：[历史规则清单](<docs/review-20261124.md#61-规则机械项>)保留 rpc_pending_mgr 三处默认捕获、生成器与三个模块 main 默认捕获、rpc_output_mgr/rpc_service_mgr 的 `||` 多 co_await、timer-before-init 引栈负例。延期：统一规则批次做，相关文件下次改动时优先同步修正（防继续复制），核对真实生命周期/失败路径；生成器要改源再生成，负例不能依赖「一定拒绝」而留下悬空引用。
- **clock 注入有效性**：[历史弱断言](<docs/review-20261125.md#4-新增-p2p3测试有效性>)未核对 so 存在、加载与实际时间偏移。延期：不塞进近期关键假绿批；**下次 clock 专项之前必须先证明注入生效**，失效要失败而非 PASS，不放宽业务断言。
- **wake_bench 恒真与 hold 配置**：[历史弱断言](<docs/review-20261125.md#4-新增-p2p3测试有效性>)的分段求和代数恒真，hold 的 max_clients/idle_ms 为死配置、buckets 接受 0。延期：下次相应基准/边界专项前补真实故障负例与配置生效证明，wake 数字不能继续以恒真断言声称守恒已验证。
- **Web 诊断与弱断言**：[历史证据](<docs/review-20261125.md#4-新增-p2p3测试有效性>)包括 IPv6 环境阻塞只打印、run_web 主断言丢 stdout；[charset 复用同值输出](<docs/review-20261124.md#63-测试覆盖缺口>)使不写输出也能过。延期：未阻挡当前场景；下次 Web 工具批次/IPv6 环境失败提级，用不同哨兵、保留双流证据并把环境阻塞正确记账。
- **覆盖缺口**：[原用例清单](<docs/review-20261124.md#63-测试覆盖缺口>)保留 CallAll 部分失败、UDP 真实丢包/重试、每拨号 DNS 次数（非配置串）、WebTest 响应源取消/释放可观察证据；hold/udp 模式和 RpcRace、fake DB 已入近期，不重复延期。其余在对应能力改动前补旧行为能失败的业务断言，新请求恢复不能替代原请求/旧资源结果。
- **测试错误收尾**：[历史错误路径](<docs/review-20261124.md#62-错误路径资源>)的 LogMgr 持续发送失败永不达标、HoldMgr Pump 异常未 Done、CheckSyncOnLoop 无超时。延期：外围 harness 故障路径，相关异常复现或工具批次提级；须有非 0 失败、原错误原因及安全收尾，不仅超时杀进程。
- **工具/扫描器范围**：[历史工具缺口](<docs/review-20261124.md#63-测试覆盖缺口>)保留 CIM 无创建时间静默跳过、registry proxy.close 覆盖原异常、log-backpressure 复用 logd.out 截断、matrix 只扫默认 VS；[继承/格式漏 tools 自有 C++、sync --require-text 吃前导点](<docs/review-20261124.md#7-已知项状态复核后不变>)。延期：独立工具批次或再次阻挡证据采集时提级；不得降低进程归属安全条件，补隐藏路径/失败清理与自定义安装位置负例。
- **旧文档差异**：[历史文档清单](<docs/review-20261124.md#64-文档>)保留 StartFramework 返回码、UDP Notify 入队/实际 sendto 语义、行号漂移（含接入表/Redis/HTTP）、project 的 XML、已不存在基类、时序图本机字节序、time 续期措辞。延期：专题文档同步时逐项对当前实现核实；hooks 自动恢复口径已归档，不重复列未完成，不因旧报告一句话改写当前合同。
- **规范路由 hook 真实会话验证**：价值是确认必读文档真能拦住未读就改。证据：[hook 实现](<tools/agent_route.py>)与[自测](<tools/test_agent_route.py>) 26 用例都是模拟事件；2026-11-25 真实会话只观察到 subagent 的 `PostToolUse(Read)` 带 `agent_id`（状态目录出现「会话号-agent 号」文件），编辑拦截、subagent 编辑事件的 `agent_id`、`PreToolUse(Agent)` 的 `updatedInput` 改写均未实测。subagent 编辑时合并主会话已读记录是有意放宽，不是缺陷。延期：hook 出错一律放行，失效只是退回自觉先读；下次新开会话即可做。验收：新会话未读 `docs/coroutine.md` 时向 C++ 写 `co_await` 被拒、读后重试放行；派 subagent（原提示词不含「【规范路由】」字样，否则按设计不追加）后其收到的提示词带【规范路由】；主会话未读时，subagent A 读后自己可写、subagent B 仍被拒，确认拦截按 agent 记账。失败按实际事件字段修正并补自测。
- **Linux 侧规范路由检查被跳过**：价值是让 Linux 矩阵这两步真正执行，而不是空 PASS。证据：WSL `python3` 为 3.8.10 且无 tomli，[矩阵](<tools/test_matrix.py>)的 `agent-route-tests`、`agent-route` 打印「跳过」并返回 0。延期：索引检查与平台无关，Windows 侧 Python 3.13 可执行（加入矩阵后尚未跑过全量）。最小方向：WSL（及远程 Linux）装 Python 3.11+ 或 tomli，装在持久位置，不改脚本的跳过口径。验收：Linux 矩阵两步日志无「跳过」、自测 26 用例实际执行；临时改坏索引或 AGENTS.md 表时 `agent-route` 返回非零。
- **四路回归 subagent 定义（可选）**：价值是派四路时不用每次手工替换范本占位符。证据：[测试规程](<docs/testing.md>)第 7 节范本目前只能复制粘贴。延期：现有范本可用，仅为便利。最小方向：做成 `.claude/agents/regression-lane.md` 一类定义，只引用第 7 节、不复制规则，Codex/Cursor 仍按范本；决定做时先确认不与「杂活交给 subagent」的模型约定冲突。验收：用它派一路，清理、运行、回报与范本一致，偏移和路名不串。

## E. 性能与未来优化

- **模板展开/编译折中**：生成器目前直线调用仍落到递归/SFINAE 模板，见 [序列化生成源](<cbaselib/proto/proto_tool/serialize_data_file_gen.py>)、[字节流](<cbaselib/base/inc/oi_bytes_stream.h>)、[JSON](<cbaselib/proto/inc/oi_json.h>)、[DB 字段表](<cbaselib/db/inc/oi_db_row.h>)。延期：无真实协议规模下编译瓶颈证据；有 profile 再按具体标量/容器展开、普通重载减嵌套，生成源与头同步，不手改生成头；字节、JSON、列/主键/dbtype/dbjson/NULL/按成员取列保持不变。[历史分发测量](<docs/pb-dispatch.md>)中宏编译快、switch 解码约快 1µs但编译约慢至 6 倍，map 无优势；保留用户选择，不作当前性能承诺。
- **多线程协程批量创建/入队偏慢**：2026-11-14 WSL 历史 live ST≈69ns/条、MT 3 工人≈4339ns/条（约 60 倍），churn ST≈1299 万/秒、MT≈373 万/秒；后续异机/平台数字见 [历史专项](<docs/lasttesting.md#协程>)，不能混成同次测量。延期：接受当前架构，真实业务 profile 证实共享队列/挂起登记锁竞争瓶颈才考虑批量/分片，不预设无锁或重做调度；保持单线程与任务语义并用有效基准比较。
- **Blocking 唤醒开销**：用户已接受现有开销、保留 poll，见 [接受策略](<docs/fixed.md#已接受策略blocking-暂不优化>)。历史 Linux 空回调整链约 13.5–13.8µs、原生对照约 8.5–8.6µs；Windows 约 35.8–61.0µs 与 1.06–1.11µs；起止点/同步不同，不能差值归因。只在真实阻塞业务链证明瓶颈后再分段调研，不称已优化，不用旧数作当前承诺。
- **redisserver 首参仍复制**：[ChangesConnection](<modules/redisserver/src/redis_server_session.cpp#L115-L129>)仍构造 string，完整扫描后 ProcessInput 不消除前面的复制；旧报告「已不成立」不能关闭。当前默认接收限额限制规模，内部测试进程/可信客户端，未做大输入实测；延期：不为测试进程额外拷贝先优化，真实规模/profile 证明瓶颈再评估 view 生命周期。
- **HTTP trailer 重扫/请求源结束后的 2ms 泵**：[历史性能路径](<docs/review-20261124.md#62-错误路径资源>)分别可能重扫成 O(n²)、慢响应期间纯唤醒。延期：先有实际流量开销数据；受控慢分片/长轮询量化 poll CPU 后最小记忆扫描或停止空泵，不先改解析/生命周期。

## F. 其他错误路径与诊断差异

> 下列均保留为未完成；当前无对应实际使用/复现证据，随该能力真实接入或复现提级，先检查当前代码和业务结果，不从历史行号推导新根因。

- **HTTP 拒绝/额度合同**：[历史错误路径](<docs/review-20261124.md#62-错误路径资源>)含服务端缓冲响应超发送额度时 handler 无感断连、客户端大请求整包 BUSY；[已知边界](<docs/review-20261124.md#7-已知项状态复核后不变>)含极小 chunk wire 计入 8MiB、413 后 RST。触发实际大响应/上传或小 chunk 贴限额后，真实 FIN/RST 核对调用码/收到字节和诊断，再选流式/拒绝合同，不称已修。
- **UDP 错误与绑定口释放**：[历史代码链](<docs/review-20261124.md#62-错误路径资源>)为入队已成功但 sendto 硬错误仅日志丢包、绑定口无解绑释放；[已定空闲回收丢未发包策略](<docs/fixed.md#2026-11-21-已定策略>)不等于此处全修。实际送达合同/同进程重绑需求出现再提级，验收原包结果、错误码/诊断、端口释放，不能默认 UDP 入队即送达。
- **旧会话迟到析构通知与 TLS 线程约束**：[历史已知项](<docs/review-20261124.md#7-已知项状态复核后不变>)中的 OnClientDisconnect/OnPeerDisconnect 仍须业务比对 sid；TLS SendResult/pending 仅 poll 线程路径当前被使用。应用受迟到通知影响或要跨线程发送时，先复现接管/线程交错并定身份/线程合同，不能仅因现用法安全就关闭。
- **StopAccept / PauseEvents**：[历史边界](<docs/review-20261124.md#7-已知项状态复核后不变>)保留 Publish 失败后立即重试时监听 fd 尚未真关、测试 UDP 暂停恢复覆盖期间新增事件位。延期：当前窄触发；出现同进程立即重绑/真实暂停用途后，按 fd 最终归属、事件位合并及唤醒验收，不把已完成 Publish 回滚登记误作 fd 立即释放已证。
- **registry/redisserver 诊断和兼容**：[历史错误路径](<docs/review-20261124.md#62-错误路径资源>)保留一条坏静态行放弃整表且每秒重试、纯选路非法 Registry 配置呈 3 秒 TIMEOUT、redisserver 大回包丢弃断连；[RESP NIL 重写](<docs/review-20261125.md#3-新增-p2框架--协议>)中 `*-1`→`$-1` 对 hiredis 下游等价但严格 RESP2 下游不同。配置运维问题、真实大回包或非 hiredis 下游接入后提级，核对原错误原因、大小拒绝/类型合同；其中 OOM C 回调风险见 C，不漏记。
- **停止后错误码**：[线程池入口](<cbaselib/base/src/oi_thread.cpp>)的 AddStopHandler 在停止后返回 CONNECT 语义不准。延期：诊断差异，相关接口维护时对齐错误码或明确注释，并验证停止后拒绝及调用方分支，不无故破坏兼容。
