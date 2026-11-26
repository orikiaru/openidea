"""回归用例注册表。新注册的用例默认进入全量，定向运行需说明原因。"""
CLIENT_SECONDS = {"hold": 120, "coro": 180, "empty": 180}

class Case:
    """One regression case; group modules override the hooks they use."""

    def __init__(self, name, mode, *, scene=None, standalone=None, targeted_reason=None):
        self.name = name
        self.targeted_reason = targeted_reason
        self.standalone = standalone
        self.scene = scene
        self.mode = mode
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
from . import admission, coro, db, echo, hold, log, multi, redis, udp, web  # noqa: E402

def register_cases(groups):
    registry = {}
    for entries in groups:
        for entry in entries:
            if entry.name in registry or entry.name == "all":
                raise ValueError("duplicate or reserved regression case: " + entry.name)
            registry[entry.name] = entry
    return registry


REGISTRY = register_cases((echo.CASES, log.CASES, redis.CASES, hold.CASES, multi.CASES,
                           udp.CASES, admission.CASES, db.CASES, coro.CASES, web.CASES))
TARGETED_CASES = {name: entry.targeted_reason for name, entry in REGISTRY.items() if entry.targeted_reason is not None}
ALL_CASES = tuple(name for name in REGISTRY if name not in TARGETED_CASES)


def normalize_case_name(name):
    return name[5:] if name.startswith("test-") else name


def get_case(name):
    return REGISTRY[normalize_case_name(name)]
