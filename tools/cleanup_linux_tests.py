"""WSL-side cleanup. Only verified repository executables or marked test directories qualify."""
import argparse
import os
from pathlib import Path
import re
import signal
import time

from test_process_owner import owned_path, read_owner, within, linux_records, linux_start

APPLICATIONS = {"ipcclient", "ipcserver", "logd", "redisserver", "registry", "webclient", "webserver", "redis-server"}


def matches(executable, arguments, cwd, roots, offset):
    if Path(executable).name == "redis-server" and re.search(r":6379(?:\s|$)", " ".join(arguments)):
        return False
    if any(within(executable, root) for root in roots):
        return True
    if Path(executable).name.startswith(("python", "pypy")):
        script = next((arg for arg in arguments[1:] if arg.endswith(".py")), "")
        if script and owned_path(script, roots, offset):
            return True
    if Path(executable).name in APPLICATIONS:
        directory = Path(cwd)
        return any(read_owner(parent, roots, offset) for parent in (directory, *directory.parents))
    return False


def snapshot(pid):
    root = Path("/proc") / str(pid)
    stat = (root / "stat").read_text().rsplit(")", 1)[1].split()
    if stat[0] == "Z":
        return None
    return (os.readlink(root / "exe"), (root / "cmdline").read_bytes().decode(errors="replace").split("\0"),
            os.readlink(root / "cwd"), stat[19])


def targets(roots, offset):
    keep = {os.getpid(), os.getppid()}
    records = linux_records(roots, offset)
    found = {}
    for root in Path("/proc").iterdir():
        if not root.name.isdigit() or int(root.name) in keep:
            continue
        try:
            item = snapshot(int(root.name))
        except (FileNotFoundError, ProcessLookupError):
            continue
        except PermissionError as error:
            pid = int(root.name)
            try:
                if pid in records:
                    if linux_start(pid) == records[pid]:
                        raise RuntimeError("cannot inspect owned WSL test PID %d: %s" % (pid, error)) from error
                else:
                    arguments = (root / "cmdline").read_bytes().decode(errors="replace").split("\0")
                    script = next((arg for arg in arguments[1:] if arg.endswith(".py")), "")
                    if (owned_path(arguments[0], roots, offset)
                            or (Path(arguments[0]).name.startswith(("python", "pypy"))
                                and owned_path(script, roots, offset))):
                        raise RuntimeError("cannot inspect repository WSL test PID %d: %s" % (pid, error)) from error
            except (FileNotFoundError, ProcessLookupError):
                pass
            except PermissionError:
                if pid in records:
                    raise RuntimeError("cannot verify recorded WSL test PID %d" % pid) from error
            continue
        if item and matches(*item[:3], roots, offset):
            found[int(root.name)] = item
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", action="append", required=True)
    parser.add_argument("--offset", type=int)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    errors = []
    for pid, item in targets(args.root, args.offset).items():
        print("match" if args.dry_run else "kill", pid, item[0], flush=True)
        if args.dry_run:
            continue
        try:
            current = snapshot(pid)
            if current and current[3] == item[3]:
                os.kill(pid, signal.SIGKILL)
        except (FileNotFoundError, ProcessLookupError):
            pass
        except OSError as error:
            errors.append(str(error))
    if not args.dry_run:
        end = time.monotonic() + 3
        remaining = targets(args.root, args.offset)
        while remaining and time.monotonic() < end:
            time.sleep(.05)
            remaining = targets(args.root, args.offset)
        if remaining:
            errors.append("remaining WSL test PIDs: " + ",".join(map(str, remaining)))
    for error in errors:
        print(error, flush=True)
    return int(bool(errors))


if __name__ == "__main__":
    raise SystemExit(main())
