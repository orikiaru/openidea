"""echo 组：回声、连接重置、注册发现和 RPC ingress。"""
import re

from regression_platform import SUFFIX, WINDOWS

from .. import ports
from ..config import replace, set_test_flags
from ..probes import check_echo, echo_total, set_echo_total
from ..rpc_faults import reset_connections, rpc_default_receive_boundary, rpc_ingress_faults
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
        publish_config = ctx.cfg / "ipcserver-publish-fail.toml"
        publish_text = (ctx.cfg / "ipcserver.toml").read_text(encoding="utf-8")
        publish_text = publish_text.replace('name = "ipcserver"', 'name = "ipc.server"', 1)
        publish_config.write_text(publish_text, encoding="utf-8")
        publish, publish_path = ctx.start("ipcserver-publish-fail", [str(ctx.work / ("ipcserver" + SUFFIX)), str(publish_config)])
        publish.wait(timeout=15)
        ctx.procs.remove(publish)
        output = publish_path.read_text(encoding="utf-8", errors="replace")
        # 监听成功后登记名字非法。退出码是 OI_ERR_INVALID_ARGUMENT。随后的正式 ipcserver 还要绑上同一端口。
        expected_publish = (0x100000000 - 6) & 0xFFFFFFFF if WINDOWS else (256 - 6)
        assert publish.returncode == expected_publish, "publish failure exit code=%s\n%s" % (publish.returncode, output)
        assert "IpcServer StartServer failed: code=-6" in output, output
        assert 'registry publish name="ipc.server"' in output, output
        assert "ipc server listen" not in output, output
        print("registry publish failure released listen PASS", flush=True)
        timer_text = (ctx.cfg / "ipcserver.toml").read_text(encoding="utf-8")
        zero_config = ctx.cfg / "ipcserver-timer-zero.toml"
        zero_config.write_text(replace(timer_text, "offset_ms", "0"), encoding="utf-8")
        zero, zero_path = ctx.start("ipcserver-timer-zero",
                                    [str(ctx.work / ("ipcserver" + SUFFIX)), str(zero_config)])
        zero.wait(timeout=15)
        ctx.procs.remove(zero)
        output = zero_path.read_text(encoding="utf-8", errors="replace")
        assert zero.returncode == expected_publish, (zero.returncode, output)
        assert "Timer.offset_ms=0 must divide 1000" in output, output
        assert "StartFramework failed: code=-6" in output, output
        assert "ipc server listen" not in output, output
        print("zero timer offset rejected PASS", flush=True)
        for key, value in (("offset_ms", "-50"), ("timer_lv_count", "-50")):
            invalid_text = replace(timer_text, key, value)
            invalid_config = ctx.cfg / ("ipcserver-invalid-" + key + "-" + value + ".toml")
            invalid_config.write_text(invalid_text, encoding="utf-8")
            invalid, invalid_path = ctx.start("ipcserver-invalid-" + key,
                                              [str(ctx.work / ("ipcserver" + SUFFIX)), str(invalid_config)])
            invalid.wait(timeout=15)
            ctx.procs.remove(invalid)
            output = invalid_path.read_text(encoding="utf-8", errors="replace")
            assert invalid.returncode == expected_publish, (key, invalid.returncode, output)
            assert "key=Timer." + key in output and "function=Init" in output and "oi_timer_manager.cpp" in output, output
            assert re.search(r"line=[1-9][0-9]*\b", output) and "code=-6" in output, output
            assert "ipc server listen" not in output, output
        missing_text = re.sub(r"(?m)^(?:offset_ms|timer_lv_count)\s*=.*\n", "", timer_text)
        missing_config = ctx.cfg / "ipcserver-timer-default.toml"
        missing_config.write_text(missing_text, encoding="utf-8")
        default, default_path = ctx.start("ipcserver-timer-default",
                                          [str(ctx.work / ("ipcserver" + SUFFIX)), str(missing_config)])
        ctx.ready(default, default_path, "ipc server listen")
        ctx.stop(default)
        default.wait(timeout=10)
        ctx.procs.remove(default)
        assert default.returncode == 0, "default timer startup exit=%s" % default.returncode
        print("timer invalid values rejected and defaults start PASS", flush=True)
        overflow_config = ctx.cfg / "ipcserver-service-id-overflow.toml"
        overflow_text = (ctx.cfg / "ipcserver.toml").read_text(encoding="utf-8")
        assert 'id = 1' in overflow_text
        overflow_config.write_text(overflow_text.replace('id = 1', 'id = 4294967297', 1), encoding="utf-8")
        overflow, overflow_path = ctx.start("ipcserver-service-id-overflow",
                                            [str(ctx.work / ("ipcserver" + SUFFIX)), str(overflow_config)])
        overflow.wait(timeout=15)
        ctx.procs.remove(overflow)
        output = overflow_path.read_text(encoding="utf-8", errors="replace")
        assert overflow.returncode == expected_publish, "overflow service id exit=%s\n%s" % (overflow.returncode, output)
        assert "registry publish ipcserver:" in output and "[Service] id" in output, output
        assert "ipc server listen" not in output, output
        print("overflow service id rejected PASS", flush=True)
        for value in ("0", "65536", "abc", "80x"):
            invalid_config = ctx.cfg / ("ipcserver-advertise-" + value + ".toml")
            invalid_text = (ctx.cfg / "ipcserver.toml").read_text(encoding="utf-8")
            assert '# advertise_port = "27981"' in invalid_text
            invalid_text = invalid_text.replace('# advertise_port = "27981"', 'advertise_port = "%s"' % value)
            invalid_config.write_text(invalid_text, encoding="utf-8")
            invalid, invalid_path = ctx.start("ipcserver-advertise-" + value,
                                              [str(ctx.work / ("ipcserver" + SUFFIX)), str(invalid_config)])
            invalid.wait(timeout=15)
            ctx.procs.remove(invalid)
            output = invalid_path.read_text(encoding="utf-8", errors="replace")
            assert invalid.returncode == expected_publish, "invalid advertise_port=%s exit=%s\n%s" % (value, invalid.returncode, output)
            assert "registry publish invalid advertise_port for IpcServer" in output, output
            assert "ipc server listen" not in output, output
        print("invalid registry advertise ports rejected PASS", flush=True)

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
            text = server_config.read_text(encoding="utf-8")
            text = re.sub(r"(?m)^\s*(?:max_frame_bytes|max_receive_buffer_bytes)\s*=.*\n", "", text)
            server_config.write_text(text, encoding="utf-8")
            ctx.server, ctx.serverpath = ctx.start("ipcserver-default-limit", [str(ctx.work / ("ipcserver" + SUFFIX))])
            ctx.ready(ctx.server, ctx.serverpath, "ipc server listen")
            rpc_default_receive_boundary()
            ctx.stop(ctx.server)
            ctx.server.wait(timeout=10)
            assert ctx.server.returncode == 0, "default-limit probe server failed to stop"
            ctx.procs.remove(ctx.server)
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
