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


def windows_install_text():
    return (
        "安装：打开 Visual Studio Installer，修改 Visual Studio 2022，勾选工作负载「使用 C++ 的桌面开发」。\n"
        "该工作负载包含 MSVC v143、MSBuild、Windows SDK 和 CMake。\n"
        "只有 cl.exe、没有 SDK 的 include/lib 时，CMake 仍会失败。"
    )


def linux_install_text():
    return (
        "安装：\n"
        "  sudo apt-get update\n"
        "  sudo apt-get install -y gcc-13 g++-13\n"
        "若源里没有 gcc-13：\n"
        "  sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test\n"
        "  sudo apt-get update\n"
        "  sudo apt-get install -y gcc-13 g++-13"
    )


def cmake_missing_message():
    if WINDOWS:
        return "缺少：CMake\n" + windows_install_text() + "\n也可以单独安装 CMake 并加入 PATH，或给矩阵传 --cmake <cmake.exe>。"
    return (
        "缺少：CMake\n"
        "安装：sudo apt-get install -y cmake\n"
        "或把已有 cmake 放到 PATH，或 $HOME/opt/cmake/bin，或给矩阵传 --cmake。"
    )


def missing_toolset(windows):
    # 配置前就能确定的缺口。CMake 自己报出来的缺口由 explain_configure_failure 再补一句。
    if windows:
        base = Path("C:/Program Files/Microsoft Visual Studio/2022")
        roots = [path for path in base.iterdir() if path.is_dir()] if base.is_dir() else []
        has_msvc = False
        has_msbuild = False
        for root in roots:
            tools = root / "VC" / "Tools" / "MSVC"
            if tools.is_dir() and any(tools.iterdir()):
                has_msvc = True
            if (root / "MSBuild" / "Current" / "Bin" / "MSBuild.exe").is_file():
                has_msbuild = True
        has_sdk = Path("C:/Program Files (x86)/Windows Kits/10/Include").is_dir()
        missing = []
        if not has_msvc:
            missing.append("MSVC v143（VC/Tools/MSVC）")
        if not has_msbuild:
            missing.append("MSBuild")
        if not has_sdk:
            missing.append("Windows SDK（Windows Kits 10）")
        if not missing:
            return ""
        return (
            "缺少编译工具集：" + "、".join(missing) + "\n"
            "CMake 使用生成器 Visual Studio 17 2022、平台 x64。\n"
            + windows_install_text()
        )
    missing = [name for name in ("gcc-13", "g++-13") if not shutil.which(name)]
    if not missing:
        return ""
    return (
        "缺少编译工具集：" + "、".join(missing) + "\n"
        "CMake 配置指定 -DCMAKE_C_COMPILER=gcc-13 -DCMAKE_CXX_COMPILER=g++-13。\n"
        + linux_install_text()
    )


def explain_configure_failure(log_text, windows):
    lowered = log_text.lower()
    markers = (
        "could not find any instance of visual studio",
        "no cmake_c_compiler could be found",
        "no cmake_cxx_compiler could be found",
        "cmake_cxx_compiler-notfound",
        "cmake_c_compiler-notfound",
        "compiler identification is unknown",
        "missing toolset",
        "the build tools for v143",
        "msb8020",
        "was not found in the path",
        "gcc-13: not found",
        "g++-13: not found",
        "gcc-13: no such file",
        "g++-13: no such file",
    )
    if not any(marker in lowered for marker in markers):
        return ""
    detail = missing_toolset(windows)
    if detail:
        return detail
    if windows:
        return "CMake 没有找到 Visual Studio 17 2022 的 C++ 工具集。\n" + windows_install_text()
    return "CMake 没有找到 gcc-13/g++-13。\n" + linux_install_text()


def find_cmake(explicit):
    if explicit:
        return explicit
    if shutil.which("cmake"):
        return shutil.which("cmake")
    candidates = list(Path("C:/Program Files/Microsoft Visual Studio/2022").glob(
        "*/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe")) if WINDOWS else []
    if candidates:
        return str(candidates[0])
    message = cmake_missing_message()
    print(message, flush=True)
    raise RuntimeError(message)


