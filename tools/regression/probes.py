"""回归断言和磁盘/日志探测助手。"""
import re

CAP = 2 * 1024**3


class EnvironmentBlocked(RuntimeError):
    pass


def disk_failure(exit_code, message):
    return exit_code is not None and exit_code != 0 and bool(re.search(
        r"(?m)^logd (?:fwrite|fflush|injected fwrite ENOSPC) failed: .+$", message))


def echo_total_match(config):
    section = re.search(r"(?ms)^\[TestEcho\][ \t]*\n(.*?)(?=^\[|\Z)", config)
    if not section:
        raise RuntimeError("missing [TestEcho] configuration")
    matches = list(re.finditer(r"(?m)^total[ \t]*=[ \t]*(\d+)[ \t]*(?:#.*)?$", section.group(1)))
    if len(matches) != 1 or int(matches[0].group(1)) <= 0:
        raise RuntimeError("invalid or duplicate [TestEcho] total")
    match = matches[0]
    return section.start(1) + match.start(1), section.start(1) + match.end(1)


def echo_total(config):
    start, end = echo_total_match(config)
    return int(config[start:end])


def set_echo_total(config, total):
    start, end = echo_total_match(config)
    return config[:start] + str(total) + config[end:]


def check_echo(text, expected_total):
    assert re.search(r"ipc mode=\S+ total=%d ok=%d mismatch=0 rpc_fail=0\b" % (expected_total, expected_total), text), "echo count mismatch"
    assert text.count("ipc client connect sid=") == 1, "echo test started more than once"


def check_admission_rounds(text):
    lines = re.findall(r"hold rpc admission round=[^\r\n]*", text)
    assert len(lines) == 3, "expected exactly three admission rounds"
    expected = {"hold rpc admission round=%d ok=2 busy=4 bad=0" % i for i in range(3)}
    assert {line.strip() for line in lines} == expected, "invalid or duplicate admission round"


def check_hold_timer(text):
    assert "hold timer uninitialized update=1 convert=1 tick=0 ticks=0 unchanged=1 bad=0" in text
    assert re.search(r"hold timer conversion offset=[1-9][0-9]* bad=0", text)
    assert "hold framework timer zero=-6 negative=-6 valid=0 invalid_calls=0 valid_calls=1 bad=0" in text
    assert "hold timer boundaries phase=3 calls=18 bad=0" in text
    assert "hold timer exceptions executed=5 destroyed=5 bad=0" in text
    assert "hold timer once calls=2 destroyed=1 bad=0" in text
    assert re.search(r"hold timer repeat calls=3 throws=1 .* bad=0", text)
    assert "hold timer context stopped=1 extended=3 remaining=0/1/0 zero=1 negative=1 bad=0" in text
    assert re.search(r"hold timer update tick=[1-9][0-9]* second=[1-9][0-9]* overlap=0/0 skipped=[0-9]+ bad=0", text)


def check_hold_blocking(text):
    for marker in ("hold blocking delay seed=20261110 min_ms=1 max_ms=20",
                   "hold blocking tasks=128 waiters=64 runs=1 bad=0",
                   "hold blocking read started=1 timer=1 rpc=1 early=0 content=1 bad=0",
                   "hold blocking categories bad=0", "hold blocking access bad=0", "hold blocking fifo type bad=0", "hold blocking poll register bad=0",
                   "hold poll unregister read=1 write_after=0 close_after=0 reused=1 bad=0",
                   "hold poll worker wake worker=1 poll=1 done=1 bad=0",
                   "hold poll wake burst posts=10000 done=10000 bad=0"):
        assert marker in text, marker


def bytes_on_disk(root):
    return sum(p.stat().st_size for p in root.rglob("*") if p.is_file() and not p.is_symlink())


def log_payload_on_disk(root):
    return sum(p.stat().st_size for p in root.rglob("info") if p.is_file() and not p.is_symlink())
