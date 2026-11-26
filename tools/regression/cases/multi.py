"""multi 组：多实例路由、静态注册和本地名单。"""
import os
import re
import shutil
import subprocess

from regression_platform import SUFFIX

from .. import ports
from ..config import set_test_flags
from ..probes import EnvironmentBlocked
from . import Case


class MultiCase(Case):
    def configure(self, ctx, name, text):
        case = ctx.name
        if name == "registry" and case == "registry-static":
            required = ("OI_TEST_MYSQL_HOST", "OI_TEST_MYSQL_PORT", "OI_TEST_MYSQL_USER", "OI_TEST_MYSQL_DATABASE")
            missing = [item for item in required if not os.environ.get(item)]
            if missing:
                raise EnvironmentBlocked("registry static missing " + ",".join(missing))
            text = text.replace(
                'config = "dynamic"',
                'config = "static"\ndb_host = "%s"\ndb_port = %s\ndb_user = "%s"\ndb_password = "%s"\ndb_database = "%s"' % (
                    os.environ["OI_TEST_MYSQL_HOST"], os.environ["OI_TEST_MYSQL_PORT"], os.environ["OI_TEST_MYSQL_USER"],
                    os.environ.get("OI_TEST_MYSQL_PASSWORD", ""), os.environ["OI_TEST_MYSQL_DATABASE"]),
                1)
        if name == "ipcclient":
            if case in ("multi", "local"):
                text = text.replace("max_send_queue_bytes = 65536", "", 1)
                if case == "multi":
                    text = text.replace("[RpcRoute]", "[RpcRoute]\nmax_receive_buffer_bytes = 1048584\nmax_frame_bytes = 1048576\nincomplete_frame_timeout_ms = 1234\nidle_timeout_ms = 5678", 1)
                text += "\n[Regression]\nroute_limits = true\nroute_port = \"%d\"\n" % ports.GAME_PORTS[0]
            text = set_test_flags(text, ctx.mode)
            if case == "local":
                # 只去掉注册地址这三行。点号不能跨行，否则会把后面的 LogClient 一起删掉。
                text = re.sub(
                    r'(?m)^\[Registry\]\nip = "[^"]*"\nport = "[^"]*"\ntype = "[^"]*"\n',
                    '[Registry]\ninstances = [\n    "peerserver 1 127.0.0.1 %d",\n    "peerserver 2 127.0.0.1 %d",\n]\n' % ports.GAME_PORTS,
                    text,
                    count=1)
        return text

    def after_logd(self, ctx):
        if ctx.name != "registry-static":
            return
        mysql = os.environ.get("OI_TEST_MYSQL_CLIENT") or shutil.which("mysql")
        if not mysql:
            raise EnvironmentBlocked("registry static needs OI_TEST_MYSQL_CLIENT or mysql on PATH")
        sql = (
            "CREATE TABLE IF NOT EXISTS oi_service_instance ("
            "`name` VARCHAR(64) NOT NULL, `id` INT NOT NULL, `ip` VARCHAR(64) NOT NULL, "
            "`port` VARCHAR(8) NOT NULL, `zone` VARCHAR(32) NOT NULL, PRIMARY KEY (`name`, `id`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4; "
            "DELETE FROM oi_service_instance WHERE `name`='peerserver'; "
            "INSERT INTO oi_service_instance (`name`, `id`, `ip`, `port`, `zone`) VALUES "
            "('peerserver', 1, '127.0.0.1', '%d', ''), ('peerserver', 2, '127.0.0.1', '%d', '');"
        ) % ports.GAME_PORTS
        command = [mysql, "--no-defaults", "--protocol=tcp", "--host", os.environ["OI_TEST_MYSQL_HOST"],
                   "--port", os.environ["OI_TEST_MYSQL_PORT"], "--user", os.environ["OI_TEST_MYSQL_USER"],
                   "--database", os.environ["OI_TEST_MYSQL_DATABASE"], "--execute", sql]
        if os.environ.get("OI_TEST_MYSQL_PASSWORD"):
            command.insert(-2, "--password=" + os.environ["OI_TEST_MYSQL_PASSWORD"])
        subprocess.run(command, check=True, timeout=30)
        ctx.registry, ctx.registry_path = ctx.start("registry")
        ctx.ready(ctx.registry, ctx.registry_path, "registry server listen")

    def scenario(self, ctx):
        if ctx.mode not in ("multi", "pool"):
            return
        # 同名两台全局服，实例号 1、2。multi 在客户端起来前登记；local 用配置名单，registry-static 用表里的名单。
        for instance, port in zip((1, 2), ports.GAME_PORTS):
            text = (ctx.cfg / "ipcserver.toml").read_text(encoding="utf-8")
            text = text.replace('name = "ipcserver"', 'name = "peerserver"', 1)
            text = re.sub(r"(?m)^id = 1$", "id = %d" % instance, text, count=1)
            text = text.replace('port = "%d"' % ports.IPC_PORT, 'port = "%d"' % port, 1)
            text = text.replace('port = "%d"' % ports.UDP_PORT, 'port = "0"', 1)
            path = ctx.cfg / ("peerserver-%d.toml" % instance)
            if ctx.name == "local":
                text = re.sub(
                    r'(?m)^\[Registry\]\nip = "[^"]*"\nport = "[^"]*"\ntype = "[^"]*"\n',
                    "[Registry]\n",
                    text,
                    count=1)
            path.write_text(text, encoding="utf-8")
            proc, output = ctx.start("peerserver-%d" % instance, [str(ctx.work / ("ipcserver" + SUFFIX)), str(path)])
            ctx.ready(proc, output, "ipc server listen")
            ctx.peers[instance] = proc

    def on_poll(self, ctx):
        if ctx.mode == "multi" and not ctx.peer_restarted:
            client_text = ctx.clientpath.read_text(encoding="utf-8", errors="replace")
            if not ctx.peer_stopped and "multi request stop id=2" in client_text:
                # 直接杀掉，连接和注册都靠对端发现断开。
                ctx.peers[2].kill()
                ctx.peers[2].wait(timeout=5)
                ctx.procs.remove(ctx.peers[2])
                ctx.peer_stopped = True
            if ctx.peer_stopped and "multi request start id=2" in client_text:
                ctx.peers[2], output = ctx.start("peerserver-2-again", [str(ctx.work / ("ipcserver" + SUFFIX)), str(ctx.cfg / "peerserver-2.toml")])
                ctx.ready(ctx.peers[2], output, "ipc server listen")
                ctx.peer_restarted = True

    def check(self, ctx):
        case, text = ctx.name, ctx.text
        peer_stopped, peer_restarted = ctx.peer_stopped, ctx.peer_restarted
        if ctx.mode == "pool":
            if case == "local":
                assert "route limits configured=0 bad=0" in text
            assert re.search(r"pool call any one=[1-9]\d* two=[1-9]\d* failed=0 bad=0", text)
            assert "pool call all slots=2 bad=0" in text
            assert "pool done bad=0" in text
        elif ctx.mode == "multi":
            assert "route limits configured=1 bad=0" in text
            assert "multi watcher unchanged recovered=1 bad=0" in text
            assert "multi notify early accepted=0 delivered=1 bad=0" in text
            assert "multi notify reconnect disconnected=1 accepted=0 delivered=1 drained=1 bad=0" in text
            assert "multi fixed early=1 connected=1 ids=0 missing=-3 bad=0" in text
            assert "multi registry seed=1,2 connected=1 bad=0" in text
            assert "multi call by instance one=1 two=2 none=-3 bad=0" in text
            assert re.search(r"multi call any one=[1-9]\d* two=[1-9]\d* failed=0 bad=0", text)
            assert "multi call all slots=2 bad=0" in text
            assert "multi fixed swap reject=1 two=2 back=1 same=1 connected=1 bad=0" in text
            assert "multi reverse notify call=1 notify=0 none=-3 any=0 received=3 wrong=0 bad=0" in text
            assert "multi notify batch all_started=2 all_got=2 many_started=2 many_got=2 reconnect=1 bad=0" in text
            assert "multi peer down disconnect=1 gone=1 one=1 two=-3 bad=0" in text
            assert re.search(r"multi peer back listed=1 two=2 connects=[2-9]\d* bad=0", text)
            assert "multi done bad=0" in text
            assert peer_stopped and peer_restarted
        return False

CASES = (
    MultiCase("multi", "multi"),
    MultiCase("local", "pool"),
    MultiCase("registry-static", "pool", targeted_reason="uses the same database fixture as mysql"),
)
