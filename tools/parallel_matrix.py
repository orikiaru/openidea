"""Windows 宿主：同步出四份工作目录，Windows MT/ST、Linux MT/ST 同时跑 test_matrix.py。

每份用自己的构建目录和 deploy/bin，并用不同的 --port-offset，互不抢端口。
Windows 侧 Redis 用原生 redis-server.exe；WSL 转发在大流量下会卡死，不要用 --redis-wsl。
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# 互不重叠：基准端口 16379-19995，每份平移 1000。
LANES = (("windows", "mt", 0), ("windows", "st", 1000), ("linux", "mt", 2000), ("linux", "st", 3000))
EXCLUDED_DIRS = ("build-win", "build-win-st", "build", "build-clangd-win", "build-clangd-linux", ".vs", "deploy/bin")
EXCLUDED_FILES = ("*.tar.gz",)


def wsl(user, command, **kwargs):
    return subprocess.run(["wsl", "-u", user, "--exec"] + command, **kwargs)


def linux_path(user, path):
    return wsl(user, ["wslpath", "-a", Path(path).as_posix()], capture_output=True, text=True, encoding="utf-8",
               errors="replace", check=True).stdout.strip()


def git_dir():
    # 作为子模块时 .git 是一行相对的 gitdir，拷到副本就指空了，副本里要换成绝对路径。
    marker = ROOT / ".git"
    if not marker.is_file():
        return None
    return (ROOT / marker.read_text(encoding="utf-8").strip().split(":", 1)[1].strip()).resolve()


def sync_windows(destination):
    excluded = [str(ROOT / name) for name in EXCLUDED_DIRS] + [str(destination / name) for name in EXCLUDED_DIRS]
    code = subprocess.run(["robocopy", str(ROOT), str(destination), "/MIR", "/NFL", "/NDL", "/NJH", "/NJS", "/NP",
                           "/XD"] + excluded + ["/XF"] + list(EXCLUDED_FILES)).returncode
    # robocopy 0-7 表示成功，8 及以上才是失败。
    if code >= 8:
        raise RuntimeError("robocopy failed %d -> %s" % (code, destination))
    if git_dir() is not None:
        # 拷来的 .git 带隐藏属性，Windows 不让直接覆盖写。
        (destination / ".git").unlink()
        (destination / ".git").write_text("gitdir: %s\n" % git_dir().as_posix(), encoding="utf-8")


def sync_linux(user, destination):
    command = ["rsync", "-a", "--delete"]
    for name in EXCLUDED_DIRS + EXCLUDED_FILES:
        command += ["--exclude", name if name.startswith("*") else "/" + name]
    wsl(user, ["mkdir", "-p", destination], check=True)
    wsl(user, command + [linux_path(user, ROOT) + "/", destination + "/"], check=True)
    # 从 Windows 盘拷过去会留下原用户属主。这里的 git 2.25.1 不认 safe.directory，属主对不上就拒绝读。
    wsl(user, ["chown", "-R", user + ":" + user, destination], check=True)
    if git_dir() is not None:
        wsl(user, ["sh", "-c", 'printf "gitdir: %s\\n" "$1" > "$2"', "sh", linux_path(user, git_dir()), destination + "/.git"], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--windows-root", type=Path, default=Path("F:/oi-par"))
    parser.add_argument("--linux-root", default="/root/oi-par")
    parser.add_argument("--wsl-user", default="root")
    parser.add_argument("--windows-redis-binary", required=True)
    parser.add_argument("--redis-binary", help="Linux lanes; default: redis-server on WSL PATH")
    parser.add_argument("--case", default="all")
    parser.add_argument("--mysqld", help="Linux lanes: build with MySQL and run the mysql case before each lane's tests")
    parser.add_argument("--define", action="append", default=[], metavar="NAME=VALUE",
                        help="Linux lanes: extra CMake cache entry passed to test_matrix.py")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--lane", action="append", choices=["%s-%s" % (platform, mode) for platform, mode, _ in LANES],
                        help="run only these lanes; repeatable, default all four")
    args = parser.parse_args()
    artifacts = args.artifacts.resolve()
    artifacts.mkdir(parents=True, exist_ok=True)

    processes = []
    for platform, mode, offset in LANES:
        lane = "%s-%s" % (platform, mode)
        if args.lane and lane not in args.lane:
            continue
        output = artifacts / lane
        log = (artifacts / (lane + ".log")).open("w", encoding="utf-8")
        common = ["--mode", mode, "--case", args.case, "--jobs", str(args.jobs), "--port-offset", str(offset)]
        if platform == "windows":
            copy = args.windows_root / lane
            sync_windows(copy)
            command = [sys.executable, str(copy / "tools/test_matrix.py"), "--build-dir", str(copy / "build-win"),
                       "--redis-binary", args.windows_redis_binary, "--artifacts", str(output)] + common
            cwd = copy
        else:
            copy = args.linux_root + "/" + lane
            sync_linux(args.wsl_user, copy)
            command = ["wsl", "-u", args.wsl_user, "--exec", "python3", copy + "/tools/test_matrix.py",
                       "--build-dir", copy + "/build", "--artifacts", linux_path(args.wsl_user, output)] + common
            if args.redis_binary:
                command += ["--redis-binary", args.redis_binary]
            if args.mysqld:
                command += ["--mysqld", args.mysqld]
            for item in args.define:
                command += ["--define", item]
            cwd = ROOT
        print("START", lane, "port_offset=%d" % offset, flush=True)
        processes.append((lane, subprocess.Popen(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT), log, time.monotonic()))

    results = {}
    for lane, process, log, started in processes:
        code = process.wait()
        log.close()
        results[lane] = {"exit_code": code, "seconds": round(time.monotonic() - started, 1)}
        print("PASS" if code == 0 else "FAIL", lane, "seconds=%.1f" % results[lane]["seconds"],
              "log=", artifacts / (lane + ".log"), flush=True)
    (artifacts / "parallel.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    return int(any(item["exit_code"] for item in results.values()))


if __name__ == "__main__":
    raise SystemExit(main())
