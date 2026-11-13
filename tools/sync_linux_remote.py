"""从 Windows 宿主把本仓库同步到 .local/linux-hosts.json 里的远程 Linux 机器。

不要手写 tar/scp。远程已有 test_matrix / run_regressions 时默认拒绝同步。
不把 IP、用户名写进可入库文件；终端只打印 role 与远端 source 路径。
"""
from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
import tarfile
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOSTS_FILE = ROOT / ".local" / "linux-hosts.json"
DEFAULT_IDENTITY = Path(os.path.expandvars(r"%USERPROFILE%\.ssh\id_rsa"))
DEFAULT_SOURCE = "$HOME/openidea"
SSH_OPTS = (
    "-o",
    "IdentitiesOnly=yes",
    "-o",
    "BatchMode=yes",
    "-o",
    "PreferredAuthentications=publickey",
    "-o",
    "ConnectTimeout=15",
)

# 顶层目录/文件名，或任意路径段名。
EXCLUDE_NAMES = {
    ".local",
    "build-win",
    "build-win-st",
    "build",
    "build-clangd",
    "build-clangd-win",
    "build-clangd-linux",
    ".vs",
    "__pycache__",
}
EXCLUDE_SUFFIXES = (".tar", ".tar.gz", ".tgz")


def load_hosts(path: Path) -> list[dict]:
    if not path.is_file():
        raise SystemExit(f"missing hosts file: {path}")
    data = json.loads(path.read_text(encoding="utf-8"))
    hosts = data.get("hosts")
    if not isinstance(hosts, list) or not hosts:
        raise SystemExit(f"hosts[] missing in {path}")
    return hosts


def pick_hosts(hosts: list[dict], role: str) -> list[dict]:
    if role == "all":
        selected = list(hosts)
    else:
        selected = [host for host in hosts if host.get("role") == role]
    if not selected:
        raise SystemExit(f"no host with role={role}")
    for host in selected:
        if not host.get("ip") or not host.get("user"):
            raise SystemExit(f"host role={host.get('role')!r} missing ip or user")
    return selected


def identity_for(host: dict, override: Path | None) -> Path:
    raw = override or host.get("identity") or DEFAULT_IDENTITY
    path = Path(os.path.expandvars(str(raw))).expanduser()
    if not path.is_file():
        raise SystemExit(f"ssh identity missing: {path}")
    return path


def source_for(host: dict) -> str:
    return str(host.get("source") or DEFAULT_SOURCE)


def ssh_base(identity: Path, user: str, ip: str) -> list[str]:
    return ["ssh", "-i", str(identity), *SSH_OPTS, f"{user}@{ip}"]


def scp_base(identity: Path) -> list[str]:
    return ["scp", "-i", str(identity), *SSH_OPTS]


def run(command: list[str], *, input_bytes: bytes | None = None) -> subprocess.CompletedProcess[str]:
    # Windows 文本模式会把 \\n 再写成 \\r\\n，远端 bash 会把 set -eu 解析坏。stdin 一律按字节送。
    return subprocess.run(
        command,
        input=input_bytes,
        capture_output=True,
        check=False,
    )


def remote_bash(identity: Path, user: str, ip: str, script: str) -> subprocess.CompletedProcess[str]:
    cleaned = script.replace("\r\n", "\n").replace("\r", "\n")
    if not cleaned.endswith("\n"):
        cleaned += "\n"
    completed = run([*ssh_base(identity, user, ip), "bash", "-s"], input_bytes=cleaned.encode("utf-8"))
    return subprocess.CompletedProcess(
        completed.args,
        completed.returncode,
        completed.stdout.decode("utf-8", errors="replace"),
        completed.stderr.decode("utf-8", errors="replace"),
    )


def should_exclude(relative: Path) -> bool:
    parts = relative.parts
    if not parts:
        return False
    if parts[0] in EXCLUDE_NAMES:
        return True
    if len(parts) >= 2 and parts[0] == "deploy" and parts[1] == "bin":
        return True
    if any(part == "__pycache__" or part == ".vs" for part in parts):
        return True
    if any(part.startswith("build-clangd") for part in parts):
        return True
    name = parts[-1]
    if name.endswith(EXCLUDE_SUFFIXES):
        return True
    return False


def add_tree(archive: tarfile.TarFile, root: Path) -> int:
    count = 0
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root)
        if should_exclude(relative):
            continue
        if path.is_symlink():
            continue
        if path.is_dir():
            info = tarfile.TarInfo(name=relative.as_posix().rstrip("/") + "/")
            info.type = tarfile.DIRTYPE
            info.mtime = int(path.stat().st_mtime)
            info.mode = 0o755
            archive.addfile(info)
            count += 1
            continue
        if not path.is_file():
            continue
        archive.add(path, arcname=relative.as_posix(), recursive=False)
        count += 1
    return count


def build_archive(root: Path, archive_path: Path) -> int:
    if archive_path.exists():
        archive_path.unlink()
    with tarfile.open(archive_path, "w") as archive:
        return add_tree(archive, root)


def busy_script(source: str) -> str:
    return f"""set -eu
SOURCE={shlex.quote(source)}
if ps -eo args | grep -E '[t]est_matrix\\.py|[r]un_regressions\\.py' >/dev/null
then
  echo BUSY=1
  ps -eo pid,args | grep -E '[t]est_matrix\\.py|[r]un_regressions\\.py' | grep -v grep || true
  exit 2
fi
echo BUSY=0
echo SOURCE_PATH=$SOURCE
"""


