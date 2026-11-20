# Windows 单线程 admission 崩溃记录（2026-11-21）

**原因尚未确认。** 当时的 Windows x64 Release 单线程增量产物在 `rpc-admission-count` 和 `rpc-admission-bytes` 中均以 `0xC0000005` 退出。两个原始定向复跑均再次失败；在 `count` 的独立调试复跑中，cdb 捕获到一次向地址 `0x1` 写入的访问异常。现有证据不能确定哪个对象或哪段源码导致该写入，也不能证明增量编译、ABI、UDP 或其他某项修改是根因。

## 触发和保留的用例

两项都在现有 `--case all` 中，分别以临时配置限制 RPC 在途数量或字节；`count` 的总数/单会话数上限为 2/1，`bytes` 的总字节/单会话字节上限为 64/32。每项要求三个客户端各发两次调用，三轮均为 2 次成功、4 次 BUSY、无其他失败，并检查完成标记；字节用例还检查超大请求拒绝及后续恢复。原始失败中的客户端退出码为 3221225477（`0xC0000005`），客户端捕获输出为空；服务端已经监听并记录两个会话。仅凭这些输出不能判断失败发生在哪一条调用或哪个初始化步骤。

旧产物复跑使用当时的独立 `windows-st` 副本及其 `build-win-st` 的 Release 产物，先后分别运行以下命令；每次前后用项目归属清理工具清理该路，Redis 路径与结果目录按环境替换：

```powershell
python tools/kill_test_procs.py --lane windows-st
# 在 windows-st 副本根目录运行，分别替换 <case> 为两个 admission 用例名。
python tools/run_regressions.py --case <case> --port-offset 1000 --bin-dir F:/oi-par/windows-st/deploy/bin/Release --redis-binary <Windows原生Redis> --artifacts <结果目录>
python tools/kill_test_procs.py --lane windows-st
```

两项用例及各轮断言仍保留在 `tools/run_regressions.py` 和 `RpcAdmissionMgr`；不以本记录替代回归。上述目录后来已同步新源码并重建，直接再次执行会测试当前产物，不能当成旧二进制复现。

## 调试器原始证据

旧 `count` 复跑让 cdb 从进程启动时运行，使用 `sxe av;g;.exr -1;kv;q`。保存输出给出的异常为：

```text
ExceptionCode: c0000005 (Access violation)
ExceptionFlags: 00000000
Parameter[0]: 0000000000000001
Parameter[1]: 0000000000000001
Attempt to write to address 0000000000000001

ipcclient+0x15d0f2
ipcclient+0x15cf1a
ipcclient+0xe5159
ipcclient+0x1843a4
ipcclient+0x1946eb
ipcclient+0x1869fe
ipcclient+0x21af
ipcclient+0x1cb0a8
KERNEL32!BaseThreadInitThunk+0x1d
ntdll!RtlUserThreadStart+0x28
```

这是首次机会异常的原始模块偏移，第一帧地址在该次运行中为 `00007ff6253fd0f2`。原 Release 目录未找到匹配 PDB；cdb 同时提示无法验证临时副本的校验和。没有记录该出错二进制的哈希，也没有找到对应 dump；临时进程目录和原增量输出已被后续构建覆盖。此前另存的 `udp-old-binaries` 来自更早的 UDP 调查，不能认作该 admission 出错二进制。因此不能可靠地把这些偏移套到后来重编的可执行文件或 PDB 上，也不能从偏移推断函数或行号。cdb 输出中的 C++ 首次机会异常本身不足以证明它们是崩溃原因。

## 重编及最终对照

同一个旧源码副本另用 `/MD /O2 /Ob2 /DNDEBUG /Zi` 与 `/DEBUG:FULL` 干净重编；与旧矩阵一致启用 `OI_ENABLE_TEST_HOOKS=ON` 后，cdb 下 `count` 通过，非调试 `bytes` 通过，均完成三轮预期结果。干净重编和调试会改变二进制布局与时序；增量产物或 ABI 不一致只是待查方向，这些对照不足以证明它是原因，也不足以给旧异常符号化。

最终冻结工作区的 Windows 单线程完整矩阵两项均通过，三轮均为 2 次成功、4 次 BUSY。最终结果只能证明该快照和这些断言成立，不能反推旧异常的根因或证明所有时序均无问题。

本机保留的文本证据位于 `%USERPROFILE%/openidea-test-results/`：

- `20261121-udp-queue-final-windows-st/windows-st/st-tests.log` 和 `st/results.json`：旧完整矩阵两项失败。
- `20261121-admission-old-count/`、`20261121-admission-old-bytes/`：旧增量产物的独立复跑。
- `20261121-admission-old-debug/rpc-admission-count/ipcclient.out`：上述 cdb 异常参数与调用栈。
- `20261121-admission-old-hooks-symbol/`、`20261121-admission-old-hooks-symbol-bytes/`：带符号、启用测试钩子的重编对照。
- `20261121-fifo-final-windows-st/windows-st/st-tests.log` 和 `st/results.json`：最终单线程完整矩阵对照。

旧完整矩阵的 `environment.json` 记录 HEAD `cd33b7f8eb8907d00377f7be8717c01227851597` 和 tracked diff SHA-256 `c963d955fe0cc74a40f64a878d29ff2c6c87cd296bf59b9bb96f6989aafee1a7`；最终矩阵的对应 tracked diff SHA-256 是 `4541d9db5bac78aa60913f74dce93a55b3693554bb6f9258566cf1adc65f576f`。这些摘要不等于二进制哈希，也不覆盖未跟踪文件内容。
