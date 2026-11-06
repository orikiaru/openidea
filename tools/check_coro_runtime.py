"""Check pure-coroutine static linking in both modes, without any RPC/HTTP calls.

WSL: python3 tools/check_coro_runtime.py [--build-dir ~/openidea-build-gcc13] [--asan]
Uses the existing build's flags/libraries and removes all temporary artifacts.
Business/network regressions remain in ipcclient's managers.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

SOURCE = r'''
#include "oi_app.h"
#include "oi_coro.h"
#include "oi_thread.h"
#include <memory>
#include <cstdio>
static OI::Coro::TaskResult<int> Child(int iValueParam)
{
    co_await OI::Coro::yield(0);
    co_return iValueParam;
}
static OI::Coro::TaskResult<std::string> SharedChild(std::shared_ptr<std::atomic<int>> pRunsParam)
{
    ++*pRunsParam;
    co_await OI::Coro::yield(20);
    co_return std::string(256, 's');
}
int main(int iCountParam, char** vArgumentsParam)
{
    if (iCountParam != 2 || OI::Base::StartFramework(vArgumentsParam[1]))
    {
        return 2;
    }
    auto pLifetime = std::make_shared<int>(42);
    std::weak_ptr<int> pWeak = pLifetime;
    OI::Coro::StartTask([pLifetime](OI::Coro::Token) -> OI::Coro::Task
    {
        int iBad = 0;
        for (int iIndex = 0; iIndex < 1024; ++iIndex)
        {
            if ((co_await Child(*pLifetime)) != 42)
            {
                ++iBad;
            }
        }
        auto pRuns = std::make_shared<std::atomic<int>>(0);
        auto pBad = std::make_shared<std::atomic<int>>(0);
        auto stShared = OI::Coro::StartTask([pRuns](OI::Coro::Token)
        {
            return SharedChild(pRuns);
        });
        std::vector<OI::Coro::Task> vWaiters;
        for (int iIndex = 0; iIndex < 64; ++iIndex)
        {
            vWaiters.push_back(OI::Coro::StartTask([stShared, pBad](OI::Coro::Token) -> OI::Coro::Task
            {
                for (int iRound = 0; iRound < 2; ++iRound)
                {
                    if ((co_await stShared) != std::string(256, 's'))
                    {
                        ++*pBad;
                    }
                }
            }));
        }
        for (const auto& stWaiter : vWaiters)
        {
            co_await stWaiter;
        }
        iBad += pBad->load() + (pRuns->load() != 1);
        std::printf("shared task runs=%d bad=%d\n", pRuns->load(), iBad);
        OI::Thread::ThreadPool::GetInstance().RequestStop(iBad ? 9 : 0);
    });
    pLifetime.reset();
    const int iCode = OI::Base::RunFramework();
    std::printf("pure coroutine ret=%d released=%d\n", iCode, pWeak.expired());
    return iCode == 0 && pWeak.expired() ? 0 : 9;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path.home() / "openidea-build-gcc13")
    parser.add_argument("--asan", action="store_true")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    entry = next(row for row in json.loads((build / "compile_commands.json").read_text())
                 if row["file"].endswith("/ipcclient/src/main.cpp"))
    link_dir = build / "modules/ipcclient"
    link_base = shlex.split((link_dir / "CMakeFiles/ipcclient.dir/link.txt").read_text())
    for mode in ("mt", "st"):
        with tempfile.TemporaryDirectory(prefix="oi-coro-runtime-") as directory:
            root = Path(directory)
            source, obj, binary = root / "probe.cpp", root / "probe.o", root / "probe"
            source.write_text(SOURCE)
            config = root / "config.toml"
            config.write_text("[ThreadPool]\nthreadcount=4\n")
            compile_args = [part for part in shlex.split(entry["command"]) if not part.startswith("-D_SINGLE_THREAD_")]
            compile_args[compile_args.index("-o") + 1] = str(obj)
            compile_args[compile_args.index("-c") + 1] = str(source)
            if mode == "st":
                compile_args.append("-D_SINGLE_THREAD_")
            if args.asan:
                compile_args += ["-O1", "-fsanitize=address", "-fno-omit-frame-pointer"]
            subprocess.run(compile_args, cwd=entry["directory"], check=True)
            link_args = [part for part in link_base if not part.endswith(".o")]
            suffix = "_m_static.a" if mode == "mt" else "_static.a"
            link_args = [re.sub(r"lib(base|net|proto|timer|logger|redis|http)(?:_m)?_static\.a$",
                                lambda match: "lib" + match[1] + suffix, part) for part in link_args]
            link_args.insert(1, str(obj))
            link_args[link_args.index("-o") + 1] = str(binary)
            if args.asan:
                link_args.append("-fsanitize=address")
            subprocess.run(link_args, cwd=link_dir, check=True)
            env = dict(os.environ)
            if args.asan:
                env["ASAN_OPTIONS"] = "detect_leaks=0:abort_on_error=1"
            result = subprocess.run([str(binary), str(config)], capture_output=True, text=True,
                                    timeout=15, env=env)
            print(mode, result.stdout, result.stderr, flush=True)
            result.check_returncode()
            assert "pure coroutine ret=0 released=1" in result.stdout
            assert "shared task runs=1 bad=0" in result.stdout


if __name__ == "__main__":
    main()
