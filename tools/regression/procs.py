"""回归临时目录、所有权记录、进程生命周期和收尾。"""
import contextlib
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

from regression_platform import SUFFIX, WINDOWS, launch, pause, stop, WslRedis
from test_process_owner import record_process, write_owner

from . import ports

REPO = Path(__file__).resolve().parents[2]

@contextlib.contextmanager
def regression_directory(**kwargs):
    directory = tempfile.TemporaryDirectory(**kwargs)
    try:
        yield directory.name
    finally:
        primary = sys.exc_info()[1]
        try:
            directory.cleanup()
        except BaseException as error:
            if primary is None:
                raise
            print("temporary directory cleanup failed:", repr(error), file=sys.stderr, flush=True)


def copy_runtime_dlls(bin_dir, work):
    # Windows 只在 exe 同目录找依赖库，例如 MySQL 客户端和它带的 OpenSSL。
    for dll in Path(bin_dir).glob("*.dll"):
        shutil.copy2(dll, work / dll.name)


def cleanup_owned(procs, handles, root, args, case, *, db=None, db_paused=False,
                  monitor=None, http_fixture=None, http_thread=None, primary=None,
                  registry=None, registry_paused=False, allow_nonzero=None):
    """Attempt every owned cleanup action, without masking the original test error."""
    errors = []
    allowed = set(allow_nonzero or ())

    def attempt(label, action):
        try:
            action()
            return True
        except BaseException as error:
            errors.append((label, error))
            return False

    if db_paused:
        attempt("resume Redis", lambda: pause(db, False) if db.poll() is None else None)
    if registry_paused:
        attempt("resume registry", lambda: pause(registry, False) if registry.poll() is None else None)
    for proc in reversed(procs):
        def graceful(proc=proc):
            if proc.poll() is None:
                if proc is db and WINDOWS and not args.redis_wsl:
                    # Only the owned native Redis fixture uses protocol shutdown.
                    try:
                        with socket.create_connection(("127.0.0.1", ports.REDIS_PORT), timeout=2) as conn:
                            conn.sendall(b"*2\r\n$8\r\nSHUTDOWN\r\n$6\r\nNOSAVE\r\n")
                            conn.recv(64)
                    except OSError:
                        pass
                else:
                    stop(proc)
                proc.wait(timeout=3)
            code = proc.returncode
            if code not in (0, None) and proc not in allowed and not (case == "disk-full" and "logd" in str(proc.args)):
                raise RuntimeError("service exited code=%s: %s" % (code, proc.args))

        if not attempt("graceful shutdown " + str(proc.args), graceful):
            # Windows Popen.kill uses the held process handle, not a fresh PID lookup.
            def force(proc=proc):
                if proc.poll() is None:
                    proc.kill()
            attempt("force shutdown " + str(proc.args), force)
            attempt("reap " + str(proc.args), lambda proc=proc: proc.wait(timeout=3))
    if monitor is not None:
        attempt("monitor close", monitor.close)
    if http_fixture is not None:
        attempt("HTTP shutdown", http_fixture.shutdown)
        attempt("HTTP close", http_fixture.server_close)
        def join_http():
            http_thread.join(timeout=3)
            if http_thread.is_alive():
                raise RuntimeError("HTTP fixture thread did not exit")
        attempt("HTTP join", join_http)
    for handle in handles:
        attempt("output close", handle.close)
    if args.artifacts:
        destination = args.artifacts / case
        attempt("artifact directory", lambda: destination.mkdir(parents=True, exist_ok=True))
        def copy_outputs():
            for output in root.glob("*.out"):
                attempt("artifact " + output.name, lambda output=output: shutil.copy2(output, destination / output.name))
        attempt("artifact enumeration", copy_outputs)
    if errors:
        message = "cleanup failed: " + "; ".join(label + ": " + repr(error) for label, error in errors)
        print(message, file=sys.stderr, flush=True)
        if primary is None:
            for _, error in errors:
                if not isinstance(error, Exception):
                    raise error
            raise RuntimeError(message) from errors[0][1]


