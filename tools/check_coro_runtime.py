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
#include "oi_poll.h"
#include "oi_blocking_work_session.h"
#include "oi_errno.h"
#include "work_protocol/oi_rpcdata_workcompleted.h"
#include "work_protocol/rpc_enums.h"
#include <memory>
#include <cstdio>
#include <cstring>
#include <thread>
#include <array>
#include <uv.h>
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
static OI::Coro::Task ThreadGuardChild(std::shared_ptr<std::atomic<int>> pRuns)
{
    ++*pRuns;
    co_return;
}
static int32_t CopyBlocking(std::string sInput, std::string& sOutput)
{
    sOutput = std::move(sInput);
    return 0;
}
struct QueuedProbe
{
    OI::Net::Poll::QueuedCall stCall;
    std::atomic<int> iRuns{0};
    std::atomic<int>* pBad = nullptr;
};
static void QueuedProbeCallback(void* pArgument)
{
    auto* pProbe = static_cast<QueuedProbe*>(pArgument);
    if (!OI::Net::Poll::GetInstance().InLoopThread() || pProbe->iRuns.fetch_add(1) != 0)
    {
        ++*pProbe->pBad;
    }
}
struct BatchProbe
{
    OI::Net::Poll::QueuedCall stFirst;
    OI::Net::Poll::QueuedCall stSentinel;
    OI::Net::Poll::QueuedCall stReposted;
    std::atomic<int> iStage{0};
    std::atomic<int> iMarker{0};
    std::atomic<int> iBad{0};
};
static void StartBatchProbe(void* pArgument)
{
    auto* pProbe = static_cast<BatchProbe*>(pArgument);
    pProbe->stFirst.fnCall = [](void* pValue)
    {
        auto* pBatch = static_cast<BatchProbe*>(pValue);
        if (!OI::Net::Poll::GetInstance().InLoopThread() || pBatch->iStage.exchange(1) != 0)
        {
            ++pBatch->iBad;
        }
        OI::Net::Poll::GetInstance().PostOnLoop([](void* pMarker)
        {
            auto* pBatchMarker = static_cast<BatchProbe*>(pMarker);
            if (!OI::Net::Poll::GetInstance().InLoopThread() || pBatchMarker->iStage.load() != 2 ||
                pBatchMarker->iMarker.fetch_add(1) != 0)
            {
                ++pBatchMarker->iBad;
            }
        }, pBatch);
        OI::Net::Poll::GetInstance().PostOnLoop(pBatch->stReposted);
    };
    pProbe->stSentinel.fnCall = [](void* pValue)
    {
        auto* pBatch = static_cast<BatchProbe*>(pValue);
        if (!OI::Net::Poll::GetInstance().InLoopThread() || pBatch->iStage.exchange(2) != 1)
        {
            ++pBatch->iBad;
        }
    };
    pProbe->stReposted.fnCall = [](void* pValue)
    {
        auto* pBatch = static_cast<BatchProbe*>(pValue);
        if (!OI::Net::Poll::GetInstance().InLoopThread() || pBatch->iMarker.load() != 1 ||
            pBatch->iStage.exchange(3) != 2)
        {
            ++pBatch->iBad;
        }
    };
    pProbe->stFirst.pArgument = pProbe;
    pProbe->stSentinel.pArgument = pProbe;
    pProbe->stReposted.pArgument = pProbe;
    OI::Net::Poll::GetInstance().PostOnLoop(pProbe->stFirst);
    OI::Net::Poll::GetInstance().PostOnLoop(pProbe->stSentinel);
}
int main(int iCountParam, char** vArgumentsParam)
{
    if (iCountParam != 3 || OI::Base::StartFramework(vArgumentsParam[1]))
    {
        return 2;
    }
    if (std::strcmp(vArgumentsParam[2], "shutdown") == 0)
    {
        auto pStarted = std::make_shared<std::atomic<bool>>(false);
        auto pFinished = std::make_shared<std::atomic<bool>>(false);
        auto pRejected = std::make_shared<std::atomic<bool>>(false);
        auto pRelease = std::make_shared<std::atomic<bool>>(false);
        auto pDrained = std::make_shared<std::atomic<int>>(0);
        auto pRegistered = std::make_shared<std::atomic<bool>>(false);
        auto pCloseBad = std::make_shared<std::atomic<int>>(0);
        auto pCloseProbe = std::make_shared<QueuedProbe>();
        pCloseProbe->pBad = pCloseBad.get();
        pCloseProbe->stCall.fnCall = QueuedProbeCallback;
        pCloseProbe->stCall.pArgument = pCloseProbe.get();
        OI::Coro::StartTask([pStarted, pFinished, pRejected, pRelease, pDrained, pRegistered, pCloseProbe](OI::Coro::Token) -> OI::Coro::Task
        {
            auto stWork = OI::Coro::RunBlocking([pStarted, pFinished, pRelease]()
            {
                pStarted->store(true);
                for (int iWait = 0; iWait < 2000 && !pRelease->load(); ++iWait)
                {
                    uv_sleep(5);
                }
                pFinished->store(true);
                return int32_t{0};
            });
            while (!pStarted->load())
            {
                co_await OI::Coro::yield(1);
            }
            OI::Thread::ThreadPool::GetInstance().RequestStop(0);
            auto stRejected = OI::Coro::RunBlocking([]() { return int32_t{0}; });
            const int32_t iRejectedCode = co_await stRejected;
            pRejected->store(iRejectedCode == OI_ERR_CONNECT);
            struct DrainProbe
            {
                std::atomic<int>* pCount;
                std::atomic<bool>* pRegistered;
            };
            auto stProbe = std::make_shared<DrainProbe>(DrainProbe{pDrained.get(), pRegistered.get()});
            OI::Net::Poll::GetInstance().CallOnLoop([](void* pArgument)
            {
                auto* pProbe = static_cast<DrainProbe*>(pArgument);
                auto fnDone = [](void* pValue)
                {
                    ++*static_cast<std::atomic<int>*>(pValue);
                };
                OI::Net::BlockingWorkSessionManager::GetInstance().Drain(fnDone, pProbe->pCount);
                OI::Net::BlockingWorkSessionManager::GetInstance().Drain(fnDone, pProbe->pCount);
                pProbe->pRegistered->store(true);
            }, stProbe.get());
            while (!pRegistered->load())
            {
                co_await OI::Coro::yield(1);
            }
            OI::Net::Poll::GetInstance().PostOnLoop(pCloseProbe->stCall);
            pRelease->store(true);
        });
        const int iCode = OI::Base::RunFramework();
        std::printf("blocking shutdown ret=%d finished=%d rejected=%d drained=%d\n", iCode, pFinished->load(), pRejected->load(), pDrained->load());
        std::printf("queued shutdown runs=%d bad=%d\n", pCloseProbe->iRuns.load(), pCloseBad->load());
        return iCode == 0 && pFinished->load() && pRejected->load() && pDrained->load() == 2 &&
            pCloseProbe->iRuns.load() == 1 && pCloseBad->load() == 0 ? 0 : 9;
    }
    OI::Base::BytesStream stFields;
    stFields.WritePb(0, uint64_t{42});
    stFields.WritePb(17, uint64_t{99});
    OI::RpcData::WorkCompleted stMessage;
    stFields >> stMessage;
    OI::Base::BytesStream stPacket;
    stPacket.WritePb(999, stMessage);
    uint32_t iProtocol = 0;
    uint32_t iSize = 0;
    if (stMessage.iOperation != 42 ||
        stPacket.PreparePbHeader(iProtocol, iSize) != OI::Base::PB_HEADER_OK ||
        iProtocol != 999 || iProtocol == OI::RpcData::WORK_PROTOCOL_COMPLETED)
    {
        std::fprintf(stderr, "work protocol extension/unknown id failed\n");
        return 9;
    }
    std::printf("work protocol extension unknown id ok\n");
    auto pQueuedBad = std::make_shared<std::atomic<int>>(0);
    auto pQueued = std::make_shared<std::array<QueuedProbe, 4096>>();
    auto pBatch = std::make_shared<BatchProbe>();
    auto pLifetime = std::make_shared<int>(42);
    std::weak_ptr<int> pWeak = pLifetime;
    OI::Coro::StartTask([pLifetime, pQueuedBad, pQueued, pBatch](OI::Coro::Token) -> OI::Coro::Task
    {
        int iBad = 0;
        for (auto& stProbe : *pQueued)
        {
            stProbe.pBad = pQueuedBad.get();
            stProbe.stCall.fnCall = QueuedProbeCallback;
            stProbe.stCall.pArgument = &stProbe;
        }
        std::array<std::thread, 4> vPosters;
        for (size_t iPoster = 0; iPoster < vPosters.size(); ++iPoster)
        {
            vPosters[iPoster] = std::thread([pQueued, iPoster]()
            {
                for (size_t iIndex = iPoster; iIndex < pQueued->size(); iIndex += 4)
                {
                    OI::Net::Poll::GetInstance().PostOnLoop((*pQueued)[iIndex].stCall);
                }
            });
        }
        for (auto& stPoster : vPosters)
        {
            stPoster.join();
        }
        OI::Net::Poll::GetInstance().PostOnLoop(StartBatchProbe, pBatch.get());
        int iCompleted = 0;
        for (int iWait = 0; iWait < 5000; ++iWait)
        {
            iCompleted = 0;
            for (const auto& stProbe : *pQueued)
            {
                iCompleted += stProbe.iRuns.load() == 1;
            }
            if (iCompleted == static_cast<int>(pQueued->size()) && pBatch->iStage.load() == 3)
            {
                break;
            }
            co_await OI::Coro::yield(1);
        }
        const int iQueuedBad = pQueuedBad->load() + pBatch->iBad.load() +
            (iCompleted != static_cast<int>(pQueued->size())) + (pBatch->iStage.load() != 3);
        std::printf("queued calls completed=%d batch=%d bad=%d\n", iCompleted, pBatch->iStage.load(), iQueuedBad);
        iBad += iQueuedBad;
        auto pThreadRuns = std::make_shared<std::atomic<int>>(0);
        auto pWrappers = std::make_shared<std::atomic<int>>(0);
        auto pRejected = std::make_shared<std::atomic<int>>(0);
        auto pWrongError = std::make_shared<std::atomic<int>>(0);
        auto stExisting = ThreadGuardChild(pThreadRuns);
        co_await stExisting;
        OI::Coro::TaskScope stScope;
        // 故意违规从外部线程创建和启动，验证 ST 拒绝及 MT 仍允许。
        auto* pScope = &stScope;
        auto* pExisting = &stExisting;
        std::thread stForeign([pThreadRuns, pWrappers, pRejected, pWrongError, pScope, pExisting]()
        {
            try
            {
                auto stTask = ThreadGuardChild(pThreadRuns);
            }
            catch (const std::logic_error& stError)
            {
                ++*pRejected;
                if (std::string(stError.what()) != "single-thread coroutine must start on the poll thread")
                {
                    ++*pWrongError;
                }
            }
            try
            {
                auto stTask = OI::Coro::StartTask([pThreadRuns, pWrappers](OI::Coro::Token)
                {
                    ++*pWrappers;
                    return ThreadGuardChild(pThreadRuns);
                });
            }
            catch (const std::logic_error& stError)
            {
                ++*pRejected;
                if (std::string(stError.what()) != "single-thread coroutine must start on the poll thread")
                {
                    ++*pWrongError;
                }
            }
            try
            {
                pScope->Start([pThreadRuns, pWrappers](OI::Coro::Token, OI::Coro::StopToken)
                {
                    ++*pWrappers;
                    return ThreadGuardChild(pThreadRuns);
                });
            }
            catch (const std::logic_error& stError)
            {
                ++*pRejected;
                if (std::string(stError.what()) != "single-thread coroutine must start on the poll thread")
                {
                    ++*pWrongError;
                }
            }
            try
            {
                pExisting->Start();
            }
            catch (const std::logic_error& stError)
            {
                ++*pRejected;
                if (std::string(stError.what()) != "single-thread coroutine must start on the poll thread")
                {
                    ++*pWrongError;
                }
            }
        });
        stForeign.join();
        co_await stScope.Join();
        for (int iWait = 0; iWait < 500 && pThreadRuns->load() < 4 && pRejected->load() == 0; ++iWait)
        {
            co_await OI::Coro::yield(1);
        }
#ifdef _SINGLE_THREAD_
        if (pRejected->load() != 4 || pWrappers->load() != 0 || pThreadRuns->load() != 1 || pWrongError->load() != 0)
#else
        if (pRejected->load() != 0 || pWrappers->load() != 2 || pThreadRuns->load() != 4)
#endif
        {
            std::fprintf(stderr, "foreign task rejects=%d wrappers=%d runs=%d\n", pRejected->load(), pWrappers->load(), pThreadRuns->load());
            ++iBad;
        }
        std::printf("foreign task rejects=%d wrappers=%d runs=%d bad=%d\n", pRejected->load(), pWrappers->load(), pThreadRuns->load(), iBad);
        std::string sOutput;
        auto stCopy = OI::Coro::RunBlocking(CopyBlocking, std::string(512, 'r'), sOutput);
        const int32_t iCopyCode = co_await stCopy;
        if (iCopyCode != 0 || sOutput != std::string(512, 'r'))
        {
            std::fprintf(stderr, "blocking argument lifetime failed\n");
            ++iBad;
        }
        std::printf("blocking argument lifetime bad=%d\n", iBad);
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
        auto pRelease = std::make_shared<std::atomic<bool>>(false);
        auto pExecuted = std::make_shared<std::atomic<int>>(0);
        std::vector<OI::Coro::TaskResult<int32_t>> vBlocking;
        for (int iIndex = 0; iIndex < 1025; ++iIndex)
        {
            vBlocking.push_back(OI::Coro::RunBlocking([pRelease, pExecuted]()
            {
                ++*pExecuted;
                for (int iWait = 0; iWait < 1000 && !pRelease->load(); ++iWait)
                {
                    uv_sleep(5);
                }
                return int32_t{0};
            }));
        }
        auto* pReleaseOwner = new std::shared_ptr<std::atomic<bool>>(pRelease);
        OI::Net::Poll::GetInstance().PostOnLoop([](void* pArgument)
        {
            std::unique_ptr<std::shared_ptr<std::atomic<bool>>> pOwner(
                static_cast<std::shared_ptr<std::atomic<bool>>*>(pArgument));
            (*pOwner)->store(true);
        }, pReleaseOwner);
        int iRejected = 0;
        for (const auto& stWork : vBlocking)
        {
            const int32_t iWorkCode = co_await stWork;
            if (iWorkCode == OI_ERR_BUSY)
            {
                ++iRejected;
            }
            else if (iWorkCode != 0)
            {
                std::fprintf(stderr, "unexpected capacity status=%d\n", iWorkCode);
                ++iBad;
            }
        }
        if (iRejected != 1 || pExecuted->load() != 1024)
        {
            std::fprintf(stderr, "capacity rejected=%d executed=%d\n", iRejected, pExecuted->load());
            ++iBad;
        }
        std::printf("blocking capacity rejected=%d executed=%d\n", iRejected, pExecuted->load());
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
            result = subprocess.run([str(binary), str(config), "normal"], capture_output=True, text=True,
                                    timeout=15, env=env)
            print(mode, result.stdout, result.stderr, flush=True)
            result.check_returncode()
            assert "pure coroutine ret=0 released=1" in result.stdout
            assert "shared task runs=1 bad=0" in result.stdout
            assert ("foreign task rejects=0 wrappers=2 runs=4 bad=0" if mode == "mt" else
                    "foreign task rejects=4 wrappers=0 runs=1 bad=0") in result.stdout
            if mode == "st":
                assert result.stderr.count("warning: coroutine start rejected: current thread is not the single-thread poll thread") == 4
            assert "blocking argument lifetime bad=0" in result.stdout
            assert "blocking capacity rejected=1 executed=1024" in result.stdout
            assert "work protocol extension unknown id ok" in result.stdout
            assert "queued calls completed=4096 batch=3 bad=0" in result.stdout
            shutdown = subprocess.run([str(binary), str(config), "shutdown"], capture_output=True, text=True,
                                      timeout=15, env=env)
            print(mode, shutdown.stdout, shutdown.stderr, flush=True)
            shutdown.check_returncode()
            assert "blocking shutdown ret=0 finished=1 rejected=1 drained=2" in shutdown.stdout
            assert "queued shutdown runs=1 bad=0" in shutdown.stdout


if __name__ == "__main__":
    main()
