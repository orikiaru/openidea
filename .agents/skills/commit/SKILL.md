---
name: commit
description: >-
  按 openidea 仓库规则提交并推送当前分支：检查 diff、补 todo/文档、生成中文提交说明、普通 push 并核对远端。
  用户说提交、commit、推送、push、提交子模块、更新子模块指针时使用。
---

# 提交与推送

用户说「提交」时，默认包含本地 `git commit` 和推送当前分支的远程，不需要再问一次。只让看 diff、review 或整理改动时不要提交。

## 硬规则

- 不改 git 配置，不改远程地址，不强推，不 `push --force`，不 amend 已推送的提交。
- 只提交本次任务相关的文件。用户没要求的改动、临时文件、构建产物、日志、令牌和本机路径都不进提交。
- 不跳过 hooks。不删断言、不放宽超时、不靠重试掩盖失败来完成提交。
- 不能只提交不推送；推送失败要说明原因和用户需要做的动作，不能假装完成。

## 1. 侦察

先看工作区，不要直接 `git add -A`：

```sh
git status --porcelain
git branch --show-current
git remote -v
git log --oneline -5
git diff --stat
```

有未暂存改动时逐个读 `git diff`；有暂存内容时再读 `git diff --cached`。确认改动范围与本次请求一致。

以下路径不应进入提交：

- `.local/`、`reviewsession/`、`build-*`、`deploy/bin/`、`modules/include/`、`__pycache__/`、`*.obj`；
- 日志、结果目录、PAT/密码/本机 IP 文件；
- CMake configure 改写的已跟踪 `.clangd`。若它只是构建副作用，恢复原内容，不提交。

子模块改动先确认：openidea 作为应用仓库子模块时，框架改动在子模块内单独提交推送，父仓库再更新子模块指针并单独提交；应用侧改动不要带进子模块。

## 2. 补文档和待办

- 行为变化同步 `docs/` 对应文档、时序图或接入表。
- 没做完的事、已知缺口写进 `todo.md`，写清需求、现状和方向。
- 全量四路回归通过才更新 `docs/lasttesting.md`；只跑部分用例不要写成全量通过。

## 3. 验证

按改动范围选择验证，编译和回归矩阵交给 subagent（AGENTS.md 的长期规则），主对话核对结果：

- 有行为改动：Linux 和 Windows 都要跑；默认单线程、多线程各一遍。协程、挂起、调度、`Task`、`StartTask`、`co_await`、`yield` 一定涉及两种线程模式，不能当纯逻辑跳过。
- 只改文档、注释或纯生成说明：可以不编译，但要在提交说明里写明未跑。
- 至少检查与改动相关的格式/命名/继承工具，例如 `python3 tools/format_cpp.py --check`、`python3 tools/check_inheritance.py`。
- 没有跑到的测试必须在提交说明和最终回复里写「未跑」，不能写「通过」。

## 4. 分批与暂存

- 无关改动分开提交；同一主题才合并。范围不清或存在用户自己未提交的改动时，先给选项问用户，不要替用户决定提交范围。
- 显式暂存路径：

```sh
git add <path> [<path> ...]
git diff --cached --stat
git diff --cached --check
```

再读一次 `git diff --cached`，确认没有混入秘密、临时文件或无关格式化。

## 5. 提交说明

用中文，参考仓库历史风格；subject 概括改了什么，不加 `feat:`/`fix:` 前缀，不写「更新代码」这类空话。正文列关键改动和验证情况：

```text
动词短语，概括改了什么，必要时带修复对象或模块

- 关键改动 1
- 关键改动 2
- 验证：跑了什么；没跑什么、原因
```

示例：

```text
修复时间轮积压续期和 HTTP 边界，补充回归

- Update 追赶期间到期定时器直接进入本批，不再延后一跳
- HTTP 等待队列增加条数和字节上限，拒绝时返回 BUSY
- 验证：Linux/Windows 单线程与多线程 --case all 通过；coro 未跑
```

## 6. 推送

```sh
git push
```

- 非快进或远程拒绝：停下，`git fetch` 后看差异，rebase/merge 让用户拍板；不要强推。
- 认证失败：把验证动作交给用户，不索要密码。本机可用 `git-credential-manager github login --browser` 重新登录。
- Windows 沙箱里 HTTPS 报 `schannel: AcquireSecurityContextHandle ... SEC_E_NO_CREDENTIALS` 时，用 OpenSSL 后端重试：

```sh
git -c http.sslBackend=openssl push
```

- 沙箱拒绝写 `.git`（例如 `index.lock: Permission denied`）时，按当前会话策略请求完整权限或让用户执行；不要复制仓库或绕开沙箱。

## 7. 核对与回复

推送后核对远端确实到达本次提交：

```sh
git -c http.sslBackend=openssl ls-remote origin refs/heads/<当前分支>
git status -sb
git log --oneline -1
```

回复写清楚：

1. commit hash 和 subject；
2. 推送范围（例如 `5d6cf82..245e09c master -> master`）；
3. 实际跑了哪些测试、哪些没跑；
4. 工作区是否干净，或故意留下了哪些未提交改动。

如果没有可提交内容，直接说明，不造空提交。只能本地提交、不能推送时，明确告知原因和用户需要做的认证动作。
