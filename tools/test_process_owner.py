"""Ownership metadata for temporary regression processes (never infer ownership from a port)."""
import ctypes
import json
import os
import posixpath
from pathlib import Path
import re
import tempfile

MARKER = ".oi-test-owner.json"


def normal(path):
    value = str(path).replace("\\", "/").rstrip("/")
    if re.match(r"^[A-Za-z]:/", value):
        value = "/mnt/" + value[0].lower() + value[2:]
    if value:
        value = posixpath.normpath(value)
    return value.lower() if os.name == "nt" else value


def within(path, root):
    path, root = normal(path), normal(root)
    return bool(path and root) and (path == root or path.startswith(root + "/"))


def write_owner(directory, repository, offset):
    (Path(directory) / MARKER).write_text(json.dumps({
        "repository": normal(repository), "offset": offset, "processes": []}), encoding="utf-8")


def read_owner(directory, roots, offset=None):
    try:
        data = json.loads((Path(directory) / MARKER).read_text(encoding="utf-8"))
        if not isinstance(data, dict) or not isinstance(data.get("repository"), str):
            return None
        if any(within(data["repository"], root) for root in roots) and (offset is None or data["offset"] == offset):
            return data
    except (OSError, ValueError, KeyError, TypeError):
        pass
    return None


def owned_path(path, roots, offset=None):
    if not path or not Path(path).is_absolute():
        return False
    if any(within(path, root) for root in roots):
        return True
    value = Path(path)
    return any(read_owner(parent, roots, offset) for parent in (value, *value.parents))


def windows_process(pid, expected_birth=None):
    """Read identity, or terminate that identity using the same open process handle."""
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.GetProcessTimes.argtypes = [ctypes.c_void_p] + [ctypes.c_void_p] * 4
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel.OpenProcess(0x1000 | (1 if expected_birth is not None else 0), False, pid)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        times = [ctypes.c_ulonglong() for _ in range(4)]
        if not kernel.GetProcessTimes(handle, *(ctypes.byref(value) for value in times)):
            raise ctypes.WinError(ctypes.get_last_error())
        if expected_birth is not None and times[0].value == expected_birth:
            kernel.TerminateProcess.argtypes = [ctypes.c_void_p, ctypes.c_uint]
            if not kernel.TerminateProcess(handle, 1):
                raise ctypes.WinError(ctypes.get_last_error())
        return times[0].value
    finally:
        kernel.CloseHandle(handle)


def windows_birth(pid):
    return windows_process(pid)


def terminate_verified(pid, expected_birth):
    return windows_process(pid, expected_birth) == expected_birth


def record_process(directory, proc):
    try:
        identity = ({"birth": windows_birth(proc.pid)} if os.name == "nt"
                    else {"start": linux_start(proc.pid)})
    except OSError:
        if proc.poll() is not None:
            return
        raise
    path = Path(directory) / MARKER
    data = json.loads(path.read_text(encoding="utf-8"))
    data["processes"].append({"pid": proc.pid, **identity})
    path.write_text(json.dumps(data), encoding="utf-8")


def linux_start(pid):
    return int((Path("/proc") / str(pid) / "stat").read_text().rsplit(")", 1)[1].split()[19])


def process_records(roots, offset, identity_key):
    records = {}
    for directory in Path(tempfile.gettempdir()).glob("oi-*"):
        data = read_owner(directory, roots, offset)
        if data:
            items = data.get("processes", [])
            if not isinstance(items, list):
                continue
            for item in items:
                if (isinstance(item, dict) and type(item.get("pid")) is int and item["pid"] > 0
                        and type(item.get(identity_key)) is int and item[identity_key] > 0):
                    records[item["pid"]] = item[identity_key]
    return records


def windows_records(roots, offset=None):
    return process_records(roots, offset, "birth")


def linux_records(roots, offset=None):
    return process_records(roots, offset, "start")
