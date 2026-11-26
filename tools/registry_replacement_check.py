"""Registry replacement regression using real RPC sessions."""
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


def rpc(stream, protocol, payload, call_id, allow_not_ready=False, expected_code=0):
    body = struct.pack("<Q", (1 << 63) | call_id) + payload
    stream.sendall(struct.pack("<iI", protocol, len(body)) + body)
    reply_protocol, size = struct.unpack("<iI", receive(stream, 8))
    reply = receive(stream, size)
    assert len(reply) >= 12, "short registry reply"
    received_id, code = struct.unpack("<Qi", reply[:12])
    assert reply_protocol == protocol and received_id == call_id, (reply_protocol, received_id)
    if allow_not_ready and code != 0:
        return None
    assert code == expected_code, (protocol, received_id, code, expected_code)
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
    owner_key = b"oi:{openidea.dev}:owner:" + name + b":77"
    heartbeat = field(0, name) + field(1, struct.pack("<i", 77))

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
        # 分隔符不得进入地址字段：拒绝后已登记实例和 Redis 原值保持不变。
        for index, (ip, port) in enumerate((
                (b"127.0.0.1 extra", b"43210"), (b"127.0.0.1", b"43210 extra"),
                (b"0.0.0.0", b"43210"), (b"::", b"43210"), (b"[::]", b"43210"), (b"*", b"43210"))):
            invalid = b"".join((field(0, name), field(1, struct.pack("<i", 77)),
                                field(2, ip), field(3, port), field(4, b"")))
            rpc(old, -1004, invalid, 20 + index, expected_code=-1)
            assert redis_get(redis_port, key) == value, "invalid address changed Redis lease"
            assert any(item.get(0) == name and item.get(1) == struct.pack("<i", 77)
                       and item.get(2) == b"127.0.0.1" and item.get(3) == b"43210"
                       for item in snapshot(observer)), "invalid address changed snapshot"
        old_owner = redis_get(redis_port, owner_key)
        assert old_owner, "old registration missing owner"
        rpc(new, -1004, request(name, 77), 3)
        new_owner = redis_get(redis_port, owner_key)
        assert new_owner and new_owner != old_owner, "replacement did not acquire a new owner"
        # Lose only the owner key while the service lease and current session survive.
        redis_command(redis_port, b"DEL", owner_key)
        assert redis_get(redis_port, owner_key) is None and redis_get(redis_port, key) == value
        rpc(old, -1006, heartbeat, 40, expected_code=-4)
        rpc(old, -1004, request(name, 77), 41, expected_code=-4)
        assert redis_get(redis_port, owner_key) is None, "superseded session reclaimed missing owner"
        rpc(new, -1006, heartbeat, 42)
        assert redis_get(redis_port, owner_key) == new_owner, "current heartbeat did not restore owner"
        redis_command(redis_port, b"DEL", owner_key)
        rpc(new, -1004, request(name, 77), 43)
        assert redis_get(redis_port, owner_key) == new_owner, "current registration did not restore owner"
        assert redis_get(redis_port, key) == value, "owner recovery changed service lease"
        rpc(old, -1006, heartbeat, 5, expected_code=-4)
        rpc(old, -1004, request(name, 77), 6, expected_code=-4)
        rpc(new, -1006, heartbeat, 7)
        assert redis_get(redis_port, owner_key) == new_owner, "old session changed replacement owner"
        assert redis_get(redis_port, key) == value
        assert redis_get(redis_port, sentinel_key) == value
        assert any(item.get(0) == name and item.get(1) == struct.pack("<i", 77)
                   and item.get(2) == b"127.0.0.1" and item.get(3) == b"43210"
                   for item in snapshot(observer)), "old requests changed replacement snapshot"
        old.close()
        deadline = time.monotonic() + 2
        while redis_get(redis_port, sentinel_key) is not None:
            assert time.monotonic() < deadline, "old session sentinel was not removed"
            time.sleep(0.02)
        assert redis_get(redis_port, key) == value, "old close removed replacement lease"
        assert redis_get(redis_port, owner_key) == new_owner, "old close removed replacement owner"
        instances = snapshot(observer)
        assert any(item.get(0) == name and item.get(1) == struct.pack("<i", 77)
                   and item.get(2) == b"127.0.0.1" and item.get(3) == b"43210"
                   for item in instances), "old close removed replacement snapshot"
        new.close()
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            instances = snapshot(observer)
            if (redis_get(redis_port, key) is None and redis_get(redis_port, owner_key) is None
                    and not any(item.get(0) == name for item in instances)):
                break
            time.sleep(0.02)
        else:
            raise AssertionError("new close did not remove lease and snapshot")

        # A fresh session may claim the same instance after the current owner closes.
        with socket.create_connection(("127.0.0.1", registry_port), timeout=2) as third:
            rpc(third, -1004, request(name, 77), 8)
            third_owner = redis_get(redis_port, owner_key)
            assert third_owner and third_owner != new_owner, "fresh session did not acquire owner"
            rpc(third, -1006, heartbeat, 9)
            assert redis_get(redis_port, key) == value, "fresh session did not register address"
            assert any(item.get(0) == name and item.get(1) == struct.pack("<i", 77)
                       and item.get(2) == b"127.0.0.1" and item.get(3) == b"43210"
                       for item in snapshot(observer)), "fresh session missing from snapshot"


