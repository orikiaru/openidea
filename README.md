# openidea

C++20 的模块化网络进程。基础库在 `cbaselib`，业务进程从 `modules/xmlconfig` 生成骨架。同一条协程任务可以跑在单线程，也可以跑在多线程。

## 目录

| 路径 | 内容 |
| --- | --- |
| `cbaselib` | 线程池、网络、协议、定时器、日志客户端 |
| `modules/xmlconfig` | 模块、组件、协议的生成输入 |
| `modules/include` | 每次 cmake 重写的序列化代码 |
| `modules/<name>` | 一个进程。现有 `logd`、`ipcclient`、`ipcserver` |
| `deploy/bin` | 可执行文件 |
| `deploy/config` | 与可执行文件同名的 toml |
| `docs` | 说明 |

## 编译

CMake 3.10、Python 3，编译器用 GCC 11 或更高。这套协程调度在 GCC 10 上不能用。

```shell
mkdir build
cd build
cmake ..
make
```

cmake 会先跑 `cbaselib/proto/proto_tool/main.py`，再编库和模块。可执行文件在 `deploy/bin`。

模块默认多线程，链接 `*_m_static`。要单线程时，把该模块 `CMakeLists.txt` 里的 `SINGLE_THREAD` 改成 `TRUE`，会改链 `*_static` 并定义 `_SINGLE_THREAD_`。`logd` 受 logmanager 限制，只能多线程。

## 运行

在 `deploy/bin` 下启动，配置读 `../config/<进程名>.toml`。

先起 `logd`，再起 `ipcserver`，再起 `ipcclient`。默认 ipc 端口 `17981`，日志端口 `19995`，地址是 `127.0.0.1`。

## 说明

- [基础库](docs/cbaselib.md)
- [协程](docs/coroutine.md)
- [模块和协议](docs/module.md)
- [定时器](docs/timer.md)
