"""hold 组：TestHold 场景、阻塞、定时器和时间平移。"""
import re
import socket
import socketserver
import subprocess
import threading
import time

from regression_platform import SUFFIX, WINDOWS, launch, pause
from test_process_owner import record_process

from .. import ports
from ..config import replace, set_test_flags
from ..probes import check_hold_blocking, check_hold_timer
from . import Case


def check_rpc_race(text):
    """每个标记只完成一次，回包和超时都要出现，受控的 0ms 回包和 5s 超时不能对调。"""
    match = re.search(
        r"hold rpc race marks=([0-9:RTB,]+) reply=(\d+) timeout=(\d+) "
        r"controlled_reply=(\d+) controlled_timeout=(\d+) negatives=(\d+) bad=(\d+)",
        text)
    assert match, "missing hold rpc race marker"
    seen = {}
    reply = timeout = 0
    for item in match.group(1).split(","):
        mark, status = item.split(":")
        assert status in ("R", "T"), item
        assert mark not in seen, "duplicate race completion " + mark
        seen[mark] = status
        reply += status == "R"
        timeout += status == "T"
    assert set(seen) == {str(mark) for mark in range(100, 108)}, seen
    assert reply >= 1 and timeout >= 1, match.group(0)
    assert int(match.group(2)) == reply and int(match.group(3)) == timeout, match.group(0)
    assert match.group(4) == "1" and match.group(5) == "1", match.group(0)
    assert match.group(6) == "3" and match.group(7) == "0", match.group(0)


def require_thread_mode(text, expect):
    """本路预期的线程模式必须和日志一致，另一种模式的行出现就算失败。"""
    assert expect in ("st", "mt"), "hold requires --expect-thread st or mt"
    if expect == "st":
        assert "hold foreign task st rejected=1 runs=0 bad=0" in text
        assert "hold foreign task mt skipped=1 bad=0" not in text
        assert "warning: coroutine start rejected: current thread is not the single-thread poll thread" in text
    else:
        assert "hold foreign task mt skipped=1 bad=0" in text
        assert "hold foreign task st rejected=1 runs=0 bad=0" not in text


