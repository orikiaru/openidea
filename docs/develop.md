# 开发标准

写业务、改框架之前先对上这里，再读对应的专题文档。拿不准的设计先问，不自己拍板；调接口前先查头文件和文档，不臆造接口。

## 1. 分层

类怎么继承、谁持有谁，见 [框架类图](class-diagram.md)。

- Channel 是读写层：tcp、udp、unix、fifo 只管收发字节。Session 是协议层：拆包、反序列化、序列化。业务代码不碰裸包，不自己拆包头，不自己 `recvfrom` / `sendto`。
- 会话管理器（`ProtoSessionManager`、`ProtoClientManager` 组件）只管会话和协议转发。流程是：协议进来，转给逻辑 mgr，mgr 处理，填回包并返回错误码。
- 逻辑 mgr 管一类实体的集合：加载、缓存、保存、读配置。实体自己的操作写在实体类里。例：`RoomServer` 把协议转给 `RoomMgr`，`RoomMgr` 取出 `Room`，具体操作在 `Room` 里。
- 读改存写成 Load、Handle、Save 三步，不拆成一堆在外面填结构的辅助函数。输出参数直接传进去让被调方填。
- 配置在 mgr 的 `Init` 里读一次，存成成员。实体处理时由 mgr 按参数传进去，实体不自己读配置。
- 要换会话类型就重写 `CloneSession`，不在通道外面挂一层自己的解析。
- 框架缺能力就在 cbaselib 里补，补齐回归、跑全量，不在业务里绕。例：UDP 用 `type = "udp"` 的 `UDP_Channel`，见 [基础库](cbaselib.md) 的 net 段；会话上的单向发送用 `Notify(sid)` 或 `RpcContext::Notify`，按服务名和多连接用 `RouteManager`、`ProtoMultiClientManager` 的 `Notify`，见 [协程](coroutine.md) 的单向消息。`RpcNotify` 是按 sid 发送的底层函数。处理函数登记在接收该连接的管理器上：自己的 `Init` 里 `RegisterRpcHandler(&类::成员)`，别处写 `管理器.RegisterRpcHandler(&类::成员)`。
- 处理函数按原连接回调时用 `stContext.Call(请求, 回复, 时长)`；没有入站上下文时可用 RPC 协议管理器的 `Call(sid, 请求, 回复, 时长)`。独立后台任务按值捕获上下文，但不延长连接或管理器寿命，断线后调用仍失败。直连客户端用自身的 `Call(请求, 回复, 时长)`，按服务名或实例调用仍用对应入口。多实例管理器的实例号与 sid 不可混用；底层 `RpcCall` 保留兼容及专项用途。

## 2. 模块和代码组织

- 一个 module 对应一个进程，业务代码都放在 `modules/<进程>/inc`、`src` 里。几个进程共用的东西放 `modules/include`，或者单独建目录，不放仓库根下的 `src`。
- 基础库用 `framework.cmake`。本仓库根 `CMakeLists.txt` 和外部工程都 include 它，不在业务工程里再列一遍库。进程模块仍由调用方自己的 `module.proto` 生成后加入，见 [模块](module.md)。按功能接到具体接口见 [基础库](cbaselib.md) 的接入表。
- 进程及其 server/client 组件在 `protoconfig/module.proto` 里声明，由 `proto_tool` 生成骨架，见 [模块](module.md)。业务文件保留已有内容；组件启动和构建清单由生成器更新。
- 启动时可用 `--config <路径>` 指定配置文件，例如 `ipcserver --config "../config/my server.toml"`。相对路径以进程当前工作目录为准；未指定时仍使用各进程原来的默认路径。ipcserver 保留旧的首位置路径参数，同时指定时 `--config` 优先。路径缺失、为空、重复指定或下一项是 `--` 开头的选项会被拒绝；ipcclient 和 logd 的原有探针可放在配置参数前或后。
- `StartFramework` 成功后，可将返回 `int32_t` 的模块初始化回调传给 `RunFramework(fnInit)`。回调返回 0 才进入主循环；返回非 0 时先停止并等待 poll 线程，再返回原错误码；抛异常时记录原因、停止 poll 并返回 `OI_ERR_FAIL`。无参数的 `RunFramework()` 仍可用于已完成初始化的调用方。
- 不写类外的 C 风格函数，不用全局状态。对外可调用能力按职责放类成员；确需文件内自由辅助函数时用 `.cpp` 匿名名字空间或内部链接，公共头的 `inline` 或 `Detail` 名字空间不会限制外部调用。处理函数是组件的成员，在这个管理器上 `RegisterRpcHandler(&类::成员)` / `RegisterNotifyHandler(&类::成员)` 注册，不用友元把类外函数放进类里。请求处理表是 `ProtoSessionManager` 的私有锁和 map，按管理器各存一份，对外是 `BindRpcHandler` / `FindRpcHandler`。成员函数所属的类必须继承 `ProtoSessionManager`；登记时当前管理器必须是该类本身或其派生，否则抛 `std::logic_error`。
- 状态放成员，不用静态变量，也不堆临时变量。
- 不在会话或 mgr 里随手 `StartTask`。任务由框架入口拉起（`RegisterRpcHandler`、`RegisterNotifyHandler`、测试 mgr 的 `Start`），见 [协程](coroutine.md)。

