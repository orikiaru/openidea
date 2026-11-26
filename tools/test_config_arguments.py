"""Exercise configuration argument handling against the built executables."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from regression_platform import SUFFIX

if not __debug__:
    raise SystemExit("config arguments regression requires Python assertions enabled")

NAMES = ("ipcserver", "ipcclient", "logd", "redisserver", "registry", "webserver", "webclient")


def run(binary, cwd, arguments):
    result = subprocess.run([str(binary), *map(str, arguments)], cwd=cwd,
                            capture_output=True, text=True, errors="replace", timeout=15)
    return result.returncode, result.stdout + result.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, required=True)
    args = parser.parse_args()
    binaries = {name: (args.bin_dir / (name + SUFFIX)).resolve() for name in NAMES}
    with tempfile.TemporaryDirectory(prefix="oi config arguments ") as directory:
        root = Path(directory)
        work = root / "bin"
        config = root / "config"
        work.mkdir()
        config.mkdir()
        explicit = root / "explicit config.toml"
        legacy = root / "legacy config.toml"
        # Existing malformed files prove that the selected path actually reaches the parser.
        explicit.write_text("[invalid\n", encoding="utf-8")
        legacy.write_text("[invalid\n", encoding="utf-8")
        for name, binary in binaries.items():
            (config / (name + ".toml")).write_text("[invalid\n", encoding="utf-8")

            def selected(arguments, path):
                code, output = run(binary, work, arguments)
                assert code != 0 and "framework init failed: parse config " + str(path) in output, (name, arguments, code, output)

            selected([], "../config/" + name + ".toml")
            selected(["--config", explicit], explicit)
            for arguments in (["--config"], ["--config", ""], ["--config", explicit, "--config", legacy],
                              ["--config", "--other"]):
                code, output = run(binary, work, arguments)
                assert (code & 255) == 250 and "--config requires one nonempty path and may only be specified once" in output, (name, arguments, code, output)
                assert "framework init failed: parse config" not in output, (name, arguments, code, output)
        selected_binary = binaries["ipcserver"]
        for arguments, expected in (([legacy], legacy), ([legacy, "--config", explicit], explicit),
                                    (["--config", explicit, legacy], explicit)):
            code, output = run(selected_binary, work, arguments)
            assert code != 0 and "framework init failed: parse config " + str(expected) in output, (arguments, code, output)
        for name, probes in (("ipcclient", ("--thread-mode", "--test-hooks", "--timer-before-init")),
                             ("logd", ("--test-hooks",))):
            for probe in probes:
                baseline = run(binaries[name], work, [probe])
                code, output = baseline
                if probe == "--thread-mode":
                    assert code == 0 and output.strip() in ("thread_mode=st", "thread_mode=mt"), baseline
                elif probe == "--timer-before-init":
                    assert code == 0 and "called=0 bad=0" in output, baseline
                else:
                    key = "blocking_wake_trace" if name == "ipcclient" else "log_fault"
                    assert (code, output.strip()) in ((40, key + "=0"), (41, key + "=1")), baseline
                assert "framework init failed: parse config" not in baseline[1], (name, probe, baseline)
                for arguments in ([probe, "--config", explicit], ["--config", explicit, probe]):
                    actual = run(binaries[name], work, arguments)
                    assert actual == baseline, (name, arguments, baseline, actual)
        # A valid explicit file must initialize the framework even without its default file.
        (config / "ipcclient.toml").unlink()
        valid = root / "valid config.toml"
        repository = Path(__file__).resolve().parents[1]
        valid.write_bytes((repository / "deploy/config/ipcclient.toml").read_bytes())
        for arguments in (("--poll-udp-close", "--config", valid),
                          ("--config", valid, "--poll-udp-close")):
            code, output = run(binaries["ipcclient"], work, arguments)
            assert code == 0 and "poll udp close bound=1 paused=1 loop=0" in output, (arguments, code, output)
    print("config arguments PASS")


if __name__ == "__main__":
    main()
