# 回归与故障测试

在 WSL 中执行，复用 `ipcclient` 的四个 Mgr。每个用例创建自己的临时配置和日志目录，退出时清理本次进程和目录。已有端口被占用时直接报错，不终止其他进程。日志超过 2GiB 立即失败，Redis 内存上限为 1536MiB。

```sh
cmake -S /mnt/e/openidea -B ~/openidea-build-gcc13 -DOI_IPC_SINGLE_THREAD=OFF
cmake --build ~/openidea-build-gcc13 --target ipcclient ipcserver logd redisserver -j8
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
