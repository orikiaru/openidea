import socket
import unittest
from unittest.mock import patch

import run_regressions


class PreflightPortsTest(unittest.TestCase):
    def test_free_port(self):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        with patch.object(run_regressions, "LOGD_PORT", port):
            run_regressions.preflight_ports("hold-timer")

    def test_listening_port_is_rejected(self):
        for reuse in (False, True):
            with self.subTest(reuse=reuse), socket.socket() as listener:
                if reuse:
                    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                listener.bind(("127.0.0.1", 0))
                listener.listen()
                port = listener.getsockname()[1]
                with patch.object(run_regressions, "LOGD_PORT", port):
                    with self.assertRaises(OSError):
                        run_regressions.preflight_ports("hold-timer")


if __name__ == "__main__":
    unittest.main()
