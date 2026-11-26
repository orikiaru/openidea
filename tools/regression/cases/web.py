"""HTTP integration scenarios manage their own peer and service fixtures."""
from regression_platform import SUFFIX, WINDOWS
from ..probes import EnvironmentBlocked
from ..procs import REPO
from ..web import run_web
from ..web_coro import run_web_coro
from . import Case


def run_web_stop(options):
    if not WINDOWS:
        raise EnvironmentBlocked("web-stop is a Windows-specific diagnostic")
    from web_stop_probe import run_all
    run_all((options.bin_dir / ("webserver" + SUFFIX)).resolve(), REPO,
            "windows-" + options.expect_thread, options.port_offset)


CASES = (
    Case("web", "web", standalone=run_web),
    Case("web-coro", "web-coro", standalone=run_web_coro),
    Case("web-stop", "web-stop", standalone=run_web_stop, targeted_reason="Windows slow-reader shutdown diagnostic; platform-specific stop behavior"),
)
