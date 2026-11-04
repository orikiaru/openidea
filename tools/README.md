# 回归与故障测试

**统一操作规程：[docs/testing.md](../docs/testing.md)。跨平台矩阵入口：`python tools/test_matrix.py --platform both --redis-wsl`（Windows 宿主）。**

下文为单项测试的补充说明，平台环境、完整用例清单和结果判定以统一规程为准。

在 WSL 中执行，复用 `ipcclient` 的四个 Mgr。每个用例创建自己的临时配置和日志目录，退出时清理本次进程和目录。已有端口被占用时直接报错，不终止其他进程。日志超过 2GiB 立即失败，Redis 内存上限为 1536MiB。

```sh
cmake -S /mnt/e/openidea -B ~/openidea-build-gcc13 -DOI_IPC_SINGLE_THREAD=OFF
cmake --build ~/openidea-build-gcc13 -j8
python3 /mnt/e/openidea/tools/run_regressions.py --case all
```

把 `OI_IPC_SINGLE_THREAD` 改成 `ON`，重新编译并执行相同用例，即为单线程客户端和回声服务端。日志服固定多线程，Redis 代理固定单线程。测完恢复为 `OFF` 并重新编译。

`all` 包含四个常规测试以及 Redis 队列限流、RST 断连、磁盘写满故障测试。也可以用 `--case echo|log|redis|redis-limit|hold|reset|disk-full` 单独执行。Redis 完整性测试还覆盖 poll 线程同步调用的拒绝、连接状态命令的拒绝和断开后的重连。

校时故障使用进程内拦截，不需要 root，也不改变机器时间。启动两秒后模拟墙上时间前进一天或回拨一小时，仍通过 HoldMgr 核对延时、RPC 和 Redis 的超时与迟到回包。

```sh
gcc-13 -shared -fPIC /mnt/e/openidea/tools/clock_fault.c -o /tmp/oi-clock-fault.so
python3 /mnt/e/openidea/tools/run_regressions.py --case clock-forward --clock-fault-so /tmp/oi-clock-fault.so
python3 /mnt/e/openidea/tools/run_regressions.py --case clock-back --clock-fault-so /tmp/oi-clock-fault.so
```

Redis 默认程序位置为 `/tmp/redis-7.4.12/src/redis-server`，可通过 `--redis-binary` 指定。`--bin-dir` 可指定另一套已编译的模块。脚本失败会输出本次进程的末尾日志；所有测试进程都在 finally 中清理。

磁盘故障用 `/dev/full` 替换临时日志文件。判据是日志服非零退出并在 stderr 明确报告写入或刷新失败。该策略是失败即停止，不代表未落盘数据已经持久化，也不做自动重放。

HoldMgr 同时检查临时带捕获协程 lambda 在让出后恢复、RPC 错误来源不消费等待、毫秒时长和直接返回响应、真实断连及时结束等待。RedisMgr 检查直接返回共享回执、参数/服务端/断连错误分类；`redis-limit` 临时将等待上限设为 1，验证 Busy 拒绝后不会串包，排队完成后可以继续 PING。

Hold 回归将临时配置的普通任务队列限额设为 1，验证批量定时唤醒不丢失，同时覆盖任务作用域停止/等待、异常传递、待连接 RPC 条数和字节限额及取消后的容量恢复。所有服务退出超过 3 秒需要强杀即判回归失败。

Hold 还注入序列化异常、排队/发送异常以及完成回调竞争，核对 513 次异常均返回调用方且等待表归零；再保留 64 条长超时请求、拒绝 10 万条请求，验证超时记录只保留这 64 条，断连清理后等待项、超时记录和空桶均恢复基线。

Hold 的会话索引回归登记 4096 个等待项，换绑其中一部分后依次断开旧、新会话，检查其他会话不受影响，最后调用表、超时索引和会话索引都恢复基线。

