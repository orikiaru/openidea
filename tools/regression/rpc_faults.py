"""RPC 故障注入：连接重置、畸形/分片帧和服务端会话回收。"""
import errno
import re
import socket
import struct
import time

from regression_platform import WINDOWS, pause

from . import ports


def reset_connections():
    # 故障注入：收到 RST 后不能在会话表留下已释放的指针。
    for _ in range(100):
        with socket.create_connection(("127.0.0.1", ports.IPC_PORT), timeout=2) as s:
            s.sendall(b"\x00")
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("HH" if WINDOWS else "ii", 1, 0))


def rpc_ingress_faults():
    # 只注入畸形/分片网络数据；正常负载仍由 ipcclient 的 TestEcho 发出。
    def closed(stream):
        try:
            assert stream.recv(1) == b"", "invalid frame accepted"
        except ConnectionResetError:
            pass

    def receive(stream, count):
        result = b""
        while len(result) < count:
            part = stream.recv(count - len(result))
            assert part, "valid fragmented frame disconnected"
            result += part
        return result

    with socket.create_connection(("127.0.0.1", ports.IPC_PORT), timeout=1) as stream:
        stream.sendall(struct.pack("<II", 3002, 65537))
        closed(stream)  # 只发头，不应等正文或残包超时。
    for partial in (b"\x01", struct.pack("<IIQ", 3002, 65536, 1 << 63)):
        with socket.create_connection(("127.0.0.1", ports.IPC_PORT), timeout=1) as stream:
            started = time.monotonic()
            stream.sendall(partial)
            time.sleep(0.15)
            stream.sendall(b"\x00")  # 滴流不刷新 250ms 的绝对期限。
            closed(stream)
            assert time.monotonic() - started < 0.39, "partial deadline extended"
    with socket.create_connection(("127.0.0.1", ports.IPC_PORT), timeout=2) as stream:
        for call_id in range(1, 4):
            body = struct.pack("<QIIiIIi", (1 << 63) | call_id, 0, 4, 0, 1, 4, 42)
            body += struct.pack("<II", 99, 65536 - len(body) - 8) + b"x" * (65536 - len(body) - 8)
            frame = struct.pack("<II", 3002, len(body)) + body
            stream.sendall(frame[:5])
            time.sleep(0.03)
            stream.sendall(frame[5:])
            proto, size = struct.unpack("<II", receive(stream, 8))
            response = receive(stream, size)
            assert proto == 3002 and struct.unpack_from("<Q", response)[0] == call_id
            assert struct.unpack_from("<i", response, 8)[0] == 0, response
            time.sleep(0.3)  # 完整包必须取消旧计时器，下一包重新计时。
    # 半读的单向帧、畸形嵌套帧和正常帧一起发送；每会话只允许一个在途请求。
    with socket.create_connection(("127.0.0.1", ports.IPC_PORT), timeout=2) as stream:
        partial = struct.pack("<Q", 0) + b"\x01\x02\x03\x04"
        body = struct.pack("<QII", (1 << 63) | 424242, 0, 9) + struct.pack("<II", 0, 4) + b"\x01"
        valid = struct.pack("<QIIiIIi", (1 << 63) | 424243, 0, 4, 0, 1, 4, 42)
        stream.sendall(struct.pack("<II", 31000, len(partial)) + partial
                       + struct.pack("<II", 3001, len(body)) + body
                       + struct.pack("<II", 3002, len(valid)) + valid)
        proto, size = struct.unpack("<II", receive(stream, 8))
        response = receive(stream, size)
        assert proto == 3001 and struct.unpack_from("<Q", response)[0] == 424242, "malformed nested response pairing"
        assert size == 12 and struct.unpack_from("<i", response, 8)[0] == -1, "malformed nested response must be OI_ERR_FAIL"
        proto, size = struct.unpack("<II", receive(stream, 8))
        response = receive(stream, size)
        assert proto == 3002 and struct.unpack_from("<Q", response)[0] == 424243, "next frame response pairing"
        assert struct.unpack_from("<i", response, 8)[0] == 0, "admission not released or next frame lost"
        assert struct.unpack_from("<IIi", response, 12) == (0, 4, 42), "next frame payload"
    print("rpc ingress partial-handler nested-failure next-frame admission-release PASS", flush=True)
    print("rpc ingress oversized/partial/drip/boundary/rearm PASS", flush=True)


