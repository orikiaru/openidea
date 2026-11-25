#!/usr/bin/env python3
"""规范路由：按 .agents/rules/index.toml 把关键词、改动路径和改动内容路由到必读文档。

用法：
  python tools/agent_route.py --write   重新生成 AGENTS.md 的关键词表
  python tools/agent_route.py --check   校验索引（文档存在、正则可编译）和 AGENTS.md 表是否同步
  python tools/agent_route.py hook      Claude Code hook 入口，从 stdin 读事件 JSON

hook 行为：
  UserPromptSubmit      用户消息命中关键词或路径，注入「先读哪份文档」。
  PreToolUse(Agent)     把命中的必读文档追加进 subagent 的提示词。
  PostToolUse(Read)     记下本会话（subagent 单独记）读过的文件。
  PreToolUse(Edit 等)   目标路径或 C++ 新内容命中、对应文档本会话没读过时拒绝编辑，读完重试即可。
hook 内部出错一律放行，只往 stderr 写原因；环境变量 OI_ROUTE_OFF=1 整体关闭。
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
INDEX_PATH = REPO_ROOT / ".agents" / "rules" / "index.toml"
AGENTS_PATH = REPO_ROOT / "AGENTS.md"
TABLE_BEGIN = "<!-- route-index:begin 由 tools/agent_route.py --write 生成，改 .agents/rules/index.toml -->"
TABLE_END = "<!-- route-index:end -->"
CPP_SUFFIXES = {".h", ".hpp", ".hh", ".inl", ".cpp", ".cc", ".cxx", ".ipp"}
EDIT_TOOLS = {"Edit", "Write", "MultiEdit"}
AGENT_TOOLS = {"Agent", "Task"}
PATH_TOKEN = re.compile(r"(?:[A-Za-z]:)?[A-Za-z0-9_.\-]*(?:[/\\][A-Za-z0-9_.\-*]+)+")
MSYS_DRIVE = re.compile(r"^/([A-Za-z])/")


class TomlUnavailable(Exception):
    pass


@dataclass
class Route:
    sId: str
    sDoc: str
    sSection: str
    sExample: str
    vKeywords: list[str]
    vPaths: list[str]
    vContent: list[str]
    vKeywordRegex: list[re.Pattern] = field(default_factory=list)
    vPathRegex: list[re.Pattern] = field(default_factory=list)
    vContentRegex: list[re.Pattern] = field(default_factory=list)

    def DocLabel(self) -> str:
        if self.sSection:
            return f"{self.sDoc}（{self.sSection}）"
        return self.sDoc


def GlobToRegex(sGlob: str) -> re.Pattern:
    vParts = []
    iIndex = 0
    while iIndex < len(sGlob):
        sChar = sGlob[iIndex]
        if sGlob.startswith("**/", iIndex):
            vParts.append("(?:.*/)?")
            iIndex += 3
            continue
        if sGlob.startswith("**", iIndex):
            vParts.append(".*")
            iIndex += 2
            continue
        if sChar == "*":
            vParts.append("[^/]*")
        elif sChar == "?":
            vParts.append("[^/]")
        else:
            vParts.append(re.escape(sChar))
        iIndex += 1
    return re.compile("^" + "".join(vParts) + "$")


def KeywordToRegex(sKeyword: str) -> re.Pattern:
    sBody = re.escape(sKeyword)
    if sKeyword[0].isascii() and (sKeyword[0].isalnum() or sKeyword[0] == "_"):
        sBody = r"(?<![A-Za-z0-9_])" + sBody
    if sKeyword[-1].isascii() and (sKeyword[-1].isalnum() or sKeyword[-1] == "_"):
        sBody = sBody + r"(?![A-Za-z0-9_])"
    return re.compile(sBody, re.IGNORECASE)


def LoadRoutes() -> list[Route]:
    try:
        import tomllib
    except ImportError:
        try:
            import tomli as tomllib
        except ImportError:
            raise TomlUnavailable(f"Python {sys.version.split()[0]} 没有 tomllib（需要 3.11+）或 tomli")

    with INDEX_PATH.open("rb") as stFile:
        mData = tomllib.load(stFile)
    vRoutes = []
    for mItem in mData.get("route", []):
        stRoute = Route(
            sId=mItem["id"],
            sDoc=mItem["doc"],
            sSection=mItem.get("section", ""),
            sExample=mItem.get("example", ""),
            vKeywords=list(mItem.get("keywords", [])),
            vPaths=list(mItem.get("paths", [])),
            vContent=list(mItem.get("content", [])),
        )
        stRoute.vKeywordRegex = [KeywordToRegex(sKeyword) for sKeyword in stRoute.vKeywords]
        stRoute.vPathRegex = [GlobToRegex(sGlob) for sGlob in stRoute.vPaths]
        stRoute.vContentRegex = [re.compile(sPattern) for sPattern in stRoute.vContent]
        vRoutes.append(stRoute)
    return vRoutes


def RelativePath(sPath: str) -> str | None:
    if not sPath:
        return None
    if os.name == "nt":
        # Git Bash 写法 /e/openidea/... 换成盘符路径
        sPath = MSYS_DRIVE.sub(lambda stMatch: stMatch.group(1) + ":/", sPath)
    stPath = Path(sPath)
    if not stPath.is_absolute():
        stPath = REPO_ROOT / stPath
    try:
        return stPath.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return None


def MatchPath(stRoute: Route, sRelPath: str) -> str | None:
    for sGlob, stRegex in zip(stRoute.vPaths, stRoute.vPathRegex):
        if stRegex.match(sRelPath):
            return sGlob
    return None


def MatchText(vRoutes: list[Route], sText: str) -> dict[str, list[tuple[Route, str]]]:
    """按文档归并命中：关键词命中，或文本里出现的路径命中 paths。"""
    mHits: dict[str, list[tuple[Route, str]]] = {}
    vPathTokens = []
    for sToken in PATH_TOKEN.findall(sText):
        sRelPath = RelativePath(sToken.replace("\\", "/"))
        if sRelPath:
            vPathTokens.append(sRelPath)
    for stRoute in vRoutes:
        sReason = None
        for sKeyword, stRegex in zip(stRoute.vKeywords, stRoute.vKeywordRegex):
            if stRegex.search(sText):
                sReason = sKeyword
                break
        if sReason is None:
            for sRelPath in vPathTokens:
                if MatchPath(stRoute, sRelPath):
                    sReason = sRelPath
                    break
        if sReason is not None:
            mHits.setdefault(stRoute.sDoc, []).append((stRoute, sReason))
    return mHits


def FormatHits(mHits: dict[str, list[tuple[Route, str]]]) -> list[str]:
    vLines = []
    for sDoc, vItems in mHits.items():
        vReasons = "、".join(dict.fromkeys(sReason for _, sReason in vItems))
        vSections = "、".join(dict.fromkeys(stRoute.sSection for stRoute, _ in vItems if stRoute.sSection))
        vExamples = "；".join(dict.fromkeys(stRoute.sExample for stRoute, _ in vItems if stRoute.sExample))
        sLine = f"- {sDoc}"
        if vSections:
            sLine += f"（{vSections}）"
        sLine += f"，命中：{vReasons}"
        if vExamples:
            sLine += f"；标准用例：{vExamples}"
        vLines.append(sLine)
    return vLines


def StatePath(mEvent: dict, bWithAgent: bool = True) -> Path:
    sKey = str(mEvent.get("session_id") or "nosession")
    sAgent = mEvent.get("agent_id")
    if bWithAgent and sAgent:
        sKey += "-" + str(sAgent)
    sKey = re.sub(r"[^A-Za-z0-9_.\-]", "_", sKey)
    return Path(tempfile.gettempdir()) / "oi-agent-route" / sKey


def LoadReadSet(mEvent: dict, bWithAgent: bool = True) -> set[str]:
    # 每个已读文件一个标记文件，内容是相对路径；并行 Read 各写各的，不会互相覆盖
    stPath = StatePath(mEvent, bWithAgent)
    if not stPath.is_dir():
        return set()
    setRead = set()
    for stMarker in stPath.iterdir():
        if stMarker.suffix == ".tmp":
            continue
        try:
            setRead.add(stMarker.read_text(encoding="utf-8"))
        except OSError:
            continue
    return setRead


def MarkRead(mEvent: dict, sRelPath: str) -> None:
    stPath = StatePath(mEvent)
    stPath.mkdir(parents=True, exist_ok=True)
    sName = hashlib.sha1(sRelPath.encode("utf-8")).hexdigest()
    stMarker = stPath / sName
    if not stMarker.exists():
        stTemp = stPath / f"{sName}.{os.getpid()}.tmp"
        stTemp.write_text(sRelPath, encoding="utf-8")
        os.replace(stTemp, stMarker)


def EditedContent(sToolName: str, mInput: dict) -> str:
    if sToolName == "Write":
        return str(mInput.get("content", ""))
    if sToolName == "MultiEdit":
        return "\n".join(str(mEdit.get("new_string", "")) for mEdit in mInput.get("edits", []))
    return str(mInput.get("new_string", ""))


def Emit(mOutput: dict) -> None:
    sys.stdout.write(json.dumps(mOutput))


def OnUserPrompt(vRoutes: list[Route], mEvent: dict) -> None:
    mHits = MatchText(vRoutes, str(mEvent.get("prompt", "")))
    if not mHits:
        return
    sText = "规范路由：本条消息命中下列规范，动手前先用 Read 读对应文档，再按标准用例做：\n" + "\n".join(FormatHits(mHits))
    Emit({"hookSpecificOutput": {"hookEventName": "UserPromptSubmit", "additionalContext": sText}})


def OnAgent(vRoutes: list[Route], mEvent: dict) -> None:
    mInput = dict(mEvent.get("tool_input") or {})
    sPrompt = str(mInput.get("prompt", ""))
    if not sPrompt or "【规范路由】" in sPrompt:
        return
    mHits = MatchText(vRoutes, sPrompt + "\n" + str(mInput.get("description", "")))
    if not mHits:
        return
    mInput["prompt"] = (
        sPrompt
        + "\n\n【规范路由】本任务命中下列规范。开始前先用 Read 读一遍；改到命中路径或写入对应代码前没读过，编辑会被 hook 拦下：\n"
        + "\n".join(FormatHits(mHits))
    )
    Emit(
        {
            "hookSpecificOutput": {
                "hookEventName": "PreToolUse",
                "permissionDecision": "allow",
                "updatedInput": mInput,
            }
        }
    )


def OnRead(mEvent: dict) -> None:
    sRelPath = RelativePath(str((mEvent.get("tool_input") or {}).get("file_path", "")))
    if not sRelPath:
        return
    setRead = LoadReadSet(mEvent)
    if sRelPath not in setRead:
        MarkRead(mEvent, sRelPath)


def OnEdit(vRoutes: list[Route], mEvent: dict) -> None:
    sToolName = str(mEvent.get("tool_name", ""))
    mInput = mEvent.get("tool_input") or {}
    sRelPath = RelativePath(str(mInput.get("file_path", "")))
    if not sRelPath:
        return
    bCpp = Path(sRelPath).suffix.lower() in CPP_SUFFIXES
    sContent = EditedContent(sToolName, mInput) if bCpp else ""
    mNeed: dict[str, list[str]] = {}
    for stRoute in vRoutes:
        if stRoute.sDoc == sRelPath:
            continue
        sGlob = MatchPath(stRoute, sRelPath)
        if sGlob:
            mNeed.setdefault(stRoute.DocLabel(), []).append(f"路径 {sGlob}")
            continue
        for sPattern, stRegex in zip(stRoute.vContent, stRoute.vContentRegex):
            if sContent and stRegex.search(sContent):
                mNeed.setdefault(stRoute.DocLabel(), []).append(f"内容 /{sPattern}/")
                break
    if not mNeed:
        return
    # subagent 的 Read 事件万一没带 agent_id，会记在会话名下；两边合起来查，避免读过仍被反复拦
    setRead = LoadReadSet(mEvent) | LoadReadSet(mEvent, False)
    vMissing = []
    for sLabel, vReasons in mNeed.items():
        sDoc = sLabel.split("（", 1)[0]
        if sDoc not in setRead:
            vMissing.append(f"- {sLabel}，命中：{'、'.join(dict.fromkeys(vReasons))}")
    if not vMissing:
        return
    sReason = f"规范路由：改 {sRelPath} 前，本会话还没读过下列规范。先用 Read 读完，再重试这次编辑：\n" + "\n".join(vMissing)
    Emit(
        {
            "hookSpecificOutput": {
                "hookEventName": "PreToolUse",
                "permissionDecision": "deny",
                "permissionDecisionReason": sReason,
            }
        }
    )


def RunHook() -> int:
    sys.stderr.reconfigure(encoding="utf-8")
    if os.environ.get("OI_ROUTE_OFF") == "1":
        return 0
    try:
        mEvent = json.loads(sys.stdin.buffer.read().decode("utf-8") or "{}")
        sEventName = str(mEvent.get("hook_event_name", ""))
        sToolName = str(mEvent.get("tool_name", ""))
        if sEventName == "PostToolUse" and sToolName == "Read":
            OnRead(mEvent)
            return 0
        vRoutes = LoadRoutes()
        if sEventName == "UserPromptSubmit":
            OnUserPrompt(vRoutes, mEvent)
        elif sEventName == "PreToolUse" and sToolName in AGENT_TOOLS:
            OnAgent(vRoutes, mEvent)
        elif sEventName == "PreToolUse" and sToolName in EDIT_TOOLS:
            OnEdit(vRoutes, mEvent)
    except Exception as stError:  # hook 失败不能挡住正常工作
        sys.stderr.write(f"agent_route hook 出错，已放行：{stError!r}\n")
    return 0


def RenderTable(vRoutes: list[Route]) -> str:
    vLines = [TABLE_BEGIN, "", "| 关键词 | 文档 | 标准用例 |", "| --- | --- | --- |"]
    for stRoute in vRoutes:
        sDoc = f"`{stRoute.sDoc}`"
        if stRoute.sSection:
            sSpace = " " if stRoute.sSection[0].isascii() else ""
            sDoc += f" 的{sSpace}{stRoute.sSection}"
        vLines.append(f"| {'、'.join(stRoute.vKeywords)} | {sDoc} | {stRoute.sExample} |")
    vLines += ["", TABLE_END]
    return "\n".join(vLines)


def SplitAgents(sText: str) -> tuple[str, str] | None:
    iBegin = sText.find(TABLE_BEGIN)
    iEnd = sText.find(TABLE_END)
    if iBegin < 0 or iEnd < iBegin:
        return None
    return sText[:iBegin], sText[iEnd + len(TABLE_END) :]


def Validate(vRoutes: list[Route]) -> list[str]:
    vErrors = []
    setIds = set()
    for stRoute in vRoutes:
        if stRoute.sId in setIds:
            vErrors.append(f"路由 id 重复：{stRoute.sId}")
        setIds.add(stRoute.sId)
        if not (REPO_ROOT / stRoute.sDoc).is_file():
            vErrors.append(f"{stRoute.sId}：文档不存在 {stRoute.sDoc}")
        if not stRoute.vKeywords and not stRoute.vPaths and not stRoute.vContent:
            vErrors.append(f"{stRoute.sId}：keywords、paths、content 全空，永远不会命中")
    return vErrors


def Main(vArgs: list[str]) -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    if vArgs == ["hook"]:
        return RunHook()
    if vArgs not in (["--check"], ["--write"]):
        sys.stderr.write(__doc__ or "")
        return 2
    try:
        vRoutes = LoadRoutes()
    except TomlUnavailable as stError:
        # 索引检查与平台无关，以装了 3.11+ 的那一侧（Windows 矩阵）为准
        print(f"跳过规范路由检查：{stError}")
        return 0
    except (OSError, ValueError, KeyError, re.error) as stError:
        print(f"索引解析失败：{stError}")
        return 1
    vErrors = Validate(vRoutes)
    sAgents = AGENTS_PATH.read_text(encoding="utf-8")
    stParts = SplitAgents(sAgents)
    if stParts is None:
        vErrors.append("AGENTS.md 缺少 route-index 起止标记")
    else:
        sExpected = stParts[0] + RenderTable(vRoutes) + stParts[1]
        if sExpected != sAgents:
            if vArgs == ["--write"]:
                AGENTS_PATH.write_text(sExpected, encoding="utf-8", newline="\n")
                print("已重新生成 AGENTS.md 关键词表")
            else:
                vErrors.append("AGENTS.md 关键词表与 .agents/rules/index.toml 不一致，运行 python tools/agent_route.py --write")
    for sError in vErrors:
        print(sError)
    return 1 if vErrors else 0


if __name__ == "__main__":
    sys.exit(Main(sys.argv[1:]))
