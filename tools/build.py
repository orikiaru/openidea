"""只配置并编译，不跑用例。

Windows 默认 build-win、Visual Studio 17 2022、x64、Release。
Linux 默认 $HOME/openidea-build-gcc13、gcc-13/g++-13、Release。
并行度默认吃满本机逻辑核。单线程只切 ipcclient/ipcserver，编完不会自动编回多线程。

    python tools/build.py
    python tools/build.py --mode st --jobs 16
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

import test_matrix

ROOT = test_matrix.ROOT
WINDOWS = test_matrix.WINDOWS


def default_jobs():
    return os.cpu_count() or 1


def default_build_dir():
    if WINDOWS:
        return ROOT / "build-win"
    return Path.home() / "openidea-build-gcc13"


def configure_command(cmake, build, mode, defines, vs_instance):
    command = [cmake, "-S", str(ROOT), "-B", str(build)]
    if WINDOWS:
        command += ["-G", "Visual Studio 17 2022", "-A", "x64", "-DPYTHON_BIN=" + sys.executable]
        if vs_instance:
            command.append("-DCMAKE_GENERATOR_INSTANCE=" + vs_instance)
        else:
            command.append("-UCMAKE_GENERATOR_INSTANCE")
    else:
        command += ["-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=gcc-13", "-DCMAKE_CXX_COMPILER=g++-13"]
    command.append("-DOI_IPC_SINGLE_THREAD=" + ("ON" if mode == "st" else "OFF"))
    command.append("-DOI_ENABLE_TEST_HOOKS=OFF")
    command += ["-D" + item for item in defines]
    return command


def build_command(cmake, build, jobs):
    command = [cmake, "--build", str(build), "--config", "Release", "-j", str(jobs)]
    if WINDOWS:
        command += ["--", "/m:%d" % jobs, "/p:CL_MPCount=%d" % jobs]
    return command


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("mt", "st"), default="mt")
    parser.add_argument("--jobs", type=int, default=default_jobs())
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--cmake")
    parser.add_argument("--vs-instance")
    parser.add_argument("--define", action="append", default=[], metavar="NAME=VALUE")
    args = parser.parse_args(argv)
    if args.jobs < 1:
        print("jobs must be >= 1", flush=True)
        return 2
    gap = test_matrix.missing_toolset(WINDOWS)
    if gap:
        print(gap, flush=True)
        return 2
    cmake = test_matrix.find_cmake(args.cmake)
    build = (args.build_dir or default_build_dir()).resolve()
    configure = configure_command(cmake, build, args.mode, args.define, args.vs_instance)
    compile_cmd = build_command(cmake, build, args.jobs)
    print("CONFIGURE", " ".join(configure), flush=True)
    code = subprocess.call(configure)
    if code:
        print("CONFIGURE_EXIT", code, flush=True)
        return code
    print("BUILD jobs=%d" % args.jobs, " ".join(compile_cmd), flush=True)
    code = subprocess.call(compile_cmd)
    print("BUILD_EXIT", code, flush=True)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
