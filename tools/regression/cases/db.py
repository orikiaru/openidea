"""db 组：MySQL 集成和阻塞唤醒基准。"""
from ..config import set_test_flags
from ..probes import EnvironmentBlocked
from . import Case


class DbCase(Case):
    def configure(self, ctx, name, text):
        case = ctx.name
        if name == "ipcclient":
            if case in ("blocking-wake", "mysql-wake"):
                text = text.replace("[Test]", "[Test]\nwake_case = \"" + ("blocking" if case == "blocking-wake" else "mysql") + "\"", 1)
            text = set_test_flags(text, ctx.mode)
        return text

    def environment_blocked(self, ctx, text):
        if ctx.name == "mysql" and "mysql environment blocked:" in text:
            raise EnvironmentBlocked("MySQL driver disabled in ipcclient build")

    def check(self, ctx):
        case, text = ctx.name, ctx.text
        if case in ("blocking-wake", "mysql-wake"):
            if case == "blocking-wake":
                assert "wake benchmark default_empty_serial n=100000" in text
                for name in ("native_coordinator_to_worker", "native_worker_to_coordinator", "native_roundtrip"):
                    assert f"wake benchmark {name} n=100000" in text
                stamp = ctx.args.bin_dir / "oi_test_hooks.txt"
                if stamp.exists() and stamp.read_text().strip().upper() in ("ON", "1", "TRUE") and \
                        "wake benchmark trace skipped multi-thread" not in text:
                    for name in ("trace_submit_to_worker", "trace_worker", "trace_done_to_post",
                                 "trace_post_to_poll", "trace_poll_to_resume", "trace_finish_to_resume", "trace_total"):
                        assert f"wake benchmark {name} n=100000" in text
            if case == "mysql-wake":
                assert "wake benchmark blocking_finish_to_resume n=1000" in text
                assert "wake benchmark mysql_direct_submit_to_worker_start n=1000" in text
                assert "wake benchmark mysql_direct_worker_start_to_finish n=1000" in text
                assert "wake benchmark mysql_direct_finish_to_resume n=1000" in text
                assert "wake benchmark mysql_execute_finish_to_resume_including_cleanup n=1000" in text
                assert "wake benchmark mysql_reset_finish_to_resume_including_cleanup n=1000" in text
                assert "wake benchmark mysql_call_total n=1000" in text
        elif case == "mysql":
            assert "mysql row mapping bad=0" in text
            assert "mysql rows bad=0" in text
            assert "mysql done bad=0" in text
        return False

CASES = (DbCase("mysql", "db"), DbCase("mysql-wake", "db"), DbCase("blocking-wake", "db"))
