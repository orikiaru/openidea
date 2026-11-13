---
name: linux-remote-test
description: >-
  在 Windows 宿主上通过公钥 SSH，到本机 .local/linux-hosts.json 记录的 Linux 机器编译并跑 openidea 回归。
  用户说远程 Linux 测试、Linux 全量、4 核多线程、2 核单线程、远程 test_matrix 时使用。
  地址不进仓库；记录缺 IP 或用户名时先问用户，用户不提供则改用 WSL。
---

# Linux 远程测试

矩阵语义、用例和通过条件以 `docs/testing.md` 为准。本技能只规定怎样从 Windows 登到远程机器并跑完，不另写客户端，不改断言。

编译和回归交给 subagent。Cursor 用 `grok-4.7-high`。subagent 只跑测试和回报，不改仓库，不提交。

## 选机器

读仓库根目录 `.local/linux-hosts.json`。这个文件已在 `.gitignore`，不要提交，不要把 IP、用户名、密码写进仓库、命令历史以外的文档，或 subagent 回报里的可入库文件。

文件不存在，或某台机器缺 `ip`、`user` 时，先问用户。用户不提供，就改用 WSL（`docs/testing.md` 第 3 节），之后一直用 WSL，直到用户再次明确说用远程机器。

`hosts[]` 里 `role` 为 `mt` 的机器编译 Release 并跑多线程，`jobs` 用记录值。`role` 为 `st` 的机器固定只跑单线程，矩阵加 `--no-restore`，测完不编回多线程，构建目录一直是单线程。一台机器不要同时跑两种模式。两台机器可以同时各跑自己的模式，但不要共用构建目录。

可选字段有则用，没有就按下面探测，不要猜另一台机器的路径：`identity`、`cmake`、`redis`、`source`、`build`。默认私钥是 `%USERPROFILE%\.ssh\id_rsa`，源码 `$HOME/openidea`，构建目录 `$HOME/openidea-build-gcc13`。

## 登录

只走公钥。SSH 参数固定：

```text
ssh -i <identity> -o IdentitiesOnly=yes -o BatchMode=yes -o PreferredAuthentications=publickey -o ConnectTimeout=15 <user>@<ip>
```

`BatchMode` 失败就停，告诉用户公钥没登上。不要改用密码，不要把密码写进命令。

在 PowerShell 里发远程脚本：用单引号 here-string，去掉 CR，管道给 `ssh <user>@<ip> "bash -s"`。不要用 `&&`，不要用 bash heredoc，远程脚本里的 `$` 不要让 PowerShell 展开。

## 同步源码

**只准用**仓库根下的标准入口，禁止手写 `tar`/`scp`/PowerShell 管道解包：

```powershell
python tools/sync_linux_remote.py --role mt
python tools/sync_linux_remote.py --role st
python tools/sync_linux_remote.py --role all
```

可选：`--require-text modules/ipcclient/src/coro_mgr.cpp:live_ns=` 同步后校验远端文件内容；`--identity` 覆盖私钥；`--force` 才在远端看似忙碌时仍同步（默认拒绝）。

脚本会读 `.local/linux-hosts.json`，打 tar（含 `.git`，排除 `.local`、`build-win`、`build-clangd*`、`deploy/bin`、`.vs`、`__pycache__`），公钥 `scp` 后在远端解到临时目录再原子替换 `source`（默认 `$HOME/openidea`）。远端已有 `test_matrix.py` 或 `run_regressions.py` 时默认不同步、也不要再开矩阵。不要删除 `$HOME/opt`、已有 Redis/CMake 安装，或 `$HOME/openidea-test-results`。单测：`python tools/test_sync_linux_remote.py`。

## 远程预检

在将要跑矩阵的同一个 shell 里确认：

