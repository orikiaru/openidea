"""agent_route.py 自测：hook 子进程收发、状态隔离和索引校验，不依赖真实会话。"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import uuid

import agent_route

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'tools' / 'agent_route.py'
IPCSERVER = str(ROOT / 'modules' / 'ipcserver' / 'src' / 'ipcserver.cpp')
COROUTINE_DOC = str(ROOT / 'docs' / 'coroutine.md')
TESTING_DOC = str(ROOT / 'docs' / 'testing.md')


class HookTestBase(unittest.TestCase):
    def setUp(self):
        self.session = 'oi-route-test-' + uuid.uuid4().hex
        self.agents = set()

    def tearDown(self):
        # 只删本用例的状态文件，目录里可能有真实会话在用
        vPaths = [agent_route.StatePath({'session_id': self.session})]
        for sAgent in self.agents:
            vPaths.append(agent_route.StatePath({'session_id': self.session, 'agent_id': sAgent}))
        for stPath in vPaths:
            shutil.rmtree(stPath, ignore_errors=True)

    def hook(self, payload, env=None):
        mEnv = dict(os.environ)
        mEnv.pop('OI_ROUTE_OFF', None)
        mEnv.update(env or {})
        data = payload if isinstance(payload, bytes) else json.dumps(payload).encode('utf-8')
        result = subprocess.run([sys.executable, str(SCRIPT), 'hook'], input=data, capture_output=True, env=mEnv,
                                timeout=60)
        return result.returncode, result.stdout.decode('utf-8'), result.stderr.decode('utf-8', 'replace')

    def event(self, name, tool=None, tool_input=None, agent=None, **extra):
        mEvent = {'session_id': self.session, 'hook_event_name': name}
        if tool is not None:
            mEvent['tool_name'] = tool
        if tool_input is not None:
            mEvent['tool_input'] = tool_input
        if agent is not None:
            self.agents.add(agent)
            mEvent['agent_id'] = agent
        mEvent.update(extra)
        return mEvent

    def prompt(self, text):
        return self.hook(self.event('UserPromptSubmit', prompt=text))

    def read(self, path, agent=None):
        return self.hook(self.event('PostToolUse', 'Read', {'file_path': path}, agent))

    def edit(self, path, new_string='x', agent=None, tool='Edit'):
        mInput = {'file_path': path, 'old_string': 'old', 'new_string': new_string}
        if tool == 'Write':
            mInput = {'file_path': path, 'content': new_string}
        return self.hook(self.event('PreToolUse', tool, mInput, agent))

    def assert_silent(self, outcome):
        code, out, err = outcome
        self.assertEqual(0, code, err)
        self.assertEqual('', out, err)

    def assert_context(self, outcome):
        code, out, err = outcome
        self.assertEqual(0, code, err)
        self.assertNotEqual('', out, err)
        mOut = json.loads(out)['hookSpecificOutput']
        self.assertEqual('UserPromptSubmit', mOut['hookEventName'])
        return mOut['additionalContext']

    def assert_deny(self, outcome):
        code, out, err = outcome
        self.assertEqual(0, code, err)
        self.assertNotEqual('', out, 'expected deny, got allow; stderr=' + err)
        mOut = json.loads(out)['hookSpecificOutput']
        self.assertEqual('PreToolUse', mOut['hookEventName'])
        self.assertEqual('deny', mOut['permissionDecision'])
        return mOut['permissionDecisionReason']


class UserPromptTests(HookTestBase):
    def test_keywords_inject_docs(self):
        sText = self.assert_context(self.prompt('这里要 co_await 一下，改完跑回归'))
        self.assertIn('docs/coroutine.md', sText)
        self.assertIn('docs/testing.md', sText)
        self.assertNotIn('docs/module.md', sText)

    def test_plain_prompt_silent(self):
        self.assert_silent(self.prompt('继续'))

    def test_ascii_whole_word(self):
        self.assertIn('docs/coroutine.md', self.assert_context(self.prompt('TaskScope 怎么用')))
        self.assertIn('docs/coroutine.md', self.assert_context(self.prompt('改一下task')))
        self.assert_silent(self.prompt('Tasks'))
        self.assert_silent(self.prompt('MyTask'))

    def test_path_in_prompt(self):
        sText = self.assert_context(self.prompt('看看 tools/regression/cases/echo.py'))
        self.assertIn('docs/testing.md', sText)


class EditGateTests(HookTestBase):
    def test_content_rule_until_doc_read(self):
        sReason = self.assert_deny(self.edit(IPCSERVER, 'co_await Foo();'))
        self.assertIn('docs/coroutine.md', sReason)
        self.assert_silent(self.read(COROUTINE_DOC))
        self.assert_silent(self.edit(IPCSERVER, 'co_await Foo();'))

    def test_reading_other_doc_does_not_unlock(self):
        self.assert_silent(self.read(TESTING_DOC))
        self.assert_deny(self.edit(IPCSERVER, 'co_await Foo();'))

    def test_write_and_multiedit_content(self):
        self.assertIn('docs/coroutine.md', self.assert_deny(self.edit(IPCSERVER, 'co_return 1;', tool='Write')))
        mInput = {'file_path': IPCSERVER, 'edits': [{'old_string': 'a', 'new_string': 'int a;'},
                                                    {'old_string': 'b', 'new_string': 'StartTask(x);'}]}
        self.assertIn('docs/coroutine.md', self.assert_deny(self.hook(self.event('PreToolUse', 'MultiEdit', mInput))))

    def test_unrelated_cpp_content_allowed(self):
        self.assert_silent(self.edit(IPCSERVER, 'int iCount = 0;'))

    def test_path_rules(self):
        sEcho = str(ROOT / 'tools' / 'regression' / 'cases' / 'echo.py')
        self.assertIn('docs/testing.md', self.assert_deny(self.edit(sEcho, 'x = 1')))
        self.assertIn('docs/testing.md', self.assert_deny(self.edit('tools/regression/cases/echo.py', 'x = 1')))
        for sProto in ('modules/protoconfig/rpc.proto', 'a/b/c.proto', 'top.proto'):
            with self.subTest(path=sProto):
                self.assertIn('docs/module.md', self.assert_deny(self.edit(str(ROOT / sProto), 'message A {}')))
        self.assert_silent(self.edit(str(ROOT / 'modules' / 'protoconfig' / 'rpc.proto.bak'), 'x'))
        self.assert_silent(self.read(TESTING_DOC))
        self.assert_silent(self.edit(sEcho, 'x = 1'))

    def test_content_rules_only_for_cpp(self):
        self.assert_silent(self.edit(str(ROOT / 'docs' / 'develop.md'), 'co_await Foo();'))
        self.assert_silent(self.edit(str(ROOT / 'tools' / 'foo.py'), 'co_await Foo()  # [&]'))
        self.assert_deny(self.edit(str(ROOT / 'cbaselib' / 'base' / 'inc' / 'x.H'), 'co_await Foo();'))

    def test_default_capture(self):
        for sCode in ('auto fn = [&]() {};', 'auto fn = [=, iCount]() {};', 'auto fn = [ & , x]() {};'):
            with self.subTest(code=sCode):
                self.assertIn('docs/coroutine.md', self.assert_deny(self.edit(IPCSERVER, sCode)))
        for sCode in ('int iValue = a[&b];', 'auto fn = [&iCount]() {};', 'auto fn = [this]() {};'):
            with self.subTest(code=sCode):
                self.assert_silent(self.edit(IPCSERVER, sCode))


class SubagentTests(HookTestBase):
    def test_agent_read_is_scoped(self):
        self.assert_silent(self.read(COROUTINE_DOC, agent='agent-a'))
        self.assertTrue(agent_route.StatePath({'session_id': self.session, 'agent_id': 'agent-a'}).exists())
        self.assertFalse(agent_route.StatePath({'session_id': self.session}).exists())
        self.assert_silent(self.edit(IPCSERVER, 'co_await Foo();', agent='agent-a'))
        self.assert_deny(self.edit(IPCSERVER, 'co_await Foo();', agent='agent-b'))
        self.assert_deny(self.edit(IPCSERVER, 'co_await Foo();'))

    def test_session_read_covers_agents(self):
        self.assert_silent(self.read(COROUTINE_DOC))
        self.assert_silent(self.edit(IPCSERVER, 'co_await Foo();', agent='agent-c'))

    def test_sessions_are_isolated(self):
        self.assert_silent(self.read(COROUTINE_DOC))
        sOld = self.session
        self.session = 'oi-route-test-' + uuid.uuid4().hex
        try:
            self.assert_deny(self.edit(IPCSERVER, 'co_await Foo();'))
        finally:
            self.session = sOld

    def test_parallel_reads_all_recorded(self):
        # 模型一次并行 Read 多份文档，各 hook 进程同时落盘，不能互相覆盖丢记录
        vDocs = sorted({stRoute.sDoc for stRoute in agent_route.LoadRoutes()})
        vDocs += ['todo.md', 'AGENTS.md', 'tools/README.md']
        for iRound in range(3):
            with self.subTest(round=iRound):
                shutil.rmtree(agent_route.StatePath({'session_id': self.session}), ignore_errors=True)
                mEnv = dict(os.environ)
                mEnv.pop('OI_ROUTE_OFF', None)
                vProcs = []
                for sDoc in vDocs:
                    data = json.dumps(self.event('PostToolUse', 'Read', {'file_path': str(ROOT / sDoc)})).encode('utf-8')
                    stProc = subprocess.Popen([sys.executable, str(SCRIPT), 'hook'], stdin=subprocess.PIPE,
                                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=mEnv)
                    vProcs.append((stProc, data))
                for stProc, data in vProcs:
                    stProc.communicate(data, timeout=60)
                    self.assertEqual(0, stProc.returncode)
                self.assertEqual(set(vDocs), agent_route.LoadReadSet({'session_id': self.session}))


class AgentToolTests(HookTestBase):
    def agent(self, tool_input, tool='Agent'):
        return self.hook(self.event('PreToolUse', tool, tool_input))

    def test_prompt_appended(self):
        mInput = {'description': '跑一遍', 'prompt': '请帮我跑回归', 'subagent_type': 'general-purpose', 'model': 'sonnet'}
        for sTool in ('Agent', 'Task'):
            with self.subTest(tool=sTool):
                code, out, err = self.agent(mInput, sTool)
                self.assertEqual(0, code, err)
                mOut = json.loads(out)['hookSpecificOutput']
                self.assertEqual('PreToolUse', mOut['hookEventName'])
                self.assertEqual('allow', mOut['permissionDecision'])
                mNew = mOut['updatedInput']
                self.assertTrue(mNew['prompt'].startswith(mInput['prompt'] + '\n'))
                sTail = mNew['prompt'][len(mInput['prompt']):]
                self.assertIn('【规范路由】', sTail)
                self.assertIn('docs/testing.md', sTail)
                self.assertEqual(1, mNew['prompt'].count('【规范路由】'))
                self.assertEqual({key: value for key, value in mInput.items() if key != 'prompt'},
                                 {key: value for key, value in mNew.items() if key != 'prompt'})

    def test_description_keywords_count(self):
        code, out, err = self.agent({'description': '协程改造', 'prompt': '照计划做', 'subagent_type': 'Explore'})
        self.assertEqual(0, code, err)
        self.assertIn('docs/coroutine.md', json.loads(out)['hookSpecificOutput']['updatedInput']['prompt'])

    def test_no_duplicate_or_noise(self):
        self.assert_silent(self.agent({'description': 'x', 'prompt': '跑回归\n\n【规范路由】已经有了', 'subagent_type': 'claude'}))
        self.assert_silent(self.agent({'description': '看看', 'prompt': '继续', 'subagent_type': 'claude'}))
        self.assert_silent(self.agent({'description': '回归', 'prompt': '', 'subagent_type': 'claude'}))


class FailOpenTests(HookTestBase):
    def test_outside_repo(self):
        sOutside = str(Path(tempfile.gettempdir()) / ('oi-route-outside-' + uuid.uuid4().hex) / 'x.cpp')
        self.assert_silent(self.edit(sOutside, 'co_await Foo();'))
        self.assert_silent(self.read(sOutside))
        self.assertFalse(agent_route.StatePath({'session_id': self.session}).exists())

    def test_bad_json(self):
        code, out, err = self.hook(b'{not json')
        self.assertEqual(0, code)
        self.assertEqual('', out)
        self.assertIn('agent_route', err)

    def test_switch_off(self):
        self.assert_deny(self.edit(IPCSERVER, 'co_await Foo();'))
        self.assert_silent(self.hook(self.event('PreToolUse', 'Edit', {'file_path': IPCSERVER, 'new_string': 'co_await Foo();'}),
                                     {'OI_ROUTE_OFF': '1'}))
        self.assert_silent(self.hook(self.event('UserPromptSubmit', prompt='跑回归'), {'OI_ROUTE_OFF': '1'}))


class IndexTests(unittest.TestCase):
    def route(self, **kwargs):
        mFields = {'sId': 'demo', 'sDoc': 'docs/testing.md', 'sSection': '', 'sExample': '',
                   'vKeywords': ['k'], 'vPaths': [], 'vContent': []}
        mFields.update(kwargs)
        return agent_route.Route(**mFields)

    def test_check_passes_on_repo(self):
        result = subprocess.run([sys.executable, str(SCRIPT), '--check'], capture_output=True, timeout=60)
        self.assertEqual(0, result.returncode, result.stdout.decode('utf-8', 'replace'))

    def test_validate(self):
        self.assertEqual([], agent_route.Validate(agent_route.LoadRoutes()))
        self.assertTrue(any('文档不存在' in sError for sError in agent_route.Validate([self.route(sDoc='docs/no_such_doc.md')])))
        self.assertTrue(any('id 重复' in sError for sError in agent_route.Validate([self.route(), self.route()])))
        self.assertTrue(any('全空' in sError for sError in agent_route.Validate([self.route(vKeywords=[])])))

    def test_table_roundtrip(self):
        vRoutes = agent_route.LoadRoutes()
        sAgents = agent_route.AGENTS_PATH.read_text(encoding='utf-8')
        stParts = agent_route.SplitAgents(sAgents)
        self.assertIsNotNone(stParts)
        self.assertEqual(sAgents, stParts[0] + agent_route.RenderTable(vRoutes) + stParts[1])
        vRoutes[0].vKeywords.append('新关键词')
        self.assertNotEqual(sAgents, stParts[0] + agent_route.RenderTable(vRoutes) + stParts[1])
        self.assertIsNone(agent_route.SplitAgents('no markers'))
        self.assertIsNone(agent_route.SplitAgents(agent_route.TABLE_END + agent_route.TABLE_BEGIN))

    def test_render_section(self):
        sTable = agent_route.RenderTable([self.route(sSection='Web 段', vKeywords=['a', 'b'], sExample='e'),
                                          self.route(sSection='第 1 节')])
        self.assertIn('| a、b | `docs/testing.md` 的 Web 段 | e |', sTable)
        self.assertIn('`docs/testing.md` 的第 1 节', sTable)

    def test_glob(self):
        stRegex = agent_route.GlobToRegex('**/*.proto')
        self.assertTrue(stRegex.match('a.proto'))
        self.assertTrue(stRegex.match('a/b/c.proto'))
        self.assertFalse(stRegex.match('a/b/c.proto.bak'))
        stRegex = agent_route.GlobToRegex('modules/ipcclient/src/*_mgr.cpp')
        self.assertTrue(stRegex.match('modules/ipcclient/src/echo_mgr.cpp'))
        self.assertFalse(stRegex.match('modules/ipcclient/src/sub/echo_mgr.cpp'))

    def test_keyword(self):
        self.assertTrue(agent_route.KeywordToRegex('Task').search('一个task'))
        self.assertFalse(agent_route.KeywordToRegex('Task').search('Tasks'))
        self.assertFalse(agent_route.KeywordToRegex('Task').search('StartTask'))
        self.assertTrue(agent_route.KeywordToRegex('.proto').search('改 rpc.proto'))
        self.assertTrue(agent_route.KeywordToRegex('协程').search('改协程'))
        self.assertIsInstance(agent_route.KeywordToRegex('x'), re.Pattern)


if __name__ == '__main__':
    try:
        agent_route.LoadRoutes()
    except agent_route.TomlUnavailable as stError:
        # 与 agent_route.py --check 同口径：索引与平台无关，以 3.11+ 那一侧为准
        print(f'跳过规范路由自测：{stError}')
        sys.exit(0)
    unittest.main()