## 3. 协议

- 协议相关的数据结构一律写成 `.proto` 里的 rpcdata，用生成的 `oi_rpcdata_*.h` 做序列化。不手写结构体，也不手写序列化。Redis 里存的值也直接用 rpcdata。业务 RPC 在 `rpc.proto`，JSON 样例在 `json.proto`，数据库行在 `db.proto`。
- 每条 rpc 都要写 request 和 response。调用号为 0 的单向消息不回 response；`NotifyPeer` / `NotifyAny` 的 RPC 包装使用非零调用号，成功时会收到默认构造的 response。
- 常量和错误码用 `rpc.proto` 的 `enum`、`errordef`，生成普通 C 枚举，不参与序列化，字段仍用定宽整数，见 [模块](module.md) 的枚举和错误码。协议暂时不支持的能力记进 `todo.md`，不另起一套。

## 4. 错误码

- 0 是成功。框架错误码在 `oi_errno.h`，是负数；业务错误码写成 `rpc.proto` 的 `errordef`，是正数。处理函数 `co_return` 错误码，回包里不再另带一个 `iCode`，也不在业务代码里另外定义错误码。

## 5. 时间、定时和过期

- 用框架的时间：秒级用 `GetTime()`，要 tick 时间用 `BaseTimerManager` 的 `GetCurTickTime()`。不自己调 `std::chrono` 计时。秒够用就不上毫秒。
- 定时用 `AddTimer`，或者在协程里 `co_await OI::Coro::yield(毫秒)`，见 [定时器](timer.md)。不写 sleep，也不自己维护计时。
- 过期逻辑：每个对象记下自己上次活跃的时间戳，在现有入口（例如心跳）里顺手清理，不另起扫描任务。
- 只做需求要的那一步。缓存就是 Load 时先查内存、Save 时写回，不叠一致性方案；没人要的精度和兜底不加。

## 6. 线程模型

- 按负载选：逻辑简单的转发服务端用单线程；要接 Windows UI 的客户端用多线程。在模块的 CMake 里用 `_SINGLE_THREAD_` 切换，见 [基础库](cbaselib.md)。

流式 source/sink 回调不得阻塞 poll 或保存借用的 span/视图；返回 `OI_ERR_BUSY` 表示当前片未消费，重试前不得产生消费副作用。验证流取消与释放的回归须有旧状态取消并释放的可观察证据；新请求健康另行验证。普通协程 handler 可在客户端断开后完成并丢弃响应。

## 7. 测试

- 框架的新能力优先扩展已有 `ipcclient` / `ipcserver` 测试主题及开关，复用相应用例的配置、启动与断言；仅当现有场景不能表达隔离需求时再新增测试主题或用例。独立用例在 `tools/regression/cases/<组>.py` 定义并按现有注册方式登记，核对 `--case all` 的实际包含关系，同时更新 [测试规程](testing.md) 的覆盖与端口。入口仍是 `python tools/run_regressions.py`。例：`udp` 对应 `TestUdp` 和 `IpcUdp`。
- 测试自己起 Redis 等依赖的临时实例，不借用户自己的服务，也不让用户去起。
- 全量是 Windows、Linux 各跑多线程和单线程，按 [测试规程](testing.md) 执行，编译和矩阵交给 subagent（见 `AGENTS.md`）。应用仓库另跑自己的用例，例如 lanroom 的 `python test\run_cases.py`。
- 改完代码在本机编一次，让 clangd 用的编译数据库是新的，跳转才对。Ninja/Makefiles 使用对应构建目录的 `compile_commands.json`；Visual Studio 构建 `oi_clangd_db` 目标，在该构建目录的 `clangd` 子目录生成数据库。给编辑器的 clangd 启动参数配置 `--compile-commands-dir=<数据库所在目录>`，Windows 例如 `--compile-commands-dir=E:/openidea/build-win/clangd`，Linux 例如 `--compile-commands-dir=/home/用户名/openidea-build-gcc13`；按实际路径替换。配置构建不会改写源码目录的 `.clangd`。
- 报告里分清实际通过、失败和环境阻塞。不删断言、不放宽超时、不靠重试来掩盖失败。

## 8. 子模块、提交和待办

- openidea 作为子模块时，应用侧的改动不进子模块。确实要改框架时先说明，在子模块里单独提交，父仓库再更新子模块指针。
- 「提交」包含 commit 和推送当前分支，见 `AGENTS.md`。不改 git 配置，不强推。
- 没做完的事和以后要加的能力写进 `todo.md`，写清需求、现状和大致方向。
