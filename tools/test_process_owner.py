"""Ownership metadata for temporary regression processes (never infer ownership from a port)."""
import ctypes
from ctypes import wintypes
import datetime
import json
import os
import posixpath
from pathlib import Path
import re
import sys
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


def note_cleanup_step(directory, label, error):
    """Append one cleanup step to the owner marker. Missing marker is skipped; recording never replaces the cleanup result."""
    path = Path(directory) / MARKER
    if not path.is_file():
        return
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        steps = data.get("cleanup")
        if not isinstance(data, dict) or (steps is not None and not isinstance(steps, list)):
            return
        if steps is None:
            steps = []
            data["cleanup"] = steps
        steps.append({"label": str(label), "ok": error is None, "error": None if error is None else repr(error)})
        path.write_text(json.dumps(data), encoding="utf-8")
    except Exception as record_error:
        print("cleanup step not recorded:", label, repr(record_error), file=sys.stderr, flush=True)


def _owner_marker(path):
    current = Path(path)
    for parent in (current, *current.parents):
        marker = parent / MARKER
        if not marker.is_file():
            continue
        try:
            data = json.loads(marker.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return parent, None
        return parent, data if isinstance(data, dict) else None
    return None, None


def _process_state(pid, recorded_birth):
    state = {"pid": pid, "recorded_birth": recorded_birth}
    if os.name != "nt" or type(pid) is not int or pid <= 0:
        state["state"] = "unchecked"
        return state
    try:
        birth = windows_birth(pid)
    except OSError as error:
        state["state"] = "open-failed"
        state["error"] = repr(error)
        return state
    state["birth"] = birth
    state["state"] = "alive" if recorded_birth is None or birth == recorded_birth else "pid-reused"
    if state["state"] != "alive":
        return state
    try:
        state["image"] = _windows_image(pid)
    except OSError as error:
        state["image_error"] = repr(error)
    return state


def _windows_image(pid):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.QueryFullProcessImageNameW.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_ulong)]
    kernel.QueryFullProcessImageNameW.restype = ctypes.c_int
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        size = ctypes.c_ulong(32768)
        buffer = ctypes.create_unicode_buffer(size.value)
        if not kernel.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
            raise ctypes.WinError(ctypes.get_last_error())
        return buffer.value
    finally:
        kernel.CloseHandle(handle)


def _restart_manager_holders(path):
    """Processes Restart Manager reports for this path. One query, no retry loop."""
    app_name = 255
    service_name = 63
    session_key = 33

    class FileTime(ctypes.Structure):
        _fields_ = [("dwLowDateTime", wintypes.DWORD), ("dwHighDateTime", wintypes.DWORD)]

    class UniqueProcess(ctypes.Structure):
        _fields_ = [("dwProcessId", wintypes.DWORD), ("ProcessStartTime", FileTime)]

    class ProcessInfo(ctypes.Structure):
        _fields_ = [
            ("Process", UniqueProcess),
            ("strAppName", wintypes.WCHAR * (app_name + 1)),
            ("strServiceShortName", wintypes.WCHAR * (service_name + 1)),
            ("ApplicationType", wintypes.DWORD),
            ("AppStatus", wintypes.ULONG),
            ("TSSessionId", wintypes.DWORD),
            ("bRestartable", wintypes.BOOL),
        ]

    library = ctypes.WinDLL("Rstrtmgr", use_last_error=True)
    library.RmStartSession.argtypes = [ctypes.POINTER(wintypes.DWORD), wintypes.DWORD, ctypes.c_wchar_p]
    library.RmStartSession.restype = wintypes.DWORD
    library.RmRegisterResources.argtypes = [wintypes.DWORD, wintypes.UINT, ctypes.POINTER(ctypes.c_wchar_p),
                                            wintypes.UINT, ctypes.c_void_p, wintypes.UINT, ctypes.c_void_p]
    library.RmRegisterResources.restype = wintypes.DWORD
    library.RmGetList.argtypes = [wintypes.DWORD, ctypes.POINTER(wintypes.UINT), ctypes.POINTER(wintypes.UINT),
                                  ctypes.c_void_p, ctypes.POINTER(wintypes.DWORD)]
    library.RmGetList.restype = wintypes.DWORD
    library.RmEndSession.argtypes = [wintypes.DWORD]
    library.RmEndSession.restype = wintypes.DWORD
    session = wintypes.DWORD()
    key = ctypes.create_unicode_buffer(session_key)
    report = {"start": library.RmStartSession(ctypes.byref(session), 0, key), "holders": []}
    if report["start"] != 0:
        return report
    try:
        files = (ctypes.c_wchar_p * 1)(str(path))
        report["register"] = library.RmRegisterResources(session, 1, files, 0, None, 0, None)
        needed = wintypes.UINT(0)
        count = wintypes.UINT(0)
        reason = wintypes.DWORD()
        report["list"] = library.RmGetList(session, ctypes.byref(needed), ctypes.byref(count), None, ctypes.byref(reason))
        if report["list"] == 234 and needed.value:
            found = (ProcessInfo * needed.value)()
            count = wintypes.UINT(needed.value)
            report["list"] = library.RmGetList(session, ctypes.byref(needed), ctypes.byref(count),
                                               ctypes.byref(found), ctypes.byref(reason))
            if report["list"] == 0:
                for index in range(count.value):
                    info = found[index]
                    birth = (int(info.Process.ProcessStartTime.dwHighDateTime) << 32) | int(info.Process.ProcessStartTime.dwLowDateTime)
                    report["holders"].append({
                        "pid": int(info.Process.dwProcessId),
                        "birth": birth,
                        "name": str(info.strAppName),
                        "service": str(info.strServiceShortName),
                        "type": int(info.ApplicationType),
                    })
        report["reboot_reason"] = int(reason.value)
    except Exception as error:
        report["error"] = repr(error)
    finally:
        report["end"] = library.RmEndSession(session)
    return report