def find_redis(explicit):
    # 不指定就用 PATH 里有的那个。
    return explicit or shutil.which("redis-server") or "/usr/local/bin/redis-server"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=("native", "both"), default="native")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--linux-build-dir", default="/root/openidea-build-gcc13")
    parser.add_argument("--cmake")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--redis-binary", help="default: redis-server on PATH")
    parser.add_argument("--windows-redis-binary", help="--platform both: native Windows redis-server.exe for the Windows side")
    parser.add_argument("--redis-wsl", action="store_true")
    parser.add_argument("--wsl-user", default="root")
    parser.add_argument("--artifacts", type=Path)
    parser.add_argument("--case", default="all")
    parser.add_argument("--port-offset", type=int, default=0, help="passed to run_regressions.py; parallel work copies need distinct offsets")
    parser.add_argument("--mode", choices=("all", "mt", "st"), default="all")
    parser.add_argument("--asan", action="store_true", help="Linux pure coroutine ASan check")
    parser.add_argument("--clock-faults", action="store_true", help="Linux optional wall-clock fault cases")
    parser.add_argument("--vs-instance", help="Optional CMake generator instance: path[,version=x.y.z.w]")
    parser.add_argument("--no-restore", action="store_true",
                        help="dedicated remote single-thread Linux host only: keep the ST build, skip rebuilding MT")
    parser.add_argument("--mysqld", help="Linux: build with OI_ENABLE_MYSQL=ON and run the mysql case before each mode's tests")
    parser.add_argument("--define", action="append", default=[], metavar="NAME=VALUE",
                        help="extra CMake cache entry for every configure, e.g. OI_MYSQL_LIBRARY=/opt/mysql/lib/libmysqlclient.so")
    args = parser.parse_args()
    if args.mysqld and WINDOWS and args.platform == "native":
        parser.error("--mysqld runs on the Linux side only")
    if any("=" not in item for item in args.define):
        parser.error("--define expects NAME=VALUE")
    # 本机和 WSL 的 deploy/bin 还要给别的测试用，必须编回多线程；只有固定单线程的远程机器可以不编回。
    if args.no_restore and (WINDOWS or args.platform != "native" or args.mode != "st"):
        parser.error("--no-restore is only for --platform native --mode st on a dedicated Linux host")
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    output = (args.artifacts or Path.home() / "openidea-test-results" / stamp).resolve()
    output.mkdir(parents=True, exist_ok=True)
    # Invalidate an earlier successful run before any fallible setup.
    summary = {"passed": False, "artifacts": str(output)}
    summary_path = output / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    (output / "matrix.json").write_text("[]", encoding="utf-8")
    (output / "error.txt").unlink(missing_ok=True)
    if args.platform == "both":
        if not WINDOWS:
            parser.error("--platform both is launched on the Windows host")
        native = [sys.executable, str(Path(__file__).resolve()), "--artifacts", str(output / "windows"),
                  "--case", args.case, "--mode", args.mode, "--jobs", str(args.jobs), "--wsl-user", args.wsl_user,
                  "--port-offset", str(args.port_offset)]
        if args.windows_redis_binary or args.redis_binary:
            native += ["--redis-binary", args.windows_redis_binary or args.redis_binary]
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
                          "--jobs", str(args.jobs), "--port-offset", str(args.port_offset)]
        if args.redis_binary:
            linux += ["--redis-binary", args.redis_binary]
        if args.asan:
            linux.append("--asan")
        if args.clock_faults:
            linux.append("--clock-faults")
        if args.mysqld:
            linux += ["--mysqld", args.mysqld]
        for item in args.define:
            linux += ["--define", item]
        codes = [subprocess.run(command, cwd=ROOT).returncode for command in (native, linux)]
        (output / "platforms.json").write_text(json.dumps(dict(zip(("windows", "linux"), codes)), indent=2))
        summary["passed"] = not any(codes)
        summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
        return int(any(codes))

    args.redis_binary = find_redis(args.redis_binary)
    records = []
    def run(name, command):
        print("START", name, flush=True)
        env = os.environ.copy()
        if name == "test-safety" and not args.redis_wsl:
            # 不解符号链接：发行版的 redis-server 指向 redis-check-rdb，按调用名决定身份。
            env["OI_SAFETY_REDIS_BINARY"] = str(Path(args.redis_binary).absolute())
        with (output / (name + ".log")).open("w", encoding="utf-8") as log:
            code = subprocess.run([str(part) for part in command], cwd=ROOT, stdout=log,
                                  stderr=subprocess.STDOUT, env=env).returncode
        records.append({"step": name, "command": [str(part) for part in command], "exit_code": code})
        (output / "matrix.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
        log_path = output / (name + ".log")
        if code != 0 and (name.endswith("-configure") or name.endswith("-build")):
            hint = explain_configure_failure(log_path.read_text(encoding="utf-8", errors="replace"), WINDOWS)
            if hint:
                print(hint, flush=True)
                with log_path.open("a", encoding="utf-8") as log:
                    log.write("\n" + hint + "\n")
        print("PASS" if code == 0 else "FAIL", name, "log=", log_path, flush=True)
        return code

    metadata = {"platform": platform.platform(), "python": sys.version, "root": str(ROOT)}
    # 拷到 /root 下的树所有权对不上时，git 会拒绝读。只对这一次命令放行，不写 git 配置。
    safe = ["-c", "safe.directory=" + str(ROOT)]
    metadata["head"] = subprocess.check_output(["git", *safe, "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    diff = subprocess.check_output(["git", *safe, "-c", "core.safecrlf=false", "diff", "HEAD"], cwd=ROOT)
    metadata["diff_sha256"] = hashlib.sha256(diff).hexdigest()
    metadata["status"] = subprocess.check_output(["git", *safe, "status", "--short"], cwd=ROOT, text=True)
    (output / "environment.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    failed = False
    configured = False
    completed = False
    restored = False
    pending = None
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
        if args.mysqld:
            configure.append("-DOI_ENABLE_MYSQL=ON")
        configure += ["-D" + item for item in args.define]
        build_command = [cmake, "--build", build, "--config", "Release", "-j", args.jobs]
        binary = ROOT / "deploy/bin/Release" if WINDOWS else ROOT / "deploy/bin"
        if run("field-names", [sys.executable, ROOT / "tools/test_field_names.py"]):
            failed = True
        if run("inheritance-tests", [sys.executable, ROOT / "tools/test_inheritance.py"]):
            failed = True
        if run("inheritance", [sys.executable, ROOT / "tools/check_inheritance.py"]):
            failed = True
        if run("process-fixtures", [sys.executable, ROOT / "tools/test_regression_platform.py"]):
            failed = True
        if run("test-safety", [sys.executable, ROOT / "tools/test_test_safety.py"]):
            failed = True
        if run("matrix-outcomes", [sys.executable, ROOT / "tools/test_matrix_outcomes.py"]):
            failed = True
        gap = missing_toolset(WINDOWS)
        if gap:
            print(gap, flush=True)
            (output / "toolchain.txt").write_text(gap, encoding="utf-8")
            failed = True
        for mode in () if gap else (("mt", "st") if args.mode == "all" else (args.mode,)):
            configured = True
            flags = ["-DOI_IPC_SINGLE_THREAD=" + ("ON" if mode == "st" else "OFF"),
                     "-DOI_ENABLE_TEST_HOOKS=" + ("ON" if WINDOWS else "OFF")]
            if run(mode + "-configure", configure + flags) or run(mode + "-build", build_command):
                failed = True
                break
            if args.mysqld:
                failed |= bool(run(mode + "-mysql", [sys.executable, ROOT / "tools/run_mysql_regression.py",
                                                     "--mysqld", args.mysqld, "--bin-dir", binary,
                                                     "--artifacts", output / mode / "mysql",
                                                     "--port-offset", args.port_offset]))
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
        completed = not failed
    except BaseException as error:
        failed = True
        pending = error
        print("ERROR", repr(error), flush=True)
    finally:
        try:
            if configured and not args.no_restore:
                code = run("restore-configure", configure + ["-DOI_IPC_SINGLE_THREAD=OFF", "-DOI_ENABLE_TEST_HOOKS=OFF"])
                if not code:
                    restored = run("restore-build", build_command) == 0
        except BaseException as error:
            print("RESTORE ERROR", repr(error), flush=True)
            summary["restore_error"] = repr(error)
            if pending is None:
                pending = error
        finally:
            summary.update(passed=completed and (restored or args.no_restore), completed=completed, restored=restored,
                           restore_skipped=args.no_restore)
            if pending is not None:
                summary["error"] = repr(pending)
            summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    if pending is not None:
        (output / "error.txt").write_text(repr(pending), encoding="utf-8")
        if not isinstance(pending, Exception):
            raise pending
    return int(not summary["passed"])


if __name__ == "__main__":
    raise SystemExit(main())
