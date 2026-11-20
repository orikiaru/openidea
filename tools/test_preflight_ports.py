import socket
import unittest
from unittest.mock import patch

from regression import ports


class PreflightPortsTest(unittest.TestCase):
    def test_tcp_occupied_and_released(self):
        for address in ("127.0.0.1", "0.0.0.0"):
            for reuse in (False, True):
                with self.subTest(address=address, reuse=reuse):
                    with socket.socket() as listener:
                        if reuse:
                            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                        listener.bind((address, 0))
                        listener.listen()
                        port = listener.getsockname()[1]
                        with patch.object(ports, "LOGD_PORT", port):
                            with self.assertRaises(OSError):
                                ports.preflight_ports("hold-timer")
                    with patch.object(ports, "LOGD_PORT", port):
                        ports.preflight_ports("hold-timer")

    def test_udp_occupied_and_released(self):
        for address in ("127.0.0.1", "0.0.0.0"):
            for reuse in (False, True):
                with self.subTest(address=address, reuse=reuse):
                    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
                        if reuse:
                            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                        listener.bind((address, 0))
                        port = listener.getsockname()[1]
                        with patch.object(ports, "UDP_PORT", port), \
                             patch.object(ports, "LOGD_PORT", 0), \
                             patch.object(ports, "IPC_PORT", 0):
                            with self.assertRaises(OSError):
                                ports.preflight_ports("hold-blocking")
                    with patch.object(ports, "UDP_PORT", port), \
                         patch.object(ports, "LOGD_PORT", 0), \
                         patch.object(ports, "IPC_PORT", 0):
                        ports.preflight_ports("hold-blocking")


if __name__ == "__main__":
    unittest.main()
