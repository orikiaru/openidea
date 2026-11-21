"""进程之间的文本用 UTF-8。

Windows 的 ANSI 代码页是 GBK。subprocess 的 text=True 不写 encoding 时按这个代码页解码，
读线程遇到非法字节会丢掉整段输出，调用方拿到的 stdout 是 None。
本仓库的源码、日志和 Python 帮助都是 UTF-8。捕获文本时带上 TEXT。
再启动 Python 时用 child_env，让子进程的标准流也按 UTF-8 写。
"""
import os

TEXT = {"encoding": "utf-8", "errors": "replace"}


def child_env(base=None):
    env = dict(os.environ if base is None else base)
    env["PYTHONUTF8"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"
    return env


def use_utf8_children():
    os.environ["PYTHONUTF8"] = "1"
    os.environ["PYTHONIOENCODING"] = "utf-8"