def rpc_receive_boundary(limit, label):
    # The receive limit includes the 8-byte frame header. The unknown field
    # pads a valid Hold request without changing its business result.
    body = struct.pack("<QIIiIIi", (1 << 63) | 424244, 0, 4, 0, 1, 4, 42)
    body += struct.pack("<II", 99, limit - 8 - len(body) - 8)
    body += b"x" * (limit - 8 - len(body))
    frame = struct.pack("<II", 3002, len(body)) + body
    assert len(frame) == limit
    with socket.create_connection(("127.0.0.1", ports.IPC_PORT), timeout=30) as stream:
        stream.sendall(frame)
        response = b""
        while len(response) < 8:
            part = stream.recv(8 - len(response))
            assert part, "%s-limit request closed before response header" % label
            response += part
        proto, size = struct.unpack("<II", response)
        response = b""
        while len(response) < size:
            part = stream.recv(size - len(response))
            assert part, "%s-limit response truncated" % label
            response += part
        assert proto == 3002 and struct.unpack_from("<Qi", response) == (424244, 0), response[:16]
        assert struct.unpack_from("<IIi", response, 12) == (0, 4, 42), response[:24]
    with socket.create_connection(("127.0.0.1", ports.IPC_PORT), timeout=3) as stream:
        stream.sendall(struct.pack("<II", 3002, limit - 8 + 1))
        try:
            assert stream.recv(1) == b"", "%s over-limit frame header accepted" % label
        except ConnectionResetError:
            pass
    print("rpc %s receive exact=%d over=%d PASS" % (label, limit, limit + 1), flush=True)


# 服务端会话回收：IpcServer::OnAddSession / OnDelSession 各打一行，del 行只在通道真正销毁时出现。
SESSION_ADD = re.compile(r"(?m)^ipc server session (\d+)\s*$")


def server_text(ctx):
    return ctx.serverpath.read_text(encoding="utf-8", errors="replace")


def wait_new_session(ctx, known, label):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        sids = SESSION_ADD.findall(server_text(ctx))
        if len(sids) > known:
            return int(sids[known])
        time.sleep(0.02)
    raise AssertionError("%s: server did not report the new session" % label)


def wait_session_deleted(ctx, sid, limit, label):
    """等服务端打印这条会话的 del 行，返回 (error, 秒)；超过 limit 秒判失败。"""
    pattern = re.compile(r"(?m)^ipc server session del %d error=(-?\d+)\s*$" % sid)
    started = time.monotonic()
    while True:
        match = pattern.search(server_text(ctx))
        elapsed = time.monotonic() - started
        if match:
            return int(match.group(1)), elapsed
        if elapsed > limit:
            raise AssertionError("%s: server session %d not reclaimed within %.0fs" % (label, sid, limit))
        time.sleep(0.02)


def receive_exact(stream, count):
    result = b""
    while len(result) < count:
        part = stream.recv(count - len(result))
        assert part, "connection closed after %d of %d bytes" % (len(result), count)
        result += part
    return result


def peer_note_count(tag):
    # PeerProbe（3005）：iTag=tag、iNotify=0，回包第 2 号字段是这个标记已收到的 PeerNote 条数。
    body = struct.pack("<QIIiIIi", (1 << 63) | 1, 0, 4, tag, 1, 4, 0)
    with socket.create_connection(("127.0.0.1", ports.IPC_PORT), timeout=5) as stream:
        stream.sendall(struct.pack("<II", 3005, len(body)) + body)
        proto, size = struct.unpack("<II", receive_exact(stream, 8))
        response = receive_exact(stream, size)
    call_id, code = struct.unpack_from("<Qi", response)
    assert proto == 3005 and call_id == 1 and code == 0, (proto, call_id, code)
    fields, offset = {}, 12
    while offset < len(response):
        index, length = struct.unpack_from("<II", response, offset)
        fields[index] = response[offset + 8:offset + 8 + length]
        offset += 8 + length
    return struct.unpack("<i", fields[2])[0] if 2 in fields else 0