def redis_command(port, *args):
    with socket.create_connection(("127.0.0.1", port), timeout=2) as stream:
        wire = b"*%d\r\n" % len(args)
        for arg in args:
            wire += b"$%d\r\n%s\r\n" % (len(arg), arg)
        stream.sendall(wire)
        with stream.makefile("rb") as reply:
            from registry_fault_proxy import read_resp
            raw, _ = read_resp(reply)
            assert not raw.startswith(b"-"), raw


def check_reload_race(registry_port, redis_port, proxy):
    """Hold one real Redis snapshot across disconnect, then fail that owner's withdrawal."""
    name = b"reload_race_regression"
    sentinel = b"reload_race_sentinel"
    prefix = b"oi:{openidea.dev}:svc:"
    key = prefix + name + b":91"
    expected = b"127.0.0.1 43210"
    external = b"reload_external_first"
    external_next = b"reload_external_second"
    base = b"oi:{openidea.dev}:"

    def add_external(service, ident):
        instance = service + b":" + str(ident).encode("ascii")
        redis_command(redis_port, b"SET", base + b"svc:" + instance, b"127.0.0.2 43211")
        redis_command(redis_port, b"SET", base + b"owner:" + instance, b"external-owner-" + service)
        redis_command(redis_port, b"SADD", base + b"svc:" + service, str(ident).encode("ascii"))
        redis_command(redis_port, b"SADD", base + b"svc", service)

    def request(service, ident):
        return b"".join((field(0, service), field(1, struct.pack("<i", ident)),
                         field(2, b"127.0.0.1"), field(3, b"43210"), field(4, b"")))

    def contains(stream, service, ident):
        return any(item.get(0) == service and item.get(1) == struct.pack("<i", ident)
                   for item in snapshot(stream))

    with socket.create_connection(("127.0.0.1", registry_port), timeout=2) as old, \
         socket.create_connection(("127.0.0.1", registry_port), timeout=2) as other, \
         socket.create_connection(("127.0.0.1", registry_port), timeout=2) as observer:
        rpc(old, -1004, request(name, 91), 31)
        rpc(other, -1004, request(sentinel, 92), 32)
        assert redis_get(redis_port, key) == expected and contains(observer, name, 91)
        proxy.reload_target = external
        proxy.withdraw_key = key
        proxy.reload_forwarded.clear()
        proxy.reload_armed.set()
        add_external(external, 93)
        try:
            assert proxy.reload_captured.wait(5), "reload response was not captured"
            proxy.withdraw_armed.set()
            old.close()
            deadline = time.monotonic() + 2
            while contains(observer, name, 91):
                assert time.monotonic() < deadline, "disconnect did not remove local snapshot"
                time.sleep(0.02)
            assert contains(observer, sentinel, 92), "disconnect removed unrelated instance"
        finally:
            proxy.reload_release.set()
        assert proxy.withdraw_failed.wait(5), "withdraw failure was not injected"
        assert redis_get(redis_port, key) == expected, "withdraw unexpectedly removed Redis lease"
        deadline = time.monotonic() + 5
        while not contains(observer, external, 93):
            assert time.monotonic() < deadline, "held reload did not publish its external instance"
            time.sleep(0.02)
        assert not contains(observer, name, 91), "stale held reload restored disconnected instance"
        assert contains(observer, sentinel, 92), "stale reload removed unrelated instance"
        # 新外来行可见证明撤销失败后的下一轮 Redis 结果已真正发布。
        add_external(external_next, 94)
        deadline = time.monotonic() + 5
        while not contains(observer, external_next, 94):
            assert time.monotonic() < deadline, "post-withdraw reload did not publish external instance"
            time.sleep(0.02)
        assert not contains(observer, name, 91), "failed withdrawal allowed old owner back on reload"
        assert contains(observer, sentinel, 92), "reload filtered unrelated owner"
        with socket.create_connection(("127.0.0.1", registry_port), timeout=2) as replacement:
            rpc(replacement, -1004, request(name, 91), 33)
            assert contains(observer, name, 91), "same-address new owner was hidden"
            assert contains(observer, sentinel, 92), "replacement hid unrelated owner"
        print("registry reload stale and failed withdraw PASS", flush=True)


