"""Build and run config macro checks; the invalid int return must fail to compile."""
import argparse
import subprocess
import tempfile
from pathlib import Path

import test_matrix


ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake")
    args = parser.parse_args()
    cmake = test_matrix.find_cmake(args.cmake)
    with tempfile.TemporaryDirectory(prefix="oi-config-macros-") as directory:
        temp = Path(directory)
        includes = ["cbaselib/base/inc", "cbaselib/timer/inc", "cbaselib/include",
                    "cbaselib/3rd/cpptoml", "cbaselib/3rd/libuv/include", "cbaselib/port"]
        if test_matrix.WINDOWS:
            includes.append("cbaselib/port/win")
        paths = " ".join('"%s"' % (ROOT / item).as_posix() for item in includes)
        source = (ROOT / "tools/config_macro_test.cpp").as_posix()
        config_source = (ROOT / "cbaselib/base/src/oi_config.cpp").as_posix()
        (temp / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.16)\nproject(config_macro_test LANGUAGES CXX)\n'
            'set(CMAKE_CXX_STANDARD 20)\n'
            'add_executable(config_test "%s" "%s")\n'
            'target_include_directories(config_test PRIVATE %s)\n'
            'add_library(invalid OBJECT "%s")\n'
            'target_include_directories(invalid PRIVATE %s)\n'
            'target_compile_definitions(invalid PRIVATE OI_CONFIG_TEST_INVALID_RETURN)\n'
            'if(MSVC)\nadd_compile_options(/utf-8)\n'
            'target_compile_options(config_test PRIVATE /utf-8)\n'
            'target_compile_options(invalid PRIVATE /utf-8)\n'
            'else()\ntarget_compile_options(config_test PRIVATE -ffunction-sections -fdata-sections)\n'
            'target_link_options(config_test PRIVATE -Wl,--gc-sections)\nendif()\n'
            % (source, config_source, paths, source, paths), encoding="utf-8")
        (temp / "config.toml").write_text('value = 42\ntext = "wrong"\nbad_array = ["wrong", "type"]\ngood_array = [1, 2]\ntoo_large = 4294967297\nnegative = -1\nbad_int_array = [1, 4294967297]\n', encoding="utf-8")
        (temp / "invalid.toml").write_text("value = 99\nbroken = [\n", encoding="utf-8")
        for mode in ("Debug", "Release"):
            build = temp / ("build-" + mode.lower())
            subprocess.run([cmake, "-S", str(temp), "-B", str(build), "-DCMAKE_BUILD_TYPE=" + mode], check=True)
            subprocess.run([cmake, "--build", str(build), "--target", "config_test", "--config", mode], check=True)
            executable = build / mode / "config_test.exe" if test_matrix.WINDOWS else build / "config_test"
            subprocess.run([str(executable), str(temp / "config.toml"), str(temp / "invalid.toml")], check=True)
            print(mode, "config macros PASS", flush=True)
        result = subprocess.run([cmake, "--build", str(build), "--target", "invalid", "--config", "Release"],
                                capture_output=True, text=True, encoding="utf-8", errors="replace")
        if result.returncode == 0 or not any(word in result.stdout + result.stderr for word in ("deleted", "C2280")):
            raise RuntimeError("invalid return did not fail as expected:\n" + result.stdout + result.stderr)
        print("int return rejected PASS", flush=True)


if __name__ == "__main__":
    main()