class RawHttpRegression(socketserver.BaseRequestHandler):
    # 原始 TCP 回包夹具：库级 HTTP 服务通常不会生成这些畸形报文。
    def handle(self):
        self.request.settimeout(3)
        with self.server.connection_lock:
            self.server.connection_id += 1
            connection_id = str(self.server.connection_id).encode()
        # 正常响应留着这条连接，让客户端按 keep-alive 接着发。畸形包和必须靠 EOF 结束的包写完就关。
        # /connection-id 回这条 TCP 连接的编号，是否复用由客户端比较编号判断，不看它写的 Connection 头。
        reuse = {b"good", b"large", b"interim", b"204", b"304", b"chunk-good", b"fragmented", b"size-1023", b"size-1024"}
        # 响应后面粘了下一条报文。客户端若丢掉尾部再复用，后续请求不再回包，用来抓住错位。
        glued = {b"tail", b"chunk-tail", b"empty-tail"}
        try:
            while True:
                request = b""
                while b"\r\n\r\n" not in request and len(request) < 8192:
                    part = self.request.recv(8192)
                    if not part:
                        return
                    request += part
                path = request.split(b" ")[1].split(b"?")[0].strip(b"/")
                ok = b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"
                chunk = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                wires = {
                    b"good": ok,
                    b"large": b"HTTP/1.1 200 OK\r\nContent-Length: 1048576\r\n\r\n" + b"x" * 1048576,
                    b"interim": b"HTTP/1.1 100 Continue\r\n\r\n" + ok,
                    b"204": b"HTTP/1.1 204 No Content\r\n\r\n",
                    b"304": b"HTTP/1.1 304 Not Modified\r\nContent-Length: 123\r\n\r\n",
                    b"chunk-good": chunk + b"2;name=value\r\nok\r\n0\r\nX-Test: done\r\n\r\n",
                    b"tail": ok + b"HTTP/1.1 500 BAD\r\nContent-Length: 3\r\n\r\nbad",
                    b"chunk-tail": chunk + b"2\r\nok\r\n0\r\n\r\n" + b"HTTP/1.1 500 BAD\r\nContent-Length: 3\r\n\r\nbad",
                    b"empty-tail": b"HTTP/1.1 204 No Content\r\n\r\n" + b"HTTP/1.1 500 BAD\r\nContent-Length: 3\r\n\r\nbad",
                    b"length-junk": b"HTTP/1.1 200 OK\r\nContent-Length: 1x\r\n\r\na",
                    b"length-overflow": b"HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551616\r\n\r\n",
                    b"length-conflict": b"HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nok",
                    b"length-duplicate": b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\nok",
                    b"te-cl": b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
                    b"chunk-separator": chunk + b"1\r\naXX0\r\n\r\n",
                    b"chunk-junk": chunk + b"1x\r\na\r\n0\r\n\r\n",
                    b"truncated": b"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\na",
                    b"only-interim": b"HTTP/1.1 100 Continue\r\n\r\n",
                }
                if path in (b"connection-close-first", b"connection-close-last", b"connection-id"):
                    headers = {b"connection-close-first": b"Connection: close\r\nConnection: keep-alive\r\n",
                               b"connection-close-last": b"Connection: keep-alive\r\nConnection: close\r\n",
                               b"connection-id": b""}[path]
                    self.request.sendall(b"HTTP/1.1 200 OK\r\n" + headers + b"Content-Length: " + str(len(connection_id)).encode() + b"\r\n\r\n" + connection_id)
                    continue
                if path.startswith(b"size-"):
                    size = int(path[5:])
                    # Size line includes an extension and excludes the terminating CRLF.
                    line = b"2;" + b"x" * (size - 2)
                    if size == 1025:
                        self.request.sendall(chunk + line + b"\r\nok\r\n0\r\n\r\n")
                    else:
                        self.request.sendall(chunk + line + b"\r")
                        time.sleep(0.05)
                        self.request.sendall(b"\nok\r\n0\r\n\r\n")
                elif path == b"fragmented":
                    self.request.sendall(b"HTTP/1.1 103 Early Hints\r\nLink: </test>\r\n\r\nHTTP/1.1 200")
                    self.request.settimeout(0.02)
                    try:
                        self.request.recv(1)
                    except socket.timeout:
                        pass
                    self.request.settimeout(3)
                    self.request.sendall(b" OK\r\nContent-Length: 2\r\n\r\nok")
                else:
                    self.request.sendall(wires[path])
                if path in glued:
                    follow = b""
                    while b"\r\n\r\n" not in follow and len(follow) < 8192:
                        part = self.request.recv(8192)
                        if not part:
                            return
                        follow += part
                    return
                if path not in reuse:
                    return
        except (OSError, IndexError, KeyError):
            return


def hold_environment(ctx):
    return ctx.mode == "hold" and not ctx.scene


def configure_hold_client(text):
    text = replace(text, "timer_lvs", "[4,4,4,1]")
    text = replace(text, "pull_ms", "60000")
    text = replace(text, "max_clients", "8")
    text = replace(text, "idle_ms", "200")
    # Hold 的 Redis 迟到回包比超时晚 1 秒；后续 PING 的预算必须覆盖这段排队。
    text = replace(text, "timeout_ms", "2000")
    return text


