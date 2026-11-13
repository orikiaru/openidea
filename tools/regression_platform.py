"""Process and Redis fixtures for the native Windows/Linux regression runner."""
import ctypes
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

WINDOWS = os.name == "nt"
SUFFIX = ".exe" if WINDOWS else ""


def launch(command, **kwargs):
    if WINDOWS:
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        kwargs.update(startupinfo=startup, creationflags=subprocess.CREATE_NEW_CONSOLE)
    else:
        kwargs["start_new_session"] = True
    return subprocess.Popen(command, **kwargs)


def stop(proc):
    if proc.poll() is not None:
        return
    if isinstance(proc, WslRedis):
        proc.terminate()
    elif WINDOWS:
        for attempt in range(10):
            try:
                subprocess.run([sys.executable, str(Path(__file__).resolve()), "--interrupt", str(proc.pid)],
                               check=True, creationflags=subprocess.CREATE_NO_WINDOW, timeout=5,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                break
            except (OSError, subprocess.SubprocessError) as error:
                if proc.poll() is not None:
                    return
                # A newly created console process can still be attaching to its console.
                # Only the helper's explicit AttachConsole ERROR_INVALID_HANDLE is retryable.
                if isinstance(error, subprocess.CalledProcessError) and error.returncode == 6 and attempt < 9:
                    time.sleep(0.05)
                    continue
                print("Windows interrupt helper failed pid=%s returncode=%s stdout=%r stderr=%r error=%r" % (
                    proc.pid, getattr(error, "returncode", None), getattr(error, "stdout", None),
                    getattr(error, "stderr", None), error), file=sys.stderr, flush=True)
                raise
    else:
        proc.terminate()


def pause(proc, suspended):
    if isinstance(proc, WslRedis):
        proc.control("STOP" if suspended else "CONT")
    elif not WINDOWS:
        proc.send_signal(signal.SIGSTOP if suspended else signal.SIGCONT)
    else:
        function = ctypes.windll.ntdll.NtSuspendProcess if suspended else ctypes.windll.ntdll.NtResumeProcess
        function.argtypes = [ctypes.c_void_p]
        function.restype = ctypes.c_long
        status = function(ctypes.c_void_p(int(proc._handle)))
        if status != 0:
            raise OSError("NtSuspend/ResumeProcess failed", status)


def rss_kib(proc):
    if not WINDOWS:
        import re
        return int(re.search(r"VmRSS:\s+(\d+)", Path(f"/proc/{proc.pid}/status").read_text()).group(1))
    class Counters(ctypes.Structure):
        _fields_ = [("cb", ctypes.c_ulong), ("faults", ctypes.c_ulong)] + [
            (name, ctypes.c_size_t) for name in ("peak", "working", "paged_peak", "paged", "nonpaged_peak", "nonpaged", "pagefile", "pagefile_peak")]
    counters = Counters()
    counters.cb = ctypes.sizeof(counters)
    function = ctypes.windll.psapi.GetProcessMemoryInfo
    function.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]
    if not function(ctypes.c_void_p(int(proc._handle)), ctypes.byref(counters), counters.cb):
        raise ctypes.WinError()
    return counters.working // 1024


class WslRedis:
    """Only Redis runs in WSL; the application processes remain native Windows EXEs."""
    def __init__(self, binary, config, user, **kwargs):
        self.prefix = ["wsl", "-u", user, "--exec"]
        def linux_path(path):
            return subprocess.check_output(self.prefix + ["wslpath", "-a", Path(path).as_posix()], text=True).strip()
        self.pidfile = Path(str(config) + ".pid")
        command = self.prefix + ["python3", linux_path(Path(__file__).with_name("redis_fixture.py")),
                                 Path(binary).as_posix(), linux_path(config)]
        self.proc = launch(command, **kwargs)
        self.args = command
        self.pid = self.proc.pid

    def control(self, action):
        pid = int(self.pidfile.read_text())
        if pid <= 1:
            raise RuntimeError("invalid owned Redis PID")
        subprocess.run(self.prefix + ["kill", "-" + action, str(pid)], check=True, timeout=5,
                       creationflags=subprocess.CREATE_NO_WINDOW)

    def poll(self):
        return self.proc.poll()

    @property
    def returncode(self):
        return self.proc.returncode

    def terminate(self):
        self.control("TERM")

    def kill(self):
        self.control("KILL")

    def wait(self, timeout=None):
        return self.proc.wait(timeout=timeout)


if __name__ == "__main__":
    if not WINDOWS or len(sys.argv) != 3 or sys.argv[1] != "--interrupt":
        raise SystemExit("internal Windows console interrupt helper")
    kernel = ctypes.windll.kernel32
    kernel.FreeConsole()
    if not kernel.AttachConsole(int(sys.argv[2])):
        error = ctypes.WinError()
        print("AttachConsole failed: %s" % error, file=sys.stderr, flush=True)
        raise SystemExit(6 if error.winerror == 6 else 1)
    kernel.SetConsoleCtrlHandler(None, True)
    if not kernel.GenerateConsoleCtrlEvent(0, 0):
        raise ctypes.WinError()
    kernel.FreeConsole()
