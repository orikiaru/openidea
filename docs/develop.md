# 开发标准

写业务、改框架之前先对上这里，再读对应的专题文档。拿不准的设计先问，不自己拍板；调接口前先查头文件和文档，不臆造接口。

## 1. 分层

- Channel 是读写层：tcp、udp、unix、fifo 只管收发字节。Session 是协议层：拆包、反序列化、序列化。业务代码不碰裸包，不自己拆包头，不自己 `recvfrom` / `sendto`。
- 会话管理器（`ProtoSessionManager`、`ProtoClientManager` 组件）只管会话和协议转发。流程是：协议进来，转给逻辑 mgr，mgr 处理，填回包并返回错误码。
- 逻辑 mgr 管一类实体的集合：加载、缓存、保存、读配置。实体自己的操作写在实体类里。例：`RoomServer` 把协议转给 `RoomMgr`，`RoomMgr` 取出 `Room`，具体操作在 `Room` 里。
- 读改存写成 Load、Handle、Save 三步，不拆成一堆在外面填结构的辅助函数。输出参数直接传进去让被调方填。
- 配置在 mgr 的 `Init` 里读一次，存成成员。实体处理时由 mgr 按参数传进去，实体不自己读配置。
- 要换会话类型就重写 `CloneSession`，不在通道外面挂一层自己的解析。
- 框架缺能力就在 cbaselib 里补，补齐回归、跑全量，不在业务里绕。例：UDP 用 `type = "udp"` 的 `UDP_Channel`，见 [基础库](cbaselib.md) 的 net 段；单向消息用 `RpcNotify` 和 `OI_NOTIFY_BIND`，见 [协程](coroutine.md) 的单向消息；成员函数注册用 `OI_RPC_BIND_SELF`。

## 2. 模块和代码组织

- 一个 module 对应一个进程，业务代码都放在 `modules/<进程>/inc`、`src` 里。几个进程共用的东西放 `modules/include`，或者单独建目录，不放仓库根下的 `src`。
- 基础库用 `framework.cmake`。本仓库根 `CMakeLists.txt` 和外部工程都 include 它，不在业务工程里再列一遍库。进程模块仍由调用方自己的 `module.xml` 生成后加入，见 [模块](module.md)。按功能接到具体接口见 [基础库](cbaselib.md) 的接入表。
- 进程和组件在 `xmlconfig/module.xml`、`module_component.xml` 里声明，由 `proto_tool` 生成骨架，见 [模块](module.md)。生成器只在文件不存在时才写，生成出来以后就在上面改。
- 不写类外的 C 风格函数，不用全局状态。处理函数是组件的成员，用 `OI_RPC_BIND_SELF` / `OI_NOTIFY_BIND_SELF` 注册，不用友元把类外函数放进类里。
- 状态放成员，不用静态变量，也不堆临时变量。
- 不在会话或 mgr 里随手 `StartTask`。任务由框架入口拉起（`HandleRpc`、`HandleNotify`、测试 mgr 的 `Start`），见 [协程](coroutine.md)。

## 3. 协议

- 协议相关的数据结构一律写成 `proto.xml` 里的 rpcdata，用生成的 `oi_rpcdata_*.h` 做序列化。不手写结构体，也不手写序列化。Redis 里存的值也直接用 rpcdata。
- 每条 rpc 都要写 request 和 response。单向消息也要声明一个 response 占位，线上不会出现。
- 常量和错误码用 `proto.xml` 的 `enum`、`errordef`，生成普通 C 枚举，不参与序列化，字段仍用定宽整数，见 [模块](module.md) 的枚举和错误码。协议暂时不支持的能力记进 `todo.md`，不另起一套。

## 4. 错误码

- 0 是成功。框架错误码在 `oi_errno.h`，是负数；业务错误码写成 `proto.xml` 的 `errordef`，是正数。处理函数 `co_return` 错误码，回包里不再另带一个 `iCode`，也不在业务代码里另外定义错误码。

## 5. 时间、定时和过期

- 用框架的时间：秒级用 `GetTime()`，要 tick 时间用 `BaseTimerManager` 的 `GetCurTickTime()`。不自己调 `std::chrono` 计时。秒够用就不上毫秒。
- 定时用 `AddTimer`，或者在协程里 `co_await OI::Coro::yield(毫秒)`，见 [定时器](timer.md)。不写 sleep，也不自己维护计时。
- 过期逻辑：每个对象记下自己上次活跃的时间戳，在现有入口（例如心跳）里顺手清理，不另起扫描任务。
- 只做需求要的那一步。缓存就是 Load 时先查内存、Save 时写回，不叠一致性方案；没人要的精度和兜底不加。

## 6. 线程模型

- 按负载选：逻辑简单的转发服务端用单线程；要接 Windows UI 的客户端用多线程。在模块的 CMake 里用 `_SINGLE_THREAD_` 切换，见 [基础库](cbaselib.md)。

## 7. 测试

- 框架的新能力要在 `ipcclient` / `ipcserver` 里加回归：一个 Mgr，一个 `[Test]` 开关，`run_regressions.py` 里加用例和标记断言，并收进 `--case all`，同步改 [测试规程](testing.md) 里的用例数和端口。例：`udp` 用例对应 `UdpMgr` 和 `IpcUdp`。
- 测试自己起 Redis 等依赖的临时实例，不借用户自己的服务，也不让用户去起。
- 全量是 Windows、Linux 各跑多线程和单线程，按 [测试规程](testing.md) 执行，编译和矩阵交给 subagent（见 `AGENTS.md`）。应用仓库另跑自己的用例，例如 lanroom 的 `python test\run_cases.py`。
- 改完代码在本机编一次，让 clangd 用的编译数据库是新的，跳转才对。
- 报告里分清实际通过、失败和环境阻塞。不删断言、不放宽超时、不靠重试来掩盖失败。

## 8. 子模块、提交和待办

- openidea 作为子模块时，应用侧的改动不进子模块。确实要改框架时先说明，在子模块里单独提交，父仓库再更新子模块指针。
- 「提交」包含 commit 和推送当前分支，见 `AGENTS.md`。不改 git 配置，不强推。
- 没做完的事和以后要加的能力写进 `todo.md`，写清需求、现状和大致方向。