def prepare_hold_environment(ctx):
    http_fixture = socketserver.ThreadingTCPServer(("127.0.0.1", 0), RawHttpRegression)
    http_fixture.connection_lock = threading.Lock()
    http_fixture.connection_id = 0
    http_fixture.daemon_threads = True
    http_thread = threading.Thread(target=http_fixture.serve_forever, kwargs={"poll_interval": 0.05})
    http_thread.start()
    ctx.http_fixture, ctx.http_thread = http_fixture, http_thread
    for name in ("ipcclient", "ipcserver"):
        with (ctx.cfg / (name + ".toml")).open("a", encoding="utf-8") as config:
            config.write("\n[Regression]\nencoding_failure = true\nregistry_checks = true\nhttp_port = %d\n" % http_fixture.server_address[1])
            if name == "ipcclient":
                config.write('registry_fault_path = "%s"\n' % (ctx.root / "registry-fault").as_posix())


def start_hold_gameservers(ctx):
    for zone, port in zip(("1001", "1002"), ports.GAME_PORTS):
        text = (ctx.cfg / "ipcserver.toml").read_text(encoding="utf-8")
        text = text.replace('name = "ipcserver"', 'name = "gameserver"', 1)
        text = re.sub(r"(?m)^id = 1$", 'id = %s\nzone = "%s"' % (zone, zone), text, count=1)
        text = text.replace('port = "%d"' % ports.IPC_PORT, 'port = "%d"' % port, 1)
        # 同一份配置，UDP 口让系统分配，不和主服抢。
        text = text.replace('port = "%d"' % ports.UDP_PORT, 'port = "0"', 1)
        path = ctx.cfg / ("gameserver-%s.toml" % zone)
        path.write_text(text, encoding="utf-8")
        proc, output = ctx.start("gameserver-" + zone, [str(ctx.work / ("ipcserver" + SUFFIX)), str(path)])
        ctx.ready(proc, output, "ipc server listen")


def poll_registry_fault(ctx):
    request_path = ctx.root / "registry-fault.request"
    command = request_path.read_text(encoding="utf-8").strip() if request_path.exists() else ""
    if command in ("pause", "resume", "budget") and command != ctx.registry_fault_command:
        if command in ("pause", "budget"):
            ctx.registry_paused = True  # finally also resumes if suspension partly fails.
            pause(ctx.registry, True)
            ctx.registry_resume_at = time.monotonic() + 0.6 if command == "budget" else None
        else:
            pause(ctx.registry, False)
            ctx.registry_paused = False
            ctx.registry_resume_at = None
        ctx.registry_fault_command = command
        (ctx.root / "registry-fault.ack").write_text(command, encoding="utf-8")
    if ctx.registry_paused and ctx.registry_resume_at is not None and time.monotonic() >= ctx.registry_resume_at:
        pause(ctx.registry, False)
        ctx.registry_paused = False
        ctx.registry_resume_at = None


