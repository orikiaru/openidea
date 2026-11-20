import argparse
import http.client
import socket
import subprocess
import tempfile
import time
from pathlib import Path
from regression_platform import launch, stop
from test_process_owner import write_owner, record_process

ROOT = Path(__file__).resolve().parents[1]
SIZE = 9 * 1024 * 1024
BODY = bytes(range(251)) * (SIZE // 251) + bytes(range(SIZE % 251))


def run(binary, owner_repo, lane, mode, index):
    with tempfile.TemporaryDirectory(prefix='oi-regression-p0-web-stop-') as tmp:
        root = Path(tmp)
        write_owner(root, owner_repo, 0 if lane.endswith('mt') else 1000)
        work = root / 'bin'
        work.mkdir()
        config = root / 'config'
        config.mkdir()
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        text = (ROOT / 'deploy/config/webserver.toml').read_text(encoding='utf-8-sig')
        text = text.replace('port = 18080', 'port = %d' % port)
        text += '\nform_regression = true\nmax_receive_buffer_bytes = 131072\nmax_frame_bytes = 131064\n'
        (config / 'webserver.toml').write_text(text, encoding='utf-8')
        with (root / 'server.out').open('w') as output:
            proc = launch([str(binary)], cwd=work, stdout=output, stderr=subprocess.STDOUT)
            record_process(root, proc)
            conn = None
            try:
                deadline = time.monotonic() + 10
                while True:
                    try:
                        conn = socket.create_connection(('127.0.0.1', port), timeout=0.2)
                        break
                    except OSError:
                        if proc.poll() is not None or time.monotonic() > deadline:
                            raise RuntimeError('server startup failed')
                        time.sleep(0.05)
                conn.settimeout(30)
                if mode == 'idle':
                    conn.sendall(b'GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n')
                    response = http.client.HTTPResponse(conn)
                    response.begin()
                    assert response.status == 200 and response.read() == b'ok'
                    stop(proc)
                    code = proc.wait(timeout=10)
                    try:
                        end = conn.recv(1)
                        termination = 'eof' if end == b'' else repr(end)
                    except ConnectionResetError as error:
                        termination = 'rst winerror=%s' % error.winerror
                    print(f'{lane} {mode} {index}: exit={code} termination={termination}', flush=True)
                    assert code == 0
                    return
                conn.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                conn.sendall(('POST /test-stream HTTP/1.1\r\nHost: localhost\r\nContent-Length: %d\r\nConnection: keep-alive\r\n\r\n' % SIZE).encode() + BODY)
                response = http.client.HTTPResponse(conn)
                response.begin()
                assert response.status == 200 and not response.will_close
                first = response.read(4096)
                assert len(first) == 4096 and first == BODY[:4096]
                time.sleep(0.5)
                if mode == 'stop':
                    stop(proc)
                conn.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 262144)
                data = bytearray(first)
                failure = None
                try:
                    while True:
                        chunk = response.read1(65536)
                        if not chunk:
                            break
                        data.extend(chunk)
                except (OSError, http.client.HTTPException) as error:
                    failure = repr(error)
                if mode == 'control':
                    stop(proc)
                code = proc.wait(timeout=15)
                good = data == BODY[:len(data)]
                print(f'{lane} {mode} {index}: exit={code} first={len(first)} received={len(data)} expected={SIZE} prefix_ok={good} complete={len(data)==SIZE} read_error={failure}', flush=True)
                assert code == 0 and failure is None and good and len(data) == SIZE
            finally:
                if conn:
                    conn.close()
                if proc.poll() is None:
                    stop(proc)
                    try:
                        proc.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait(timeout=5)
                        raise RuntimeError('server required force kill')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--lane', choices=['windows-mt', 'windows-st'], required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--owner-repo', type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    failures = []
    for mode in ('control', 'idle', 'stop'):
        for index in range(2):
            try:
                run(binary, args.owner_repo.resolve(), args.lane, mode, index)
            except Exception as error:
                print(f'{args.lane} {mode} {index}: FAILED {error!r}', flush=True)
                failures.append((mode, index, repr(error)))
    if failures:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
