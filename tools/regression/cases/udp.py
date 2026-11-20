"""udp 组：UDP 会话、队列、合并和 poll 越界探针。"""
import re
import subprocess

from regression_platform import SUFFIX

from .. import ports
from ..config import replace, set_test_flags
from . import Case


class UdpCase(Case):
    def configure(self, ctx, name, text):
        case = ctx.name
        if name == "ipcserver" and case == "udp":
            text = replace(text, "idle_timeout_ms", "1000")
        if name == "ipcclient":
            if case == "udp":
                text = replace(text, "idle_timeout_ms", "4000")
                text = text.replace("[TestUdp]", "[TestUdp]\ncheck_server_idle = true", 1)
            text = set_test_flags(text, ctx.mode)
        return text

    def check(self, ctx):
        text, serverpath, work = ctx.text, ctx.serverpath, ctx.work
        if ctx.mode == "udp":
            assert "ipc udp listen port=%d" % ports.UDP_PORT in serverpath.read_text(encoding="utf-8", errors="replace")
            assert re.search(r"udp local port=[1-9]\d* bad=0", text)
            assert "udp open dedup bad=0" in text
            assert "udp peer addr bad=0" in text
            assert "udp add peer addr bad=0" in text
            assert "udp server sees client port bad=0" in text
            assert "udp rpc calls=1000 bad=0" in text
            assert "udp notify sent=1000 received=1000 duplicate=0 bad=0" in text
            assert "udp protocol error code ret=1001 bad=0" in text
            assert "udp large datagram bytes=60000 bad=0" in text
            assert "udp oversized notify=-6 call=-6 immediate=1 recovered=1 bad=0" in text
            assert "udp garbage keeps session sessions=1 bad=0" in text
            assert "udp active sessions=1 bad=0" in text
            assert "udp server idle client_open=1 sessions=2 bad=0" in text
            assert "udp idle close reopen bad=0" in text
            assert "udp notify rpc receipt=0 bad=0" in text
            assert "udp peers parallel=1 source=1 remaining=1 reopen=1 bad=0" in text
            assert "udp client bind before=-3 output=12345 start=0 sessions=0 bad=0" in text
            assert "udp ipv6 oversized start=0 open=0 send=-6 bad=0" in text
            assert re.search(r"udp reverse peers first=9 second=6 dup=0/0 old=-\d+ bad=0", text)
            assert re.search(r"udp queue packets=1 reopen=1 concurrent=1 workers=(?:0|2) queued=0 requeue=0 old=-\d+ count=2/2 after=1 observe_ms=\d+ bad=0", text)
            assert "udp queue budget first=0 busy=-5 after=0 packets=2 bad=0" in text
            assert "udp merge frames=1 raw=1 peers=1 limit=1 bad=0" in text
            assert "udp datagram max=0 over=-6 received=65507 bad=0" in text
            assert "udp done bad=0" in text
            close_probe = subprocess.run([str(work / ("ipcclient" + SUFFIX)), "--poll-udp-close"],
                                         cwd=work, capture_output=True, text=True, timeout=15)
            close_output = close_probe.stdout + close_probe.stderr
            assert close_probe.returncode == 0 and "poll udp close bound=1 paused=1 loop=0" in close_output, (close_probe.returncode, close_output)
            wrong_probe = subprocess.run([str(work / ("ipcclient" + SUFFIX)), "--poll-wrong-thread"],
                                         cwd=work, capture_output=True, text=True, timeout=15)
            wrong_output = wrong_probe.stdout + wrong_probe.stderr
            if "poll wrong thread probe mt skipped=1" in wrong_output:
                assert wrong_probe.returncode == 0, (wrong_probe.returncode, wrong_output)
            else:
                assert wrong_probe.returncode != 0 and "poll wrong thread probe registered=1" in wrong_output and \
                       "single-thread registered channel unregister outside poll thread channel=" in wrong_output and \
                       "unexpectedly survived" not in wrong_output, (wrong_probe.returncode, wrong_output)
            print("poll udp close probe:", close_output.strip(), flush=True)
            print("poll wrong thread probe exit=%d output=%s" % (wrong_probe.returncode, wrong_output.strip()), flush=True)
            busy_probe = subprocess.run([str(work / ("ipcclient" + SUFFIX)), "--poll-loop-busy"],
                                        cwd=work, capture_output=True, text=True, timeout=15)
            busy_output = busy_probe.stdout + busy_probe.stderr
            assert busy_probe.returncode == 0 and re.search(r"poll loop busy code=-\d+ bad=0", busy_output), (busy_probe.returncode, busy_output)
            assert "poll loop close failed" in busy_output, busy_output
            print("poll loop busy probe:", busy_output.strip(), flush=True)
            print("poll udp close and wrong-thread probe PASS", flush=True)
        return False

CASES = (UdpCase("udp", "udp"),)