def check_owner_loss_disconnect(registry_port, redis_port):
    """An ownerless lease cannot reappear after its current session disconnects."""
    name, sentinel = b"owner_lost_disconnect", b"owner_lost_sentinel"
    prefix = b"oi:{openidea.dev}:"
    key = prefix + b"svc:" + name + b":71"
    owner = prefix + b"owner:" + name + b":71"
    sentinel_key = prefix + b"svc:" + sentinel + b":72"

    def request(service, ident):
        return b"".join((field(0, service), field(1, struct.pack("<i", ident)),
                         field(2, b"127.0.0.1"), field(3, b"43210"), field(4, b"")))

    def contains(stream, service, ident):
        return any(item.get(0) == service and item.get(1) == struct.pack("<i", ident)
                   for item in snapshot(stream, service))

    def add_external(service, ident):
        instance = service + b":" + str(ident).encode("ascii")
        redis_command(redis_port, b"SET", prefix + b"svc:" + instance, b"127.0.0.2 43211")
        redis_command(redis_port, b"SET", prefix + b"owner:" + instance, b"external-" + service)
        redis_command(redis_port, b"SADD", prefix + b"svc:" + service, str(ident).encode("ascii"))
        redis_command(redis_port, b"SADD", prefix + b"svc", service)

    with socket.create_connection(("127.0.0.1", registry_port), timeout=2) as client, \
         socket.create_connection(("127.0.0.1", registry_port), timeout=2) as observer:
        rpc(client, -1004, request(name, 71), 101)
        rpc(client, -1004, request(sentinel, 72), 102)
        assert contains(observer, name, 71) and redis_get(redis_port, owner)
        redis_command(redis_port, b"DEL", owner)
        assert redis_get(redis_port, owner) is None and redis_get(redis_port, key) == b"127.0.0.1 43210"
        client.close()
        deadline = time.monotonic() + 2
        while redis_get(redis_port, sentinel_key) is not None:
            assert time.monotonic() < deadline, "disconnect sentinel was not withdrawn"
            time.sleep(0.02)
        assert not contains(observer, name, 71), "disconnected service remained in snapshot"
        for service, ident in ((b"owner_loss_reload_one", 73), (b"owner_loss_reload_two", 74)):
            add_external(service, ident)
            deadline = time.monotonic() + 5
            while not contains(observer, service, ident):
                assert time.monotonic() < deadline, "reload barrier was not published"
                assert not contains(observer, name, 71), "ownerless disconnected service reappeared"
                time.sleep(0.02)
            assert not contains(observer, name, 71), "reload restored ownerless disconnected service"
        with socket.create_connection(("127.0.0.1", registry_port), timeout=2) as replacement:
            rpc(replacement, -1004, request(name, 71), 103)
            new_owner = redis_get(redis_port, owner)
            assert new_owner and redis_get(redis_port, key) == b"127.0.0.1 43210"
            rpc(replacement, -1006, field(0, name) + field(1, struct.pack("<i", 71)), 104)
            assert redis_get(redis_port, owner) == new_owner and contains(observer, name, 71)
        print("registry owner loss disconnect and replacement PASS", flush=True)
