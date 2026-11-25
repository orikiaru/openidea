"""RPC 故障注入：连接重置和畸形/分片帧。"""
import socket
import struct
import time

from regression_platform import WINDOWS

from . import ports


def reset_connections():
    # 故障注入：收到 RST 后不能在会话表留下已释放的指针。
    for _ in range(100):
        with socket.create_connection(("127.0.0.1", ports.IPC_PORT), timeout=2) as s:
            s.sendall(b"\x00")
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("HH" if WINDOWS else "ii", 1, 0))


def rpc_ingress_faults():
    # 只注入畸形/分片网络数据；正常负载仍由 ipcclient 的 EchoMgr 发出。
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


def rpc_default_receive_boundary():
    # Default receive limit includes the 8-byte frame header. The unknown field
    # pads a valid Hold request without changing its business result.
    limit = 1024 * 1024
    body = struct.pack("<QIIiIIi", (1 << 63) | 424244, 0, 4, 0, 1, 4, 42)
    body += struct.pack("<II", 99, limit - 8 - len(body) - 8)
    body += b"x" * (limit - 8 - len(body))
    frame = struct.pack("<II", 3002, len(body)) + body
    assert len(frame) == limit
    with socket.create_connection(("127.0.0.1", ports.IPC_PORT), timeout=10) as stream:
        stream.sendall(frame)
        response = b""
        while len(response) < 8:
            part = stream.recv(8 - len(response))
            assert part, "default-limit request closed before response header"
            response += part
        proto, size = struct.unpack("<II", response)
        response = b""
        while len(response) < size:
            part = stream.recv(size - len(response))
            assert part, "default-limit response truncated"
            response += part
        assert proto == 3002 and struct.unpack_from("<Qi", response) == (424244, 0), response[:16]
        assert struct.unpack_from("<IIi", response, 12) == (0, 4, 42), response[:24]
    with socket.create_connection(("127.0.0.1", ports.IPC_PORT), timeout=3) as stream:
        stream.sendall(struct.pack("<II", 3002, limit - 8 + 1))
        try:
            assert stream.recv(1) == b"", "over-limit frame header accepted"
        except ConnectionResetError:
            pass
    print("rpc default receive exact=1048576 over=1048577 PASS", flush=True)
