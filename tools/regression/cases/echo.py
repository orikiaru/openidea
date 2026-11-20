"""echo 组：回声、连接重置、注册发现和 RPC ingress。"""
import re

from regression_platform import SUFFIX, WINDOWS

from .. import ports
from ..config import replace, set_test_flags
from ..probes import check_echo, echo_total, set_echo_total
from ..rpc_faults import reset_connections, rpc_ingress_faults
from . import Case


class EchoCase(Case):
    def configure(self, ctx, name, text):
        case = ctx.name
        if name == "registry" and case == "registry":
            text = replace(text, "expire_ms", "30000")
        if name == "ipcserver" and case == "rpc-ingress":
            text = replace(text, "max_frame_bytes", "65536")
            text = replace(text, "max_receive_buffer_bytes", "65544")
            text = replace(text, "incomplete_frame_timeout_ms", "250")
            text = replace(text, "session_max_inflight_count", "1")
            text += "\n[Regression]\nrpc_read_limit = true\n"
        if name == "ipcclient":
            if case in ("reset", "registry", "rpc-ingress"):
                text = set_echo_total(text, 100)
            if ctx.mode == "echo":
                ctx.expected_echo_total = echo_total(text)
            if case == "registry":
                # 错误的直连后备端口，只有通过注册发现才能完成回声。
                text = text.replace('port = "%d"' % ports.IPC_PORT, 'port = "%d"' % ports.IPC_FALLBACK_PORT)
            text = set_test_flags(text, ctx.mode)
        return text

    def after_redis(self, ctx):
        if ctx.name != "rpc-ingress":
            return
        duplicate_config = ctx.cfg / "ipcserver-duplicate.toml"
        duplicate_config.write_text((ctx.cfg / "ipcserver.toml").read_text(encoding="utf-8")
                                    + "\nrpc_duplicate_handler = true\n", encoding="utf-8")
        duplicate, duplicate_path = ctx.start("ipcserver-duplicate", [str(ctx.work / ("ipcserver" + SUFFIX)), str(duplicate_config)])
        duplicate.wait(timeout=15)
        ctx.procs.remove(duplicate)
        output = duplicate_path.read_text(encoding="utf-8", errors="replace")
        expected = 0xFFFFFFFF if WINDOWS else 255
        assert duplicate.returncode == expected, "duplicate registration exit code=%s\n%s" % (duplicate.returncode, output)
        assert "rpc handler already bound manager=IpcServer proto=3001" in output, output
        assert "module init failed: rpc handler already bound manager=IpcServer proto=3001" in output, output
        assert "ipc server listen" not in output, output
        print("rpc duplicate registration startup failure PASS", flush=True)
        failed_config = ctx.cfg / "ipcserver-init-fail.toml"
        failed_config.write_text((ctx.cfg / "ipcserver.toml").read_text(encoding="utf-8")
                                 + "\nrpc_init_fail = true\n", encoding="utf-8")
        failed, failed_path = ctx.start("ipcserver-init-fail", [str(ctx.work / ("ipcserver" + SUFFIX)), str(failed_config)])
        failed.wait(timeout=15)
        ctx.procs.remove(failed)
        output = failed_path.read_text(encoding="utf-8", errors="replace")
        assert failed.returncode == expected, "init failure exit code=%s\n%s" % (failed.returncode, output)
        assert "IpcServer Init failed: code=-1" in output, output
        assert "pure virtual" not in output and "terminate called" not in output, output
        assert "ipc server listen" not in output, output
        print("rpc init error startup failure PASS", flush=True)

    def scenario(self, ctx):
        case = ctx.name
        if case == "reset":
            reset_connections()
        if case == "rpc-ingress":
            rpc_ingress_faults()
            ctx.stop(ctx.server)
            ctx.server.wait(timeout=10)
            assert ctx.server.returncode == 0, "ingress probe server failed to stop"
            ctx.procs.remove(ctx.server)
            server_config = ctx.cfg / "ipcserver.toml"
            server_config.write_text(replace(server_config.read_text(encoding="utf-8"), "session_max_inflight_count", "4096"), encoding="utf-8")
            ctx.server, ctx.serverpath = ctx.start("ipcserver-echo", [str(ctx.work / ("ipcserver" + SUFFIX))])
            ctx.ready(ctx.server, ctx.serverpath, "ipc server listen")

    def check(self, ctx):
        expected_echo_total = ctx.expected_echo_total
        text = ctx.text
        if ctx.mode == "echo":
            check_echo(text, expected_echo_total)
        return False

    def diagnostics(self, ctx):
        if ctx.name != "rpc-ingress":
            return
        diagnostics = ctx.stop_backends()
        assert re.search(r"rpc failure stage=decode_request token=0 call=424242 sid=\d+ proto=3001 ret=-?\d+ reason=ParserException", diagnostics), "RPC parse context missing from logd disk"
        print("diagnostic original errors persisted", ctx.name, "PASS", flush=True)

CASES = (EchoCase("echo", "echo"), EchoCase("reset", "echo"),
         EchoCase("registry", "echo"), EchoCase("rpc-ingress", "echo"))