def tcp_error_rearm(ctx):
    """P2-1：对端未读数据超过服务端一轮能读的量，紧接着 RST，两者在服务端 poll 之前都已到达。

    Unix libuv 遇 POLLERR 停掉 handle 只回调一次，DataIn 读满接收额度就返回，没读到错误。
    旧行为下这个 fd 不再被 poll，会话永不回收；修复后重开，读完全部数据再读到 ECONNRESET。
    临时配置把接收额度设为 16000 字节（400 个 40 字节帧），对端灌 3 倍额度。
    """
    if WINDOWS:
        # Windows 的 uv_poll 出错不自停，这条路径只在 Unix libuv 上存在。
        print("tcp error rearm skipped=1 platform=windows reason=unix-libuv-pollerr-only", flush=True)
        return
    tag, frames, allowance = 7101, 1200, 16000
    # PeerNote 单向消息（3006，调用号 0）：服务端只计数不回包，不会因写关注变化把 handle 重新打开。
    body = struct.pack("<QIIiIIi", 0, 0, 4, tag, 1, 4, 0)
    frame = struct.pack("<II", 3006, len(body)) + body
    assert len(frame) == 40 and allowance % len(frame) == 0
    payload = frame * frames
    assert len(payload) > 2 * allowance
    known = len(SESSION_ADD.findall(server_text(ctx)))
    stream = socket.create_connection(("127.0.0.1", ports.IPC_PORT), timeout=5)
    paused = False
    try:
        sid = wait_new_session(ctx, known, "tcp error rearm")
        pause(ctx.server, True)
        paused = True
        deadline = time.monotonic() + 5
        while open("/proc/%d/stat" % ctx.server.pid).read().rsplit(")", 1)[1].split()[0] != "T":
            assert time.monotonic() < deadline, "ipcserver did not stop"
            time.sleep(0.01)
        stream.sendall(payload)
        # 关闭前确认全部数据已经交给对端内核：SIOCOUTQNSD（0x894B）是本端还没发出的字节数。
        import fcntl
        unsent = bytearray(4)
        deadline = time.monotonic() + 2
        while True:
            fcntl.ioctl(stream.fileno(), 0x894B, unsent)
            if struct.unpack("i", unsent)[0] == 0:
                break
            assert time.monotonic() < deadline, "payload still unsent before RST"
            time.sleep(0.01)
        stream.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        stream.close()
        time.sleep(0.2)
        pause(ctx.server, False)
        paused = False
        error, elapsed = wait_session_deleted(ctx, sid, 5, "tcp error rearm")
    finally:
        if paused:
            pause(ctx.server, False)
        stream.close()
    assert error == errno.ECONNRESET, "tcp error rearm: session %d closed with error=%d, expected ECONNRESET" % (sid, error)
    # 会话是读完全部数据之后才读到 RST 关掉的：每一帧都被解析并计数。
    deadline = time.monotonic() + 3
    while True:
        count = peer_note_count(tag)
        if count == frames or time.monotonic() > deadline:
            break
        time.sleep(0.05)
    assert count == frames, "tcp error rearm: server parsed %d of %d frames before RST" % (count, frames)
    print("tcp error rearm sid=%d bytes=%d allowance=%d frames=%d/%d error=%d elapsed_ms=%d PASS" % (
        sid, len(payload), allowance, count, frames, error, elapsed * 1000), flush=True)


def tcp_close_reclaim(ctx, queue_limit):
    """P2-2：会话关闭时写关注早已打开、对端零窗口又不再发数据，必须靠兜底定时器回收。

    对端把接收缓冲调小且从不读。第一条 AllEcho 回包远大于内核能吸收的量，剩下的卡在会话里、写关注一直开着；
    确认回包开始到达后，第二条请求的回包超过 max_send_queue_bytes，服务端排不进回复就关闭会话。
    之后再没有任何 IO 事件，旧行为下会话永不回收；修复后 10 秒兜底 TryClose。
    """
    first, second = queue_limit * 3 // 4, queue_limit + 1024 * 1024

    def echo(call_id, size):
        # AllEchoReq.stData（字段 0）里只带 sMessage（字段 8），服务端原样回显。
        message = struct.pack("<II", 8, size) + b"r" * size
        body = struct.pack("<QII", (1 << 63) | call_id, 0, len(message)) + message
        return struct.pack("<II", 3001, len(body)) + body

    known = len(SESSION_ADD.findall(server_text(ctx)))
    stream = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        stream.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        stream.settimeout(30)
        stream.connect(("127.0.0.1", ports.IPC_PORT))
        sid = wait_new_session(ctx, known, "tcp close reclaim")
        stream.sendall(echo(1, first))
        # 回包整条进了会话发送缓冲才开始写出，看到首字节就说明它已排队且卡在零窗口上。只窥视，不消费。
        assert len(stream.recv(1, socket.MSG_PEEK)) == 1, "first echo reply never started"
        stream.sendall(echo(2, second))
        error, elapsed = wait_session_deleted(ctx, sid, 15, "tcp close reclaim")
    finally:
        stream.close()
    assert error == -20000, "tcp close reclaim: session %d closed with error=%d, expected SESSION_MANAGER_NORMAL_CLOSE" % (sid, error)
    print("tcp close reclaim sid=%d queue_limit=%d first=%d second=%d error=%d elapsed_ms=%d PASS" % (
        sid, queue_limit, first, second, error, elapsed * 1000), flush=True)