def apply_script(source: str, remote_tar: str, require_checks: list[tuple[str, str]]) -> str:
    check_blocks = []
    for relative, needle in require_checks:
        rel = shlex.quote(relative)
        needle_q = shlex.quote(needle)
        check_blocks.append(
            f"""
if ! test -f "$STAGE"/{rel}
then
  echo MISSING={relative}
  exit 4
fi
if ! grep -F {needle_q} "$STAGE"/{rel} >/dev/null
then
  echo MARKER_MISS={relative}
  exit 5
fi
"""
        )
    checks_text = "\n".join(check_blocks)
    return f"""set -eu
SOURCE={shlex.quote(source)}
TAR={shlex.quote(remote_tar)}
STAGE="${{SOURCE}}.incoming.$$"
BACKUP="${{SOURCE}}.backup.$$"
if ps -eo args | grep -E '[t]est_matrix\\.py|[r]un_regressions\\.py' >/dev/null
then
  echo BUSY=1
  exit 2
fi
test -f "$TAR"
rm -rf "$STAGE"
mkdir -p "$STAGE"
tar -xf "$TAR" -C "$STAGE"
test -f "$STAGE/tools/test_matrix.py"
test -f "$STAGE/tools/cleanup_linux_tests.py"
test -d "$STAGE/.git" -o -f "$STAGE/.git"
{checks_text}
PARENT=$(dirname "$SOURCE")
mkdir -p "$PARENT"
if test -e "$SOURCE"
then
  mv "$SOURCE" "$BACKUP"
fi
mv "$STAGE" "$SOURCE"
rm -rf "$BACKUP"
rm -f "$TAR"
echo SYNC_OK=1
echo SOURCE_PATH=$SOURCE
"""


def parse_require(values: list[str] | None) -> list[tuple[str, str]]:
    result: list[tuple[str, str]] = []
    for item in values or []:
        if ":" not in item:
            raise SystemExit(f"--require-text needs PATH:SUBSTRING, got {item!r}")
        relative, needle = item.split(":", 1)
        relative = relative.replace("\\", "/").lstrip("./")
        if not relative or not needle:
            raise SystemExit(f"--require-text needs PATH:SUBSTRING, got {item!r}")
        result.append((relative, needle))
    return result


def sync_one(host: dict, identity: Path, archive: Path, require_checks: list[tuple[str, str]], force: bool) -> None:
    role = host.get("role", "?")
    user = host["user"]
    ip = host["ip"]
    source = source_for(host)
    stamp = time.strftime("%Y%m%d%H%M%S")
    remote_tar = f"$HOME/openidea-sync-{stamp}.tar"
    # scp 不能展开远端 $HOME，先解析。
    home = remote_bash(identity, user, ip, "printf '%s\\n' \"$HOME\"")
    if home.returncode != 0:
        raise SystemExit(f"role={role} ssh failed: {home.stderr.strip() or home.stdout.strip()}")
    home_dir = home.stdout.strip()
    if not home_dir:
        raise SystemExit(f"role={role} empty HOME")
    remote_tar_path = f"{home_dir}/openidea-sync-{stamp}.tar"
    source_expanded = source.replace("$HOME", home_dir)

    busy = remote_bash(identity, user, ip, busy_script(source_expanded))
    if busy.returncode == 2 and not force:
        raise SystemExit(f"role={role} busy; refuse sync\n{busy.stdout}")
    if busy.returncode not in (0, 2):
        raise SystemExit(f"role={role} busy check failed: {busy.stderr or busy.stdout}")

    upload = run([*scp_base(identity), str(archive), f"{user}@{ip}:{remote_tar_path}"])
    if upload.returncode != 0:
        raise SystemExit(f"role={role} scp failed: {upload.stderr or upload.stdout}")

    apply = remote_bash(identity, user, ip, apply_script(source_expanded, remote_tar_path, require_checks))
    if apply.returncode != 0:
        raise SystemExit(f"role={role} apply failed ({apply.returncode}):\n{apply.stdout}\n{apply.stderr}")
    if "SYNC_OK=1" not in apply.stdout:
        raise SystemExit(f"role={role} apply missing SYNC_OK:\n{apply.stdout}\n{apply.stderr}")
    print(f"SYNC_OK role={role} source={source_expanded}", flush=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--role", choices=("mt", "st", "all"), required=True)
    parser.add_argument("--hosts", type=Path, default=HOSTS_FILE)
    parser.add_argument("--identity", type=Path, help="override ssh private key for all selected hosts")
    parser.add_argument("--root", type=Path, default=ROOT, help="local repository root")
    parser.add_argument(
        "--require-text",
        action="append",
        default=[],
        metavar="PATH:SUBSTRING",
        help="after extract, remote PATH must contain SUBSTRING; repeatable",
    )
    parser.add_argument("--force", action="store_true", help="sync even if remote matrix looks busy")
    parser.add_argument("--keep-archive", type=Path, help="keep local tar at this path")
    args = parser.parse_args(argv)

    root = args.root.resolve()
    if not (root / "tools" / "test_matrix.py").is_file():
        raise SystemExit(f"not a repo root: {root}")
    hosts = pick_hosts(load_hosts(args.hosts.resolve()), args.role)
    require_checks = parse_require(args.require_text)

    if args.keep_archive:
        archive_path = args.keep_archive.resolve()
        archive_path.parent.mkdir(parents=True, exist_ok=True)
        count = build_archive(root, archive_path)
        print(f"ARCHIVE path={archive_path} entries={count}", flush=True)
        for host in hosts:
            sync_one(host, identity_for(host, args.identity), archive_path, require_checks, args.force)
        return 0

    with tempfile.TemporaryDirectory(prefix="openidea-sync-") as tmp:
        archive_path = Path(tmp) / "openidea-sync.tar"
        count = build_archive(root, archive_path)
        print(f"ARCHIVE entries={count} bytes={archive_path.stat().st_size}", flush=True)
        for host in hosts:
            sync_one(host, identity_for(host, args.identity), archive_path, require_checks, args.force)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
