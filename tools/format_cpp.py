"""Format tracked, owned C++ sources (clang-format 18.1.8).

python3 tools/format_cpp.py [--check] [--clang-format /path/to/clang-format]
Third-party sources and untracked generated output are excluded.
"""
import argparse
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--clang-format", default="clang-format")
    args = parser.parse_args()
    binary = shutil.which(args.clang_format)
    if not binary:
        parser.error("clang-format 18.1.8 required; install clang-format==18.1.8 or specify --clang-format")
    version = subprocess.check_output([binary, "--version"], text=True)
    if "version 18.1.8" not in version:
        parser.error("use clang-format 18.1.8 for reproducible output: " + version.strip())
    names = subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT).decode("utf-8").split("\0")
    sources = [name for name in names if Path(name).suffix in (".h", ".hpp", ".cpp", ".cxx", ".cc")
               and name.startswith(("cbaselib/", "modules/")) and not name.startswith("cbaselib/3rd/")]
    for start in range(0, len(sources), 40):
        command = [binary, "--style=file"] + (["--dry-run", "--Werror"] if args.check else ["-i"])
        subprocess.run(command + sources[start:start + 40], cwd=ROOT, check=True)
    print("C++ format", "checked" if args.check else "updated", len(sources), "files")


if __name__ == "__main__":
    main()
