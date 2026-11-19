"""Registry replacement regression using two real RPC sessions."""
import socket
import struct
import time


def field(number, value):
    return struct.pack("<II", number, len(value)) + value


def receive(stream, size):
    data = b""
    while len(data) < size:
        part = stream.recv(size - len(data))
        if not part:
            raise RuntimeError("registry closed RPC connection")
        data += part
    return data


def rpc(stream, protocol, payload, call_id, allow_not_ready=False):
    body = struct.pack("<Q", (1 << 63) | call_id) + payload
    stream.sendall(struct.pack("<iI", protocol, len(body)) + body)
    reply_protocol, size = struct.unpack("<iI", receive(stream, 8))
    reply = receive(stream, size)
    assert len(reply) >= 12, "short registry reply"
    received_id, code = struct.unpack("<Qi", reply[:12])
    assert reply_protocol == protocol and received_id == call_id, (reply_protocol, received_id)
    if allow_not_ready and code != 0:
        return None
    assert code == 0, (protocol, received_id, code)
    return reply[12:]


def fields(data):
    result = {}
    offset = 0
    while offset < len(data):
        assert len(data) - offset >= 8, "short field header"
        number, size = struct.unpack_from("<II", data, offset)
        offset += 8
        assert size <= len(data) - offset and number not in result, "invalid field"
        result[number] = data[offset:offset + size]
        offset += size
    return result


def redis_get(port, key):
    with socket.create_connection(("127.0.0.1", port), timeout=2) as stream:
        stream.sendall(b"*2\r\n$3\r\nGET\r\n$%d\r\n%s\r\n" % (len(key), key))
        line = b""
        while not line.endswith(b"\r\n"):
            line += receive(stream, 1)
        assert line.startswith(b"$")
        size = int(line[1:-2])
        if size == -1:
            return None
        value = receive(stream, size + 2)
        assert value.endswith(b"\r\n")
        return value[:-2]


def snapshot(stream, name=None):
    names = struct.pack("<I", 0) if name is None else struct.pack("<II", 1, len(name)) + name
    data = fields(rpc(stream, -1008, field(0, names), 3))[0]
    count = struct.unpack_from("<I", data)[0]
    offset = 4
    instances = []
    for _ in range(count):
        size = struct.unpack_from("<I", data, offset)[0]
        offset += 4
        assert size <= len(data) - offset
        instances.append(fields(data[offset:offset + size]))
        offset += size
    assert offset == len(data)
    return instances


def check(registry_port, redis_port):
    name = b"replacement_regression"
    sentinel = b"replacement_sentinel"
    value = b"127.0.0.1 43210"
    prefix = b"oi:{openidea.dev}:svc:"
    key = prefix + name + b":77"
    sentinel_key = prefix + sentinel + b":78"

    def request(service, ident):
        return b"".join((field(0, service), field(1, struct.pack("<i", ident)),
                         field(2, b"127.0.0.1"), field(3, b"43210"), field(4, b"")))

    with socket.create_connection(("127.0.0.1", registry_port), timeout=2) as old, \
         socket.create_connection(("127.0.0.1", registry_port), timeout=2) as new, \
         socket.create_connection(("127.0.0.1", registry_port), timeout=2) as observer:
        deadline = time.monotonic() + 10
        while rpc(observer, -1008, field(0, struct.pack("<I", 0)), 4, allow_not_ready=True) is None:
            assert time.monotonic() < deadline, "registry initial snapshot not ready"
            time.sleep(0.05)
        assert snapshot(observer, b"replacement_absent") == [], "unexpected nonempty snapshot"
        rpc(old, -1004, request(name, 77), 1)
        rpc(old, -1004, request(sentinel, 78), 2)
        rpc(new, -1004, request(name, 77), 3)
        assert redis_get(redis_port, key) == value
        assert redis_get(redis_port, sentinel_key) == value
        old.close()
        deadline = time.monotonic() + 2
        while redis_get(redis_port, sentinel_key) is not None:
            assert time.monotonic() < deadline, "old session sentinel was not removed"
            time.sleep(0.02)
        assert redis_get(redis_port, key) == value, "old close removed replacement lease"
        instances = snapshot(observer)
        assert any(item.get(0) == name and item.get(1) == struct.pack("<i", 77)
                   and item.get(2) == b"127.0.0.1" and item.get(3) == b"43210"
                   for item in instances), "old close removed replacement snapshot"
        new.close()
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            instances = snapshot(observer)
            if redis_get(redis_port, key) is None and not any(item.get(0) == name for item in instances):
                break
            time.sleep(0.02)
        else:
            raise AssertionError("new close did not remove lease and snapshot")
