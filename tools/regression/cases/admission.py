"""rpc-admission 组：服务端在途请求配额。"""
import re

from ..config import set_test_flags
from ..probes import check_admission_rounds
from . import Case
from .hold import (configure_hold_client, hold_environment, poll_registry_fault,
                   prepare_hold_environment, start_hold_gameservers)


class AdmissionCase(Case):
    def configure(self, ctx, name, text):
        case = ctx.name
        if name == "ipcserver" and case.startswith("rpc-admission-"):
            # 只匹配真正的段头，配置注释也可能提到 [Rpc]。
            text = re.sub(r"(?ms)^\[Rpc\]\s*\n.*?(?=^\[|\Z)", "", text)
        if name == "ipcclient":
            text = set_test_flags(text, ctx.mode)
            if ctx.mode == "hold":
                text = configure_hold_client(text)
        return text

    def prepare(self, ctx):
        if hold_environment(ctx):
            prepare_hold_environment(ctx)
        if ctx.name.startswith("rpc-admission-"):
            with (ctx.cfg / "ipcclient.toml").open("a", encoding="utf-8") as config:
                config.write("rpc_admission = true\nrpc_admission_bytes = %s\n" % ("true" if ctx.name.endswith("bytes") else "false"))
            with (ctx.cfg / "ipcserver.toml").open("a", encoding="utf-8") as config:
                config.write("\n[Rpc]\nmax_inflight_count = %d\nsession_max_inflight_count = %d\nmax_inflight_bytes = %d\nsession_max_inflight_bytes = %d\n" % ((100, 100, 64, 32) if ctx.name.endswith("bytes") else (2, 1, 1048576, 1048576)))

    def scenario(self, ctx):
        if hold_environment(ctx):
            start_hold_gameservers(ctx)

    def on_poll(self, ctx):
        if hold_environment(ctx):
            poll_registry_fault(ctx)

    def check(self, ctx):
        text = ctx.text
        if ctx.name.startswith("rpc-admission-"):
            assert "hold rpc admission done bad=0" in text
            check_admission_rounds(text)
        return False

CASES = (AdmissionCase("rpc-admission-count", "admission"), AdmissionCase("rpc-admission-bytes", "admission"))
