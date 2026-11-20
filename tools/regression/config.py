"""toml 文本改写助手。"""
import re

from . import ports


def shift_ports(text):
    def shifted(match):
        port = int(match.group(2))
        return match.group(1) + str(port + ports.PORT_OFFSET if port in ports.BASE_PORTS else port) + match.group(3)
    return re.sub(r'(?m)^(port\s*=\s*")(\d+)(")', shifted, text)


def replace(text, key, value):
    return re.sub(r"(?m)^" + re.escape(key) + r"\s*=.*$", key + " = " + value, text)


def set_test_flags(text, mode):
    for flag in ("echo", "log", "redis", "hold", "udp", "multi", "pool", "db", "coro", "empty"):
        text = replace(text, flag, "true" if mode == flag else "false")
    return text