class HoldCase(Case):
    def configure(self, ctx, name, text):
        if name == "ipcclient":
            text = set_test_flags(text, ctx.mode)
            if ctx.scene:
                text += "\n[Regression]\nhold_scene = \"%s\"\n" % ctx.scene
            if ctx.mode == "hold":
                text = configure_hold_client(text)
            if hold_environment(ctx):
                # 模拟对外入口：只配接收额度，帧上限自动取它减 8；验证回包方向按这份小额度断开。
                text += "\n[HoldReceiveLimit]\nmax_receive_buffer_bytes = 1048576\n"
        if name == "ipcserver" and hold_environment(ctx):
            # 解码预算用例的请求帧约 43 MiB，执行中额度放到 128 MiB，免得先被接纳上限挡成 BUSY。
            text = replace(text, "max_inflight_bytes", "134217728")
            text = replace(text, "session_max_inflight_bytes", "134217728")
        return text

    def prepare(self, ctx):
        if hold_environment(ctx):
            prepare_hold_environment(ctx)

    def scenario(self, ctx):
        if ctx.mode == "hold" and ctx.scene in (None, "timer"):
            output = ctx.root / "timer-before-init.out"
            with output.open("w", encoding="utf-8") as stream:
                probe = launch([str(ctx.work / ("ipcclient" + SUFFIX)), "--timer-before-init"],
                               cwd=ctx.work, stdout=stream, stderr=subprocess.STDOUT)
                ctx.procs.append(probe)
                record_process(ctx.root, probe)
                code = probe.wait(timeout=10)
                ctx.procs.remove(probe)
            text = output.read_text(encoding="utf-8", errors="replace")
            assert code == 0 and re.search(r"timer before init code=-?[1-9][0-9]* called=0 bad=0", text), (code, text)
            output = ctx.root / "timer-backlog.out"
            with output.open("w", encoding="utf-8") as stream:
                probe = launch([str(ctx.work / ("ipcclient" + SUFFIX)), "--timer-backlog"],
                               cwd=ctx.work, stdout=stream, stderr=subprocess.STDOUT)
                ctx.procs.append(probe)
                record_process(ctx.root, probe)
                code = probe.wait(timeout=10)
                ctx.procs.remove(probe)
            text = output.read_text(encoding="utf-8", errors="replace")
            assert code == 0 and re.search(r"timer backlog first=1 second=2 final=3 child_first=0 child=1 stop=1 ticks=\d+/\d+ bad=0", text), (code, text)
            print(text, flush=True)
            for option, failed, finish in (("timer-shutdown", 0, 1), ("timer-shutdown-init-failure", 1, 0)):
                output = ctx.root / (option + ".out")
                with output.open("w", encoding="utf-8") as stream:
                    probe = launch([str(ctx.work / ("ipcclient" + SUFFIX)), "--" + option],
                                   cwd=ctx.work, stdout=stream, stderr=subprocess.STDOUT)
                    ctx.procs.append(probe)
                    record_process(ctx.root, probe)
                    code = probe.wait(timeout=10)
                    ctx.procs.remove(probe)
                text = output.read_text(encoding="utf-8", errors="replace")
                expected = (f"timer shutdown init_failed={failed} finish={finish} pending_calls=0 "
                            "capture=1 owned=1 borrowed=1 rejected=1 bad=0")
                assert code == 0 and expected in text.splitlines(), (code, text)
                print(expected, flush=True)
        if hold_environment(ctx):
            start_hold_gameservers(ctx)

    def client_env(self, ctx, env):
        if ctx.name.startswith("clock-"):
            if not ctx.args.clock_fault_so:
                raise RuntimeError("clock test requires --clock-fault-so")
            env["LD_PRELOAD"] = str(ctx.args.clock_fault_so)
            env["OI_TEST_CLOCK_SHIFT"] = "86400" if ctx.name == "clock-forward" else "-3600"
        return env

    def on_poll(self, ctx):
        if hold_environment(ctx):
            poll_registry_fault(ctx)

    def check(self, ctx):
        text, scene = ctx.text, ctx.scene
        serverpath = ctx.serverpath
        if ctx.scene:
            assert "hold scene=%s bad=0" % scene in text
            if scene == "blocking":
                check_hold_blocking(text)
            elif scene == "timer":
                check_hold_timer(text)
            else:
                for label in ("", "late ", "race "):
                    assert re.search(r"hold redis " + label + r"ping ok=1 status=0 reply_type=\d+ wall_ms=\d+", text)
                assert re.search(r"hold redis late timeout=1 cpu_ms=\d+ wall_ms=\d+", text)
                assert re.search(r"hold redis race (?:timeout=1 nil=0|timeout=0 nil=1) cpu_ms=\d+ wall_ms=\d+", text)
        elif ctx.mode == "hold":
            assert "hold rpc session index calls=4096 bad=0" in text
            check_hold_timer(text)
            assert "hold nested results completed=32768 caught=16384 bad=0" in text
            assert "hold hot start inline=1 queued=1 bad=0" in text
            assert "hold shared tasks waiters=64 bad=0" in text
            check_hold_blocking(text)
            require_thread_mode(text, ctx.args.expect_thread)
            check_rpc_race(text)
            for waiter_count in (2, 8, 64, 256):
                for shared_mode in range(3):
                    assert f"hold started task mode={shared_mode} waiters={waiter_count} runs=1 awakened={waiter_count} bad=0" in text
            assert "hold deferred rpc value=0 output=0 pinned=-3 bad=0" in text
            assert "hold await ownership shared=32 bad=0" in text
            assert "hold output rpc shared waiters=16 submissions=1 bad=0" in text
            assert "hold output rpc direct pinned error late bad=0" in text
            assert "hold output rpc named any zone shared=8 bad=0" in text
            assert "hold completed rpc requests retained=20 destroyed=20 bad=0" in text
            assert "hold pooled reconnect bad=0" in text
            assert "hold route backpressure status bad=0" in text
            server_text = serverpath.read_text(encoding="utf-8", errors="replace")
            assert "hold receive limit server reply=1048576" in server_text
            assert "hold receive limit server reply=1048577" in server_text
            assert "hold receive limit configured receive=1048576 frame=1048568 load=0" in text, text
            assert re.search(r"hold receive limit exact=1048576 exact_code=0 over_code=-3 preserved=1 bad=0", text), text
            budget_scaled = re.search(r"hold decode budget scaled words=(\d+) charged=(\d+) frame=(\d+) code=0 bad=0", text)
            assert budget_scaled, "missing decode budget scaled marker"
            charged, frame = int(budget_scaled.group(2)), int(budget_scaled.group(3))
            assert 64 * 1024**2 < charged < 256 * 1024**2 and 40 * 1024**2 <= frame <= 48 * 1024**2, budget_scaled.group(0)
            assert "hold disconnect drain calls=4096 bad=0" in text
            assert re.search(r"hold batch bind failure caught=1 registered=1 submits=1 drops=1 elapsed=\d+ bad=0", text), text
            assert "hold batch failure slots bad=0" in text
            assert re.search(r"hold absolute deadline buckets calls=4096 buckets=\d+ fired=32 bad=0", text)
            assert "hold sweep exception callbacks=128 same_batch=1 guard=0 bad=0" in text
            assert "hold peer finish exception callbacks=8 guard=0 bad=0" in text
            assert "hold rpc receive exception first=1/-1 second=1/-3 close=1 guard=0 bad=0" in text
            assert "hold registry pause cached many=3/3 call=0 mark=831 bad=0" in text
            paused_missing = re.search(r"hold registry pause missing snapshot=0 code=-2 elapsed_ms=(\d+) mark=-731 bad=0", text)
            assert paused_missing and int(paused_missing.group(1)) < 450, paused_missing.group(0) if paused_missing else "missing pause-missing marker"
            # 空名单：Resolve 记 Offline，RouteCall::Error 默认返回 OI_ERR_CONNECT（-3）。
            assert "hold registry resume missing snapshot=1 empty=1 code=-3 bad=0" in text
            budget = re.search(r"hold registry budget miss code=-3 elapsed_ms=(\d+) cached=0 mark=841 bad=0", text)
            assert budget and 500 <= int(budget.group(1)) < 2450, budget.group(0) if budget else "missing budget marker"
            assert "hold binary bool bad=0" in text
            assert "hold binary container bad=0" in text
            assert "hold connection isolation bad=0" in text
            assert "hold connect register failure code=-14 empty=1 added=0 failed=1 bad=0" in text
            assert "hold coroutine connect bad=0" in text
            assert "hold address connect bad=0" in text
            assert re.search(r"hold address resolve failure addresses=0 code=-3 connections=0 unresolved=1$", text, re.M)
            assert "hold address failure init=-6 bind=-15 bad=0" in text
            assert "hold address configured name preserved=1 bad=0" in text
            assert "hold address stress waiters=64 rounds=20 bad=0" in text
            assert "hold reentrant connect first=1 retry=0 connections=1 bad=0" in text
            assert "hold registry pairing bad=0" in text
            assert "hold route target stability bad=0" in text
            assert re.search(r"hold service rpc global=1 any=1 zone=1 batch=2 many=2 slot=1 elapsed_ms=\d+ wrong=1 miss=1 bad=0", text)
            assert "rpc stage=route reason=wrong_kind service=ipcserver zone=1001 zones=0 code=-1" in text
            assert "rpc stage=route reason=offline service=gameserver zone=9999 zones=0 code=-3" in text
            assert "hold named rpc ownership success=8 rejected=0 bad=0" in text
            assert "hold route notify single=1 zone=1 zone_other=0 negative=-1 all=2 many=2 missing_reported=1 absent_reported=1 reconnect=1 bad=0" in text
            assert "task failed: route notification failed service=gameserver zones=1003 protocol=3006 failed=1/3 code=-3" in text
            assert "task failed: route notification failed service=missingnotify protocol=3006 failed=1/1 code=-3" in text
            assert "hold client notify early=1 connected=1 dropped=1 reconnect=1 bad=0" in text
            assert "task failed: RPC notification failed protocol=3006 code=-3" in text
            assert re.search(r"hold http release before=\d+ after=\d+ bad=0", text)
            if WINDOWS:
                assert "hold http release probe count=32 retained=0 bad=0" in text
            assert "hold json validation bad=0" in text
            assert "hold server encoding bad=0" in text
            assert re.search(r"hold direct backpressure queued=-?\d+ direct=-?\d+ pinned=-?\d+ closed=-?\d+ bad=0", text)
            assert re.search(r"hold client malformed response sid=[1-9]\d* bad=0", text)
            assert "hold http framing bad=0" in text
            assert "hold rpc standardized reply bad=0" in text
            assert "hold rpc late overlap barrier=0/804 pending=1 follow=0/803 bad=0" in text
            assert "hold reverse trigger ret=0 mark=-2147483598 served=1 bad=0" in text
            assert "hold reverse request served delay=1000" in text
            server_text = serverpath.read_text(encoding="utf-8", errors="replace")
            assert "hold late event mark=802" in server_text
            assert "hold late event mark=804" in server_text
            assert server_text.index("hold late event mark=802") < server_text.index("hold late event mark=804")
            assert "hold done bad=0" in text
        return False

    def diagnostics(self, ctx):
        if ctx.name != "hold":
            return
        text = ctx.text
        diagnostics = ctx.stop_backends()
        server_output = ctx.serverpath.read_text(encoding="utf-8", errors="replace")
        assert "hold reverse reply bad=0" in server_output
        for stage, reason in (("handler", "injected server handler failure"), ("encode_response", "injected server response encoding failure")):
            assert re.search(r"rpc failure stage=" + stage + r" token=[1-9]\d* call=[1-9]\d* sid=\d+ proto=3002 ret=-?\d+ reason=" + reason, diagnostics), "RPC exception context missing from logd disk: " + stage
        source = re.search(r"hold client malformed response sid=([1-9]\d*) bad=0", text)
        assert source
        assert re.search(r"rpc failure stage=decode_response token=\d+ call=[1-9]\d* sid=" + source.group(1) + r" proto=3002 ret=-?\d+ reason=ParserException", diagnostics), "client reply decode source missing from logd disk"
        print("diagnostic original errors persisted", ctx.name, "PASS", flush=True)

CASES = (
    HoldCase("hold", "hold"),
    HoldCase("hold-blocking", "hold", scene="blocking", targeted_reason="also exercised by hold"),
    HoldCase("hold-timer", "hold", scene="timer", targeted_reason="also exercised by hold"),
    HoldCase("hold-redis", "hold", scene="redis", targeted_reason="also exercised by hold"),
    HoldCase("clock-forward", "hold", targeted_reason="Linux clock injection; run with --clock-faults"),
    HoldCase("clock-back", "hold", targeted_reason="Linux clock injection; run with --clock-faults"),
)
