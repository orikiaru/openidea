"""Windows 宿主：强杀回归测试残留进程，全局或只清 parallel_matrix 的某一路。

Windows 按仓库路径、归属标记和 PID 创建时间匹配，不按通用脚本名或端口匹配；
WSL 里只杀测试起的进程，保留 6379 上的 Redis。不会执行 wsl --shutdown。
"""
import argparse
import json
import os
import subprocess
from pathlib import Path

from parallel_matrix import LANES, ROOT

from test_process_owner import owned_path, windows_birth, windows_records, terminate_verified
import shlex
import sys
import time


def windows_processes():
    script = ("Get-CimInstance Win32_Process | Select-Object ProcessId,ParentProcessId,ExecutablePath,CommandLine,"
              "@{Name='CreationFileTimeUtc';Expression={if ($null -ne $_.CreationDate) {"
              "$_.CreationDate.ToUniversalTime().ToFileTimeUtc()}}} "
              "| ConvertTo-Json -Compress")
    output = subprocess.run(["powershell", "-NoProfile", "-Command", script], capture_output=True, text=True,
                            encoding="utf-8", errors="replace", check=True).stdout
    return json.loads(output or "[]")


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
    roots = [ROOT, windows_root] if lane == "all" else [windows_root / lane]
    selected_offset = None if lane == "all" else offset
    records = windows_records(roots, selected_offset)
    targets = []
    for item in processes:
        pid, path = item["ProcessId"], item["ExecutablePath"] or ""
        if pid in keep:
            continue
        try:
            arguments = [value.strip('"') for value in shlex.split(item["CommandLine"] or "", posix=False)]
        except ValueError:
            arguments = []  # An unrelated malformed command line is not an ownership proof.
        script = next((arg for arg in arguments[1:] if arg.endswith(".py")), "")
        python = Path(path).name.lower().startswith(("python", "pypy"))
        hit = bool(path and owned_path(path, roots, selected_offset))
        if python and script:
            hit |= owned_path(script, roots, selected_offset)
            # Host-side coordinator for one Linux lane must also belong to this repository.
            if lane.startswith("linux-") and owned_path(script, [ROOT]):
                hit |= any(arguments[index:index + 2] == ["--lane", lane] for index in range(len(arguments)))
        if hit or pid in records:
            snapshot_birth = item.get("CreationFileTimeUtc")
            # A reused PID with no owned path is not our process. Do not open it.
            if (not hit and type(snapshot_birth) is int and snapshot_birth > 0
                    and snapshot_birth // 10 != records[pid] // 10):
                continue
            try:
                birth = windows_birth(pid)
            except OSError as error:
                if getattr(error, "winerror", None) == 87:  # Process has exited.
                    continue
                raise
            snapshot_birth = item.get("CreationFileTimeUtc")
            # CIM timestamps have microsecond precision; keep FILETIME arithmetic integral.
            snapshot_matches = (type(snapshot_birth) is int and snapshot_birth > 0
                                and snapshot_birth // 10 == birth // 10)
            marker_matches = birth == records.get(pid)
            if (hit and snapshot_matches) or marker_matches:
                # A marker can prove ownership independently of a stale/missing CIM identity.
                label = (path or script) if snapshot_matches else "owner marker PID %d" % pid
                targets.append((pid, label, birth))
    return targets


def wsl_cleanup(lane, offset, user, linux_root, dry_run):
    prefix = ["wsl", "-u", user, "--exec"]
    def translate(path):
        return subprocess.check_output(prefix + ["wslpath", "-a", Path(path).as_posix()],
                                       text=True, encoding="utf-8").strip()
    command = prefix + ["python3", translate(ROOT / "tools/cleanup_linux_tests.py")]
    roots = [translate(ROOT), linux_root] if lane == "all" else [linux_root.rstrip("/") + "/" + lane]
    for root in roots:
        command += ["--root", root]
    if lane != "all":
        command += ["--offset", str(offset)]
    if dry_run:
        command.append("--dry-run")
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if result.returncode:
        raise RuntimeError("WSL cleanup failed: " + result.stdout + result.stderr)
    return result.stdout.splitlines()


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

    failed = False
    for pid, label, birth in windows_targets(args.lane, offset, args.windows_root.resolve()):
        print("windows", "match" if args.dry_run else "kill", pid, label, flush=True)
        if not args.dry_run:
            try:
                terminate_verified(pid, birth)
            except OSError as error:
                if getattr(error, "winerror", None) == 87:
                    continue
                failed = True
                print("  failed:", error, flush=True)
    if args.lane == "all" or args.lane.startswith("linux-"):
        try:
            for line in wsl_cleanup(args.lane, offset, args.wsl_user, args.linux_root, args.dry_run):
                print("wsl", line, flush=True)
        except (OSError, subprocess.SubprocessError, RuntimeError) as error:
            failed = True
            print(error, file=sys.stderr, flush=True)
    if args.dry_run:
        return int(failed)
    deadline = time.monotonic() + 3
    remaining = windows_targets(args.lane, offset, args.windows_root.resolve())
    while remaining and time.monotonic() < deadline:
        time.sleep(.05)
        remaining = windows_targets(args.lane, offset, args.windows_root.resolve())
    for pid, label, _ in remaining:
        print("windows remaining", pid, label, flush=True)
    return int(failed or bool(remaining))


if __name__ == "__main__":
    raise SystemExit(main())
