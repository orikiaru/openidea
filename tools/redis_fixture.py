"""WSL-side owned Redis fixture for Windows tests. Never connects to an existing DB."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from test_process_owner import MARKER, record_process
import shutil

binary, source = sys.argv[1], Path(sys.argv[2])
with tempfile.TemporaryDirectory(prefix="oi-windows-redis-") as directory:
    config = Path(directory) / "redis.conf"
    if (source.parent / MARKER).is_file():
        shutil.copy2(source.parent / MARKER, Path(directory) / MARKER)
    text = source.read_text(encoding="utf-8")
    text = re.sub(r"(?m)^dir .*$", "dir " + directory, text)
    config.write_text(text, encoding="utf-8")
    proc = subprocess.Popen([binary, str(config)], cwd=directory)
    pidfile = Path(str(source) + ".pid")
    try:
        if (Path(directory) / MARKER).is_file():
            record_process(directory, proc)
        pidfile.write_text(str(proc.pid))
        code = proc.wait()
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        pidfile.unlink(missing_ok=True)
    raise SystemExit(code)
