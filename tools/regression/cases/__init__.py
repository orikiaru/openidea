"""回归用例注册表：用例名、分组、独立入口和超时。

--case all 的用例名和顺序与旧 run_regressions.ALL_CASES 完全一致。
"""
CLIENT_SECONDS = {"hold": 120, "coro": 180, "empty": 180}

ALL_CASES = ("echo", "log", "log-backpressure", "redis", "redis-limit", "redis-shutdown", "redis-queue-timeout", "rpc-ingress", "redis-backpressure-bytes", "redis-backpressure-count", "redis-fairness", "hold", "rpc-admission-count", "rpc-admission-bytes", "reset", "tcp-error-rearm", "tcp-close-reclaim", "disk-full", "registry", "udp", "multi", "local", "web", "web-coro")

# 只在定向运行时使用、不进 --case all 的用例。
TARGETED_CASES = ("log-contract", "mysql", "mysql-wake", "blocking-wake", "coro", "empty", "hold-blocking", "hold-timer", "hold-redis", "clock-forward", "clock-back", "registry-static")


def case_scene(case):
    return case[len("hold-"):] if case in ("hold-blocking", "hold-timer", "hold-redis") else None


def case_mode(case):
    scene = case_scene(case)
    mode = "hold" if scene or case.startswith(("clock-", "rpc-admission-")) else ("echo" if case in ("reset", "registry", "rpc-ingress", "tcp-error-rearm", "tcp-close-reclaim") else case)
    mode = "db" if case in ("mysql", "mysql-wake", "blocking-wake") else mode
    mode = "pool" if case in ("local", "registry-static") else mode
    mode = "log" if case in ("disk-full", "log-backpressure", "log-contract") else mode
    mode = "redis" if case.startswith("redis-") else mode
    return mode


class Case:
    """One regression case; group modules override the hooks they use."""

    def __init__(self, name, group, standalone=False):
        self.name = name
        self.group = group
        self.standalone = standalone
        self.scene = case_scene(name)
        self.mode = case_mode(name)
        self.fairness = name == "redis-fairness"
        self.pressure = name.startswith("redis-backpressure-") or self.fairness
        # 与旧 run() 的 deadline 表达式一致。
        self.timeout = 20 if self.pressure or name == "redis-shutdown" else CLIENT_SECONDS.get(self.mode, 60)

    def configure(self, ctx, name, text):
        return text

    def prepare_directory(self, ctx):
        pass

    def prepare(self, ctx):
        pass

    def after_logd(self, ctx):
        pass

    def after_redis(self, ctx):
        pass

    def scenario(self, ctx):
        pass

    def client_env(self, ctx, env):
        return env

    def on_poll(self, ctx):
        pass

    def logd_start_failure(self, ctx, logd):
        return False

    def logd_poll_failure(self, ctx, logd):
        return False

    def environment_blocked(self, ctx, text):
        pass

    def check(self, ctx):
        return False

    def diagnostics(self, ctx):
        pass

# Case 必须先定义，组模块才能 from . import Case。
from . import admission, coro, db, echo, hold, log, multi, redis, udp  # noqa: E402

REGISTRY = {}
for _module in (echo, log, redis, hold, multi, udp, admission, db, coro):
    for _case in _module.CASES:
        REGISTRY[_case.name] = _case
for _case in (Case("web", "web", standalone=True), Case("web-coro", "web", standalone=True)):
    REGISTRY[_case.name] = _case


def get_case(name):
    return REGISTRY[name]