`--case redis-queue-timeout` 在现有 RedisMgr 中通过 `[Regression] redis_queue_timeout=true` 启用：普通队列线程同步调用阻塞命令，100ms 的请求预算必须在 1 秒内按 Timeout 返回，且迟到回执的配对位置仍保留。`--case rpc-ingress` 先注入超大包头、残缺包头/正文、滴流、64KiB 边界分片和计时器取消/重启，再由 EchoMgr 跑百万次回声。两项均包含在 `all`，配置只写入临时目录。

调度与发送回归：Hold 检查 64 个持续就绪的协程不会饿死普通任务，发送额度跨会话/通道缓冲计数，工作线程主动断连，以及自动推断响应类型/会话号的 RPC。Redis 检查两种线程模式都拒绝在真实 libuv 定时回调中同步调用；Hold 的迟到 BLPOP 使用同步接口，要求按配置超时后仍能正确配对下一条 PING。日志测试临时把 LogClient 待发额度降到 256KiB，再核对完整 1GiB 落盘，覆盖队列满后的重试和发送计数。

Hold 同时验证连接前原始发送缓存不得超过连接的发送额度，建连后缓存完整进入会话发送队列，不能被新数据挤掉。

标准 RPC 回执回归在 Hold 测试内：成功响应、业务错误 1001 的无响应体回执、GetRet/GetRes、错误访问保护、无 sid 输出引用重载、失败保留原响应，以及缺状态码/截断响应；原超时、迟到回包和断连用例继续共用相同调用路径。

服务端回调只接收请求和响应（可选上下文）：回声用同步 int32_t 返回，Hold 用 RpcTask 的 co_return 返回；Hold 另验证带返回值任务挂起后取值与异常传播。

命名回调注册使用 OI_RPC_SYNC_HANDLER / OI_RPC_HANDLER 声明和 OI_RPC_BIND 绑定，真实回声与 Hold 网络回归覆盖宏展开路径；可选上下文跨挂起在 Hold 内检查。

字段命名改造后，`--case all` 还包含 `registry` 和 `web`：前者启动独立、限制 1536MiB 的 Redis 与注册服务，将客户端直连后备端口故意改错，验证服务发现和百万次回声；后者使用现有 webserver/webclient 验证 HTTP 查询、JSON、生成协议和二进制 body。均使用临时配置、自动停进程并清理目录。

协议字段规则检查：`python3 tools/test_field_names.py`。C++ 类成员、普通函数参数和局部变量命名检查使用 libclang，先在独立 Python 虚拟环境 `pip install libclang`，再运行 `python tools/check_field_names.py --build-dir ~/openidea-build-gcc13`。扫描编译数据库中的自有 C++ 单元，包含单、多线程库；第三方和系统兼容头排除。编译错误也会导致检查失败，不能带错跳过。

类的私有成员使用 `m_` + 类型前缀，普通参数使用类型前缀和 `Param` 后缀，局部变量按类型命名并避免遮蔽成员。结构体成员保持现状；生成协议代码的参数和局部变量不参与改名，第三方代码排除。

Hold 内置 JSON 边界回归（`hold json validation bad=0`）：覆盖有符号/无符号 8/16/32/64 位上下界、溢出和类型拒绝，长整数键、非法键、容器失败保留原值、生成对象失败回滚与缺失字段保持。命名检查测试覆盖带属性的 class/struct 模板；扫描在启用断言时进行。

客户端测试按进程只启动一次，服务发现切换或重连不得重复登记测试等待。回声回归同时核对启动日志仅出现一次。

Hold 新增普通任务异常释放/继续执行、服务端半包编码异常转标准错误回执并继续下一次 RPC、HTTP 真实 TCP 分帧回归。脚本仅在临时配置注入 `[Regression] encoding_failure=true` 与 `http_port`，原始 TCP 夹具和测试进程一同清理；可使用 `--case hold` 单独复测，单、多线程均需通过。
