"""coro 组：协程存活和空转吞吐。"""
import re

from ..config import set_test_flags
from . import Case


class CoroCase(Case):
    def configure(self, ctx, name, text):
        if name == "ipcclient":
            text = set_test_flags(text, ctx.mode)
        return text

    def check(self, ctx):
        text = ctx.text
        if ctx.mode == "coro":
            match = re.search(
                r"coro live=(\d+) churn=(\d+) held=(\d+) wake=(\d+) done=(\d+) "
                r"[^\n]*?live_ns=(-?\d+) [^\n]*?wake_ns=(-?\d+) "
                r"[^\n]*?churn_ns=(-?\d+) [^\n]*?bad=(\d+)", text)
            assert match, "missing or malformed coro benchmark result"
            live, churn, held, wake, done, live_ns, wake_ns, churn_ns, bad = map(int, match.groups())
            assert live > 0 and churn > 0, f"invalid coro workload: live={live} churn={churn}"
            assert held == live and wake == live and done == churn, (
                f"coro count mismatch: live={live} held={held} wake={wake} churn={churn} done={done}")
            assert live_ns > 0 and wake_ns > 0 and churn_ns > 0, (
                f"invalid coro timing: live_ns={live_ns} wake_ns={wake_ns} churn_ns={churn_ns}")
            assert bad == 0, f"coro reported bad={bad}"
        elif ctx.mode == "empty":
            assert re.search(r"empty mode=\S+ total=1000000 ms=\d+ rate=\d+/s rpc_fail=0", text)
        return False

CASES = (
    CoroCase("coro", "coro", targeted_reason="monthly coroutine benchmark"),
    CoroCase("empty", "empty", targeted_reason="dedicated empty RPC benchmark"),
)
