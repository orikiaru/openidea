"""WSL-side owned Redis fixture for Windows tests. Never connects to an existing DB."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

binary, source = sys.argv[1], Path(sys.argv[2])
with tempfile.TemporaryDirectory(prefix="oi-windows-redis-") as directory:
    config = Path(directory) / "redis.conf"
    text = source.read_text(encoding="utf-8")
    text = re.sub(r"(?m)^dir .*$", "dir " + directory, text)
    config.write_text(text, encoding="utf-8")
    proc = subprocess.Popen([binary, str(config)])
    pidfile = Path(str(source) + ".pid")
    pidfile.write_text(str(proc.pid))
    try:
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
