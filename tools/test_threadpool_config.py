"""Check that an old-only ThreadPool configuration is rejected in MT builds."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

from regression_platform import SUFFIX, WINDOWS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    text = (root / "deploy/config/ipcserver.toml").read_text(encoding="utf-8-sig")
    text, count = re.subn(r"(?m)^threadcount\s*=.*$", "normal = 3", text)
    assert count == 1
    with tempfile.TemporaryDirectory(prefix="oi-threadpool-config-") as directory:
        config = Path(directory) / "ipcserver.toml"
        config.write_text(text, encoding="utf-8")
        result = subprocess.run([str((args.bin_dir / ("ipcserver" + SUFFIX)).resolve()), str(config)],
                                cwd=args.bin_dir, capture_output=True, text=True, errors="replace", timeout=15)
    output = result.stdout + result.stderr
    expected = 0xFFFFFFFA if WINDOWS else 250
    assert result.returncode == expected, (result.returncode, output)
    assert "key=ThreadPool.threadcount" in output, output
    assert "function=Init" in output and "oi_thread.cpp" in output, output
    assert re.search(r"line=[1-9][0-9]*\b", output), output
    assert "code=-6" in output, output
    assert "StartFramework failed: code=-6" in output, output
    assert "ipc server listen" not in output, output
    print("old ThreadPool.normal only rejected PASS")


if __name__ == "__main__":
    main()