def _exclusive_open_probe(path):
    """Try one sharing-none open and close it immediately. Success means no other handle was required to open it."""
    try:
        if Path(path).is_dir():
            return {"opened": False, "skipped": "directory"}
    except OSError as error:
        return {"error": repr(error)}
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                   wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    handle = kernel.CreateFileW(str(path), 0x80000000, 0, None, 3, 0, None)
    if handle == wintypes.HANDLE(-1).value:
        return {"opened": False, "winerror": ctypes.get_last_error()}
    kernel.CloseHandle(handle)
    return {"opened": True, "winerror": 0}


def windows_file_holders(path):
    """Holder snapshot for one path. Never raises; a query failure stays in the returned record."""
    if os.name != "nt":
        return {"holders": [], "error": "not-windows"}
    try:
        report = _restart_manager_holders(path)
    except Exception as error:
        report = {"holders": [], "error": repr(error)}
    try:
        report["exclusive_open"] = _exclusive_open_probe(path)
    except Exception as error:
        report["exclusive_open"] = {"error": repr(error)}
    return report


def load_owner(directory):
    """Read the owner marker before cleanup deletes it. Returns None when it is missing or unreadable."""
    try:
        marker = Path(directory) / MARKER
        if not marker.is_file():
            return None
        data = json.loads(marker.read_text(encoding="utf-8"))
    except (OSError, ValueError, TypeError):
        return None
    return data if isinstance(data, dict) else None


def delete_denial_record(path, error, root=None, data=None):
    """Write the holder snapshot at a failed delete. Printing and writing must not replace the original exception."""
    try:
        if root is None:
            root, data = _owner_marker(path)
        else:
            root = Path(root)
        processes = data.get("processes") if isinstance(data, dict) else None
        steps = data.get("cleanup") if isinstance(data, dict) else None
        holders = windows_file_holders(path) if os.name == "nt" else {"holders": [], "error": "not-windows"}
        record = {
            "time": datetime.datetime.now().astimezone().isoformat(timespec="milliseconds"),
            "path": str(path),
            "winerror": getattr(error, "winerror", None),
            "errno": getattr(error, "errno", None),
            "error": repr(error),
            "pid": os.getpid(),
            "holders": holders,
            "owner_root": None if root is None else str(root),
            "owner_processes": [_process_state(item.get("pid"), item.get("birth"))
                                for item in processes if isinstance(item, dict)] if isinstance(processes, list) else None,
            "cleanup": steps if isinstance(steps, list) else None,
        }
        print("delete denial " + json.dumps(record), file=sys.stderr, flush=True)
        if root is not None:
            destination = root.parent / (root.name + ".delete-denial.json")
            existing = []
            if destination.is_file():
                try:
                    loaded = json.loads(destination.read_text(encoding="utf-8"))
                    if isinstance(loaded, list):
                        existing = loaded
                except (OSError, ValueError):
                    existing = []
            existing.append(record)
            destination.write_text(json.dumps(existing), encoding="utf-8")
    except Exception as capture_error:
        print("delete denial capture failed:", repr(capture_error), file=sys.stderr, flush=True)