- `g++-13`、`python3`、`make` 可用。
- CMake：记录里的 `cmake`，否则 `command -v cmake`，再否则 `$HOME/opt/cmake/bin/cmake`。先 `cmake --version`。`ldd` 若显示某个库 `not found`，把该库所在目录（常见是 `$HOME/opt/cmake/lib`）放进 `LD_LIBRARY_PATH` 后再试。`/bin/sh: cmake: not found` 在文件还在时，通常是动态库没进本次环境，或文件在编译中途被换掉。预检通过后再启动矩阵；矩阵进行中不要重装、移动或删除 CMake。
- Redis：记录里的 `redis`，否则 `/usr/local/bin/redis-server`，再否则 `$HOME/opt/redis-7.4.12/bin/redis-server`。用 `--redis-binary` 传入。测试自己起临时实例。命令行里带 `:6379` 的 Redis 是用户的，不要杀，不要拿来当测试实例。
- 工具缺失就停下说明。没有 sudo 时不要 `apt install`。不要在测试过程中从 GitHub 现下载 CMake 或 Redis，不要把工具装进 `/tmp`。

预检还要列出该用户的长期进程（例如用户自己的 Redis、别的服务）。清理只匹配本仓库。

## 清理

远程机器上不要跑 `tools/kill_test_procs.py`。那个脚本在 Windows 宿主上调用 PowerShell 和 WSL，在 Linux 上会因为找不到 `powershell` 失败。

远程清理：

```sh
python3 tools/cleanup_linux_tests.py --root "$HOME/openidea"
```

它按仓库路径和 `.oi-test-owner.json` 匹配，并跳过命令行里带 `:6379` 的 Redis。不要按进程名杀，不要另写 `pkill`/`kill`。清理失败或有残留就报错并停止。

Windows 宿主上的 WSL 路仍用 `python tools/kill_test_procs.py`，见 `docs/testing.md` 第 7 节。

## 跑矩阵

同一构建目录上已有矩阵时不要再开。结果目录用新的 `$HOME/openidea-test-results/<lane>-<日期>`，不要覆盖上一轮失败目录。标准输出写到同级的 `runner.log`。

```sh
export PATH="$HOME/opt/cmake/bin:$PATH"
# 仅当预检证明需要时：
# export LD_LIBRARY_PATH="$HOME/opt/cmake/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cd "$HOME/openidea"
python3 tools/cleanup_linux_tests.py --root "$HOME/openidea"
python3 tools/test_matrix.py --platform native --mode <mt|st> --jobs <jobs> --case all \
  --build-dir "$HOME/openidea-build-gcc13" \
  --redis-binary "<预检得到的 redis-server>" \
  --artifacts "$HOME/openidea-test-results/<lane>-<日期>"
```

全量大约数分钟到二十分钟。长任务只用 `setsid nohup python3 tools/test_matrix.py ... > runner.log 2>&1 &` 启动，判断结束只看 artifacts 里 `summary.json` 的 `completed` 字段或 `test_matrix` 进程是否还在，不要自己另造退出码文件。不要在编译或用例中途重开。

`st` 机器用 `--mode st --no-restore`，结束后 `summary.json` 的 `restore_skipped` 为 true，`CMakeCache.txt` 仍是 `OI_IPC_SINGLE_THREAD:BOOL=ON`。`mt` 机器结束后 `restored` 为 true。恢复构建成功不等于用例通过。

只在显式要求时跑的用例（例如 `mysql`）直接加在上面的 `test_matrix.py` 命令上：`--mysqld "$MYSQL/bin/mysqld" --define OI_MYSQL_INCLUDE_DIR="$MYSQL/include" --define OI_MYSQL_LIBRARY="$MYSQL/lib/libmysqlclient.so"`。缺 `libaio` 时把已有兼容库目录加进 `LD_LIBRARY_PATH`。不要单独先编译再跑 mysql。

## 回报

只写看到的证据：

1. 退出码、起止时间。
2. `summary.json` 的 `passed`、`completed`、`restored`、`restore_skipped`。
3. `matrix.json` 每一步退出码。
4. 每个用例通过、失败或环境阻塞，以及耗时。
5. hold 日志里 `hold timer update` 那一行原文。
6. 失败时给出日志路径和编译错误或断言原文。没有 `st-tests.log` 就不要编造用例结果。
