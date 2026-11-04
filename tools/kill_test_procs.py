"""Windows 宿主：强杀回归测试残留进程，全局或只清 parallel_matrix 的某一路。

Windows 按可执行文件路径、命令行和本路 Redis 端口匹配，不按进程名（系统自带同名的 Registry 进程不能碰）；
WSL 里只杀测试起的进程，保留 6379 上的 Redis。不会执行 wsl --shutdown。
"""
import argparse
import json
import os
import signal
import subprocess
from pathlib import Path

from parallel_matrix import LANES, ROOT

REDIS_BASE_PORT = 16380
TEST_SCRIPTS = ("run_regressions", "test_matrix", "parallel_matrix", "redis_fixture", "http_fixture")


def windows_processes():
    script = ("Get-CimInstance Win32_Process | Select-Object ProcessId,ParentProcessId,ExecutablePath,CommandLine "
              "| ConvertTo-Json -Compress")
    output = subprocess.run(["powershell", "-NoProfile", "-Command", script], capture_output=True, text=True,
                            encoding="utf-8", errors="replace", check=True).stdout
    return json.loads(output or "[]")


def listeners(ports):
    script = ("Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { @(%s) -contains "
              "$_.LocalPort } | Select-Object -ExpandProperty OwningProcess") % ",".join(str(port) for port in ports)
    output = subprocess.run(["powershell", "-NoProfile", "-Command", script], capture_output=True, text=True,
                            encoding="utf-8", errors="replace").stdout
    return {int(line) for line in output.split() if line.strip().isdigit()}


def ancestors(processes):
    parents = {item["ProcessId"]: item["ParentProcessId"] for item in processes}
    chain, pid = set(), os.getpid()
    while pid and pid not in chain:
        chain.add(pid)
        pid = parents.get(pid)
    return chain


def windows_targets(lane, offset, windows_root):
    processes = windows_processes()
    keep = ancestors(processes)
    lanes = [item for item in LANES if lane in ("all", "%s-%s" % item[:2])]
    redis_pids = listeners([REDIS_BASE_PORT + item[2] for item in lanes])
    targets = []
    for item in processes:
        pid, path = item["ProcessId"], (item["ExecutablePath"] or "").lower()
        command = (item["CommandLine"] or "").lower().replace("/", "\\")
        if pid in keep or not (path or command):
            continue
        if lane == "all":
            hit = (path.startswith(str(ROOT).lower() + "\\") or path.startswith(str(windows_root).lower() + "\\")
                   or "oi-regression-" in path or "oi-regression-" in command
                   or ("python" in path and any(name in command for name in TEST_SCRIPTS)))
        elif lane.startswith("windows-"):
            copy = str(windows_root / lane).lower()
            hit = (path.startswith(copy + "\\") or copy in command or "oi-regression-p%d-" % offset in command
                   or "oi-regression-web-p%d-" % offset in command)
        else:
            hit = "--lane " + lane in command
        if hit or (pid in redis_pids and path.endswith("redis-server.exe")):
            targets.append((pid, item["ExecutablePath"] or item["CommandLine"]))
    return targets


def wsl_cleanup(lane, offset, user, linux_root, dry_run):
    if lane == "all":
        patterns = ["[/]" + linux_root.strip("/") + "/", "[o]i-regression-", "[r]edis_fixture.py"]
        redis_ports = None
    else:
        patterns = ["[/]%s/%s/" % (linux_root.strip("/"), lane), "[o]i-regression-p%d-" % offset,
                    "[o]i-regression-web-p%d-" % offset]
        redis_ports = [REDIS_BASE_PORT + offset]
    action = "pgrep -af" if dry_run else "pkill -9 -ef"
    script = "".join("%s '%s' || true\n" % (action, pattern) for pattern in patterns)
    # redis-server 的进程名带监听地址；全局清理杀掉所有非 6379 的实例，按路清理只杀本路端口。
    script += ("for pid in $(pgrep -x redis-server); do\n"
               "  title=$(tr '\\0' ' ' < /proc/$pid/cmdline)\n"
               "  case \"$title\" in *:6379*) continue;; esac\n")
    if redis_ports:
        script += "  case \"$title\" in %s) ;; *) continue;; esac\n" % "|".join("*:%d*" % port for port in redis_ports)
    script += "  echo \"redis $pid $title\"\n"
    if not dry_run:
        script += "  kill -9 $pid\n"
    script += "done\n"
    output = subprocess.run(["wsl", "-u", user, "--exec", "bash", "-c", script], capture_output=True, text=True,
                            encoding="utf-8", errors="replace").stdout
    return [line for line in output.splitlines() if line.strip()]


def main():
    lane_names = ["all"] + ["%s-%s" % item[:2] for item in LANES]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lane", choices=lane_names, default="all")
    parser.add_argument("--windows-root", type=Path, default=Path("F:/oi-par"))
    parser.add_argument("--linux-root", default="/root/oi-par")
    parser.add_argument("--wsl-user", default="root")
    parser.add_argument("--dry-run", action="store_true", help="list matches without killing")
    args = parser.parse_args()
    offset = next((item[2] for item in LANES if "%s-%s" % item[:2] == args.lane), 0)

    for pid, label in windows_targets(args.lane, offset, args.windows_root.resolve()):
        print("windows", "match" if args.dry_run else "kill", pid, label, flush=True)
        if not args.dry_run:
            try:
                os.kill(pid, signal.SIGTERM)
            except OSError as error:
                print("  failed:", error, flush=True)
    if args.lane == "all" or args.lane.startswith("linux-"):
        for line in wsl_cleanup(args.lane, offset, args.wsl_user, args.linux_root, args.dry_run):
            print("wsl", line, flush=True)
    if args.dry_run:
        return 0
    remaining = windows_targets(args.lane, offset, args.windows_root.resolve())
    for pid, label in remaining:
        print("windows remaining", pid, label, flush=True)
    return int(bool(remaining))


if __name__ == "__main__":
    raise SystemExit(main())
