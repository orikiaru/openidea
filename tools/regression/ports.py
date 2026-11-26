"""回归用端口常量、--port-offset 平移和启动前端口探测。

端口常量会被 apply_port_offset 重新绑定，其他模块必须通过 ports.<名字> 读取。
"""
import socket

from regression_platform import WINDOWS

# Windows 动态端口段（本机 1024-15000）可能被 Hyper-V/WinNAT 保留，测试 Redis 不用 6379。
REDIS_PORT = 16380
LOGD_PORT = 19995
IPC_PORT = 17981
IPC_FALLBACK_PORT = 17982
REDIS_SERVER_PORT = 16379
REGISTRY_PORT = 17990
GAME_PORTS = (17992, 17993)
# ipcserver 的 UDP 口，只有 udp 用例往这里发。
UDP_PORT = 17983
# --port-offset 平移的端口；多份工作目录并行跑时各用一段，互不抢占。
BASE_PORTS = (REDIS_PORT, LOGD_PORT, IPC_PORT, IPC_FALLBACK_PORT, REDIS_SERVER_PORT, REGISTRY_PORT, UDP_PORT) + GAME_PORTS
PORT_OFFSET = 0


def apply_port_offset(offset):
    global REDIS_PORT, LOGD_PORT, IPC_PORT, IPC_FALLBACK_PORT, REDIS_SERVER_PORT, REGISTRY_PORT, UDP_PORT, GAME_PORTS, PORT_OFFSET
    PORT_OFFSET = offset
    UDP_PORT += offset
    REDIS_PORT += offset
    LOGD_PORT += offset
    IPC_PORT += offset
    IPC_FALLBACK_PORT += offset
    REDIS_SERVER_PORT += offset
    REGISTRY_PORT += offset
    GAME_PORTS = tuple(port + offset for port in GAME_PORTS)


def preflight_ports(case):
    if case in ("web", "web-stop"):
        return
    scene_ports = {"web-coro": (IPC_PORT, REDIS_SERVER_PORT, REDIS_PORT),
                   "hold-blocking": (LOGD_PORT, IPC_PORT), "hold-timer": (LOGD_PORT,),
                   "hold-redis": (LOGD_PORT, REDIS_SERVER_PORT, REDIS_PORT)}
    def probe_port(port, kind):
        # Winsock can allow a specific-address bind beside an existing wildcard bind.
        addresses = ("127.0.0.1", "0.0.0.0") if WINDOWS else ("127.0.0.1",)
        for address in addresses:
            with socket.socket(socket.AF_INET, kind) as probe:
                if WINDOWS:
                    probe.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
                elif kind == socket.SOCK_STREAM:
                    probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                probe.bind((address, port))

    for port in scene_ports.get(case, (LOGD_PORT, IPC_PORT, REDIS_SERVER_PORT, REDIS_PORT, REGISTRY_PORT) + GAME_PORTS):
        probe_port(port, socket.SOCK_STREAM)
    if case not in ("hold-timer", "hold-redis", "web-coro"):
        probe_port(UDP_PORT, socket.SOCK_DGRAM)
