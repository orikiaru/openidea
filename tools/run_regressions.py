"""WSL: python3 tools/run_regressions.py --case all [--clock-fault-so /tmp/oi-clock.so].

只启动仓库现有进程，临时配置和日志自动清理；不会终止不属于本次测试的进程。
先编出待测版本，单、多线程分别运行一次。Redis 默认使用已有的源码编译产物。
"""
import sys

from textio import use_utf8_children
use_utf8_children()

if not __debug__:
    raise SystemExit("run_regressions.py requires assertions; do not use Python -O")

from regression import cases, config, ports, probes, procs, rpc_faults, runner, web
from regression.cases import ALL_CASES, REGISTRY, Case, get_case
from regression.config import replace, shift_ports
from regression.probes import (CAP, EnvironmentBlocked, bytes_on_disk, check_admission_rounds,
                               check_echo, check_hold_blocking, check_hold_timer, disk_failure,
                               echo_total, log_payload_on_disk, set_echo_total)
from regression.procs import (Context, cleanup_owned, copy_runtime_dlls,
                              regression_directory)
from regression.rpc_faults import reset_connections, rpc_ingress_faults
from regression.runner import main as _run_main, run, run_case
from regression.web import (check_http_disconnect, check_http_pool, check_https,
                            check_multipart_response, check_url_client, check_web_bind_restart,
                            check_web_json, check_web_stream_length, check_web_transport,
                            run_web, run_web_coro)
from regression_platform import SUFFIX, WINDOWS, launch, pause, rss_kib, stop

_PORT_NAMES = ("REDIS_PORT", "LOGD_PORT", "IPC_PORT", "IPC_FALLBACK_PORT", "REDIS_SERVER_PORT",
               "REGISTRY_PORT", "GAME_PORTS", "UDP_PORT", "BASE_PORTS", "PORT_OFFSET")


def __getattr__(name):
    # 端口常量由 apply_port_offset 重新绑定，读取时转发，避免拿到平移前的值。
    if name in _PORT_NAMES:
        return getattr(ports, name)
    raise AttributeError("module %r has no attribute %r" % (__name__, name))


def main():
    _run_main(__doc__)


if __name__ == "__main__":
    main()
