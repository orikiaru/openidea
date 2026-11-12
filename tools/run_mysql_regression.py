"""Run the MySQL regression against an isolated, temporary local instance."""

import argparse
import os
from pathlib import Path
import socket
import shutil
import subprocess
import sys
import tempfile
import time

from test_process_owner import write_owner, record_process, windows_birth, terminate_verified


REPO = Path(__file__).resolve().parents[1]


class EnvironmentBlocked(RuntimeError):
    pass


def executable(value, name):
    path = Path(value).expanduser().resolve()
    if not path.is_file():
        raise EnvironmentBlocked(f"{name} not found: {path}")
    return path


def quoted_path(path):
    # Forward slashes work in MySQL option files on Windows as well as Unix.
    if '"' in path.as_posix():
        raise ValueError("MySQL option path contains a quote")
    return '"' + path.as_posix() + '"'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mysqld", help="mysqld executable in a persistent installation")
    parser.add_argument("--mysql", help="mysql client executable; defaults to the mysqld directory, then PATH")
    parser.add_argument("--bin-dir", type=Path, default=REPO / "deploy" / "bin")
    parser.add_argument("--artifacts", type=Path)
    parser.add_argument("--port-offset", type=int, default=0)
    args = parser.parse_args()
    if not args.mysqld:
        raise EnvironmentBlocked("--mysqld is required; install MySQL in a persistent directory")
    mysqld = executable(args.mysqld, "mysqld")
    sibling = mysqld.with_name("mysql.exe" if os.name == "nt" else "mysql")
    # 发行版包把 mysqld 放在 sbin、客户端放在 bin，同目录没有就用 PATH 里的。
    mysql = executable(args.mysql or (sibling if sibling.is_file() else shutil.which("mysql") or sibling), "mysql client")
    env = os.environ.copy()
    env.pop("MYSQL_PWD", None)
    # Keep caller-provided SDK paths intact, adding the installation bin directory.
    env["PATH"] = str(mysqld.parent) + os.pathsep + env.get("PATH", "")
    if os.name != "nt":
        env["LD_LIBRARY_PATH"] = env.get("LD_LIBRARY_PATH", "")

    with tempfile.TemporaryDirectory(prefix="oi-regression-p%d-mysql-" % args.port_offset) as temporary:
        root = Path(temporary)
        write_owner(root, REPO, args.port_offset)
        data = root / "data"
        data.mkdir()
        # 发行版包编译进的默认导入导出目录（/var/lib/mysql-files）可能不存在，不存在 mysqld 拒绝启动。
        files = root / "files"
        files.mkdir(mode=0o700)
        # --no-defaults 仍会读 ~/.mylogin.cnf；8.0 客户端没有 --no-login-paths，改为指向不存在的文件。
        env["MYSQL_TEST_LOGIN_FILE"] = str(root / "no-login.cnf")
        config = root / "my.cnf"
        log = root / "mysqld.log"
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        config.write_text("[mysqld]\n" +
                          "basedir=" + quoted_path(mysqld.parent.parent) + "\n" +
                          "datadir=" + quoted_path(data) + "\n" +
                          "socket=" + quoted_path(root / "mysql.sock") + "\n" +
                          "pid-file=" + quoted_path(root / "mysqld.pid") + "\n" +
                          "log-error=" + quoted_path(log) + "\n" +
                          "secure-file-priv=" + quoted_path(files) + "\n" +
                          "mysqlx=0\nbind-address=127.0.0.1\nport=" + str(port) + "\n", encoding="utf-8")
        server_args = [str(mysqld), "--defaults-file=" + str(config)]
        if os.name != "nt" and hasattr(os, "geteuid") and os.geteuid() == 0:
            server_args.append("--user=root")
        try:
            subprocess.run(server_args + ["--initialize-insecure", "--skip-networking"],
                           env=env, check=True, timeout=120)
        except (OSError, subprocess.SubprocessError):
            if args.artifacts and log.exists():
                args.artifacts.mkdir(parents=True, exist_ok=True)
                shutil.copy2(log, args.artifacts / "mysql-initialize.log")
            raise
        if log.exists():
            log.unlink()
        process = None
        child_identity = None
        try:
            process = subprocess.Popen(server_args, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            record_process(root, process)
            deadline = time.monotonic() + 60
            while True:
                if process.poll() is not None:
                    details = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
                    raise RuntimeError(f"owned mysqld exited during startup ({process.returncode}):\n{details}")
                details = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
                if "ready for connections" in details.lower():
                    if os.name == "nt":
                        pid = int((root / "mysqld.pid").read_text().strip())
                        if pid != process.pid:
                            class OwnedProcess:
                                def __init__(self, pid):
                                    self.pid = pid
                                def poll(self):
                                    return None
                            child_identity = (pid, windows_birth(pid))
                            record_process(root, OwnedProcess(pid))
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError("owned mysqld startup timed out:\n" + details)
                time.sleep(0.1)
            client = [str(mysql), "--no-defaults", "--protocol=tcp", "--host=127.0.0.1",
                      "--port=" + str(port), "--user=root", "--execute=CREATE DATABASE oi_regression"]
            subprocess.run(client, env=env, check=True, timeout=30)
            env.update(OI_TEST_MYSQL_HOST="127.0.0.1", OI_TEST_MYSQL_PORT=str(port),
                       OI_TEST_MYSQL_USER="root", OI_TEST_MYSQL_PASSWORD="", OI_TEST_MYSQL_DATABASE="oi_regression")
            command = [sys.executable, str(REPO / "tools" / "run_regressions.py"), "--case", "mysql",
                       "--bin-dir", str(args.bin_dir), "--port-offset", str(args.port_offset)]
            if args.artifacts:
                command += ["--artifacts", str(args.artifacts)]
            return subprocess.run(command, cwd=REPO, env=env, check=False).returncode
        finally:
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=20)
            if child_identity is not None:
                try:
                    terminate_verified(*child_identity)
                except OSError as error:
                    if getattr(error, "winerror", None) not in (87, 1168):
                        raise
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    try:
                        if windows_birth(child_identity[0]) != child_identity[1]:
                            break
                    except OSError as error:
                        if getattr(error, "winerror", None) == 87:
                            break
                        raise
                    time.sleep(0.1)
                else:
                    raise RuntimeError("owned mysqld child did not stop")
            if process is not None and process.poll() is None:
                raise RuntimeError("owned mysqld did not stop")
            if args.artifacts and log.exists():
                args.artifacts.mkdir(parents=True, exist_ok=True)
                shutil.copy2(log, args.artifacts / "mysql-server.log")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except EnvironmentBlocked as error:
        print("mysql environment blocked:", error, file=sys.stderr, flush=True)
        raise SystemExit(2)
    except (OSError, ValueError, subprocess.SubprocessError, RuntimeError) as error:
        print("mysql regression failed:", error, file=sys.stderr, flush=True)
        raise SystemExit(1)