class Context:
    """一个用例的临时环境：目录、本次持有的进程和轮询状态。"""

    def __init__(self, args, case, root):
        self.args = args
        self.case = case
        self.name = case.name
        self.scene = case.scene
        self.mode = case.mode
        self.fairness = case.fairness
        self.pressure = case.pressure
        self.root = root
        self.work = root / "bin"
        self.cfg = root / "config"
        self.logs = root / "logs"
        self.procs = []
        self.handles = []
        self.log_env = None
        self.logd = None
        self.logpath = None
        self.server = None
        self.serverpath = None
        self.db = None
        self.dbpath = None
        self.db_paused = False
        self.redis_conf = None
        self.registry = None
        self.registry_path = None
        self.registry_paused = False
        self.registry_fault_command = ""
        self.registry_resume_at = None
        self.pause_at = None
        self.shutdown_sent = False
        self.redis_restarted = False
        self.peers = {}
        self.peer_stopped = False
        self.peer_restarted = False
        self.rs = None
        self.rsp = None
        self.client = None
        self.clientpath = None
        self.monitor = None
        self.http_fixture = None
        self.http_thread = None
        self.expected_echo_total = None
        self.maximum_bytes = None
        self.maximum_count = None
        self.started = None
        self.deadline = None
        self.last_growth = None
        self.last_log_bytes = None
        self.rss_before = None
        self.text = ""
        self.timeline_stop = threading.Event()
        self.timeline_thread = None
        self.timeline_lines = []
        self.timeline_errors = []

    def claim(self):
        write_owner(self.root, REPO, ports.PORT_OFFSET)

    def prepare_paths(self):
        for path in (self.work, self.cfg, self.logs):
            path.mkdir()
        copy_runtime_dlls(self.args.bin_dir, self.work)

    def start(self, name, command=None, env=None):
        output = self.root / (name + ".out")
        handle = output.open("w", encoding="utf-8")
        self.handles.append(handle)
        if name == "redis" and self.args.redis_wsl:
            proc = WslRedis(command[0], Path(command[1]), self.args.wsl_user, cwd=self.work,
                            stdout=handle, stderr=subprocess.STDOUT, env=env)
        else:
            proc = launch(command or [str(self.work / (name + SUFFIX))], cwd=self.work,
                          stdout=handle, stderr=subprocess.STDOUT, env=env)
        self.procs.append(proc)
        record_process(self.root, proc)
        return proc, output

    def ready(self, proc, path, marker):
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            text = path.read_text(encoding="utf-8", errors="replace")
            if proc.poll() is not None:
                raise RuntimeError("%s exited code=%s\n%s" % (path.name, proc.returncode, text))
            if marker in text:
                return
            time.sleep(0.05)
        raise RuntimeError("start timeout: " + str(path))

    def stop(self, proc):
        stop(proc)

    def pause(self, proc, paused):
        pause(proc, paused)

    def start_logd(self):
        logd, logpath = self.start("logd", env=self.log_env)
        self.logd, self.logpath = logd, logpath
        # 日志服没有 ready 文本，端口探测只用来确认监听，不发送日志。
        deadline = time.monotonic() + 10
        while True:
            try:
                with socket.create_connection(("127.0.0.1", ports.LOGD_PORT), timeout=0.1):
                    break
            except OSError:
                if logd.poll() is not None or time.monotonic() > deadline:
                    if self.case.logd_start_failure(self, logd):
                        return True
                    raise RuntimeError(logpath.read_text(encoding="utf-8", errors="replace"))
                time.sleep(0.05)
        return False

    def stop_backends(self):
        # 正常退出刷新 stdout，再排空 logd，核对真实落盘而非只有终端输出。
        stop(self.server)
        self.server.wait(timeout=10)
        assert self.server.returncode == 0
        stop(self.logd)
        self.logd.wait(timeout=10)
        assert self.logd.returncode == 0
        return "\n".join(path.read_text(encoding="utf-8", errors="replace")
                         for path in self.logs.rglob("error") if path.is_file())
