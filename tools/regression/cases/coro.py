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
            assert "coro live=" in text and "bad=0" in text
        elif ctx.mode == "empty":
            assert re.search(r"empty mode=\S+ total=1000000 ms=\d+ rate=\d+/s rpc_fail=0", text)
        return False

CASES = (CoroCase("coro", "coro"), CoroCase("empty", "coro"))
