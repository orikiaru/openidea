"""Build/test/restore the native ST+MT matrix. See docs/testing.md."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
WINDOWS = os.name == "nt"


def find_cmake(explicit):
    if explicit:
        return explicit
    if shutil.which("cmake"):
        return shutil.which("cmake")
    candidates = list(Path("C:/Program Files/Microsoft Visual Studio/2022").glob(
        "*/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe")) if WINDOWS else []
    if candidates:
        return str(candidates[0])
    raise RuntimeError("CMake not found; pass --cmake or install the C++ toolchain")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=("native", "both"), default="native")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--linux-build-dir", default="/home/jxy/openidea-build-gcc13")
    parser.add_argument("--cmake")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--redis-binary", default="/tmp/redis-7.4.12/src/redis-server")
    parser.add_argument("--windows-redis-binary", help="--platform both: native Windows redis-server.exe for the Windows side")
    parser.add_argument("--redis-wsl", action="store_true")
    parser.add_argument("--wsl-user", default="jxy")
    parser.add_argument("--artifacts", type=Path)
    parser.add_argument("--case", default="all")
    parser.add_argument("--port-offset", type=int, default=0, help="passed to run_regressions.py; parallel work copies need distinct offsets")
    parser.add_argument("--mode", choices=("all", "mt", "st"), default="all")
    parser.add_argument("--asan", action="store_true", help="Linux pure coroutine ASan check")
    parser.add_argument("--clock-faults", action="store_true", help="Linux optional wall-clock fault cases")
    parser.add_argument("--vs-instance", help="Optional CMake generator instance: path[,version=x.y.z.w]")
    args = parser.parse_args()
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    output = (args.artifacts or Path.home() / "openidea-test-results" / stamp).resolve()
    output.mkdir(parents=True, exist_ok=True)
    if args.platform == "both":
        if not WINDOWS:
            parser.error("--platform both is launched on the Windows host")
        native = [sys.executable, str(Path(__file__).resolve()), "--artifacts", str(output / "windows"),
                  "--case", args.case, "--mode", args.mode, "--jobs", str(args.jobs), "--wsl-user", args.wsl_user,
                  "--redis-binary", args.windows_redis_binary or args.redis_binary, "--port-offset", str(args.port_offset)]
        if args.redis_wsl and not args.windows_redis_binary:
            native.append("--redis-wsl")
        if args.cmake:
            native += ["--cmake", args.cmake]
        if args.build_dir:
            native += ["--build-dir", str(args.build_dir)]
        if args.vs_instance:
            native += ["--vs-instance", args.vs_instance]
        prefix = ["wsl", "-u", args.wsl_user, "--exec"]
        translate = lambda path: subprocess.check_output(prefix + ["wslpath", "-a", Path(path).as_posix()], text=True).strip()
        linux = prefix + ["python3", translate(Path(__file__).resolve()), "--build-dir", args.linux_build_dir,
                          "--artifacts", translate(output / "linux"), "--case", args.case, "--mode", args.mode,
                          "--jobs", str(args.jobs), "--redis-binary", args.redis_binary, "--port-offset", str(args.port_offset)]
        if args.asan:
            linux.append("--asan")
        if args.clock_faults:
            linux.append("--clock-faults")
        codes = [subprocess.run(command, cwd=ROOT).returncode for command in (native, linux)]
        (output / "platforms.json").write_text(json.dumps(dict(zip(("windows", "linux"), codes)), indent=2))
        return int(any(codes))

    records = []
    def run(name, command):
        print("START", name, flush=True)
        with (output / (name + ".log")).open("w", encoding="utf-8") as log:
            code = subprocess.run([str(part) for part in command], cwd=ROOT, stdout=log,
                                  stderr=subprocess.STDOUT).returncode
        records.append({"step": name, "command": [str(part) for part in command], "exit_code": code})
        (output / "matrix.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
        print("PASS" if code == 0 else "FAIL", name, "log=", output / (name + ".log"), flush=True)
        return code

    metadata = {"platform": platform.platform(), "python": sys.version, "root": str(ROOT)}
    metadata["head"] = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    diff = subprocess.check_output(["git", "-c", "core.safecrlf=false", "diff", "HEAD"], cwd=ROOT)
    metadata["diff_sha256"] = hashlib.sha256(diff).hexdigest()
    metadata["status"] = subprocess.check_output(["git", "status", "--short"], cwd=ROOT, text=True)
    (output / "environment.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    failed = False
    configured = False
    try:
        cmake = find_cmake(args.cmake)
        build = (args.build_dir or (ROOT / "build-win" if WINDOWS else Path(args.linux_build_dir))).resolve()
        configure = [cmake, "-S", ROOT, "-B", build]
        if WINDOWS:
            configure += ["-G", "Visual Studio 17 2022", "-A", "x64", "-DPYTHON_BIN=" + sys.executable]
            if args.vs_instance:
                configure.append("-DCMAKE_GENERATOR_INSTANCE=" + args.vs_instance)
            else:
                configure.append("-UCMAKE_GENERATOR_INSTANCE")
        else:
            configure += ["-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=gcc-13", "-DCMAKE_CXX_COMPILER=g++-13"]
        build_command = [cmake, "--build", build, "--config", "Release", "-j", args.jobs]
        binary = ROOT / "deploy/bin/Release" if WINDOWS else ROOT / "deploy/bin"
        if run("field-names", [sys.executable, ROOT / "tools/test_field_names.py"]):
            failed = True
        if run("process-fixtures", [sys.executable, ROOT / "tools/test_regression_platform.py"]):
            failed = True
        if run("test-safety", [sys.executable, ROOT / "tools/test_test_safety.py"]):
            failed = True
        for mode in (("mt", "st") if args.mode == "all" else (args.mode,)):
            configured = True
            flags = ["-DOI_IPC_SINGLE_THREAD=" + ("ON" if mode == "st" else "OFF"),
                     "-DOI_ENABLE_TEST_HOOKS=" + ("ON" if WINDOWS else "OFF")]
            if run(mode + "-configure", configure + flags) or run(mode + "-build", build_command):
                failed = True
                break
            test = [sys.executable, ROOT / "tools/run_regressions.py", "--case", args.case,
                    "--bin-dir", binary, "--redis-binary", args.redis_binary, "--artifacts", output / mode,
                    "--port-offset", args.port_offset]
            if args.redis_wsl:
                test += ["--redis-wsl", "--wsl-user", args.wsl_user]
            failed |= bool(run(mode + "-tests", test))
            if args.clock_faults and not WINDOWS:
                library = output / "clock-fault.so"
                if run(mode + "-clock-build", ["gcc-13", "-shared", "-fPIC", ROOT / "tools/clock_fault.c", "-o", library]):
                    failed = True
                else:
                    for case in ("clock-forward", "clock-back"):
                        fault = list(test)
                        fault[fault.index("--case") + 1] = case
                        fault[fault.index("--artifacts") + 1] = output / mode / case
                        fault += ["--clock-fault-so", library]
                        failed |= bool(run(mode + "-" + case, fault))
        if args.asan and not WINDOWS:
            failed |= bool(run("asan", [sys.executable, ROOT / "tools/check_coro_runtime.py", "--build-dir", build, "--asan"]))
    except Exception as error:
        failed = True
        (output / "error.txt").write_text(str(error), encoding="utf-8")
        print("ERROR", error, flush=True)
    finally:
        if configured:
            code = run("restore-configure", configure + ["-DOI_IPC_SINGLE_THREAD=OFF", "-DOI_ENABLE_TEST_HOOKS=OFF"])
            failed |= bool(code)
            if not code:
                failed |= bool(run("restore-build", build_command))
        (output / "summary.json").write_text(json.dumps({"passed": not failed, "artifacts": str(output)}, indent=2))
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
