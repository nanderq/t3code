"""Exercise the actual C WebSocket handshake against a local TCP fixture."""
import pathlib
import socket
import subprocess
import tempfile
import threading
import unittest


class UpgradeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        root = pathlib.Path(__file__).resolve().parents[1]
        probe = pathlib.Path(cls.temp.name) / 'probe.c'
        probe.write_text('''#include "ws.h"
#include <stdio.h>
int main(int argc, char **argv) {
  NetAddr addr; Ws ws;
  if (!net_parse_addr(argv[1], &addr)) return 2;
  int ok = ws_connect(&ws, &addr, "/fixture", NULL);
  printf("result=%d status=%d error=%s\\n", ok, ws.status, ws.err);
  ws_close(&ws);
  return 0;
}
''')
        cls.binary = str(pathlib.Path(cls.temp.name) / 'probe')
        subprocess.run(['cc', '-I', str(root / 'source'), str(probe),
                        *[str(root / 'source' / f) for f in ['ws.c', 'net.c', 'util.c']],
                        '-o', cls.binary, '-lz'], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def response(self, payload):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen(1)
            listener.settimeout(5)
            def serve():
                with listener.accept()[0] as conn:
                    conn.settimeout(5)
                    request = b''
                    while b'\r\n\r\n' not in request:
                        request += conn.recv(4096)
                    conn.sendall(payload)
            worker = threading.Thread(target=serve)
            worker.start()
            try:
                result = subprocess.run([self.binary, f'127.0.0.1:{listener.getsockname()[1]}'],
                                        capture_output=True, text=True, timeout=15, check=True)
            finally:
                worker.join(timeout=6)
            return result.stdout

    def test_closed_before_response(self):
        self.assertIn('Server closed before completing upgrade (0 bytes)', self.response(b''))

    def test_incomplete_response(self):
        self.assertIn('Server closed before completing upgrade (12 bytes)', self.response(b'HTTP/1.1 101'))

    def test_rejected(self):
        self.assertIn('Upgrade rejected (HTTP 403)', self.response(b'HTTP/1.1 403 Forbidden\r\n\r\n'))

    def test_upgraded(self):
        self.assertIn('result=1 status=101', self.response(b'HTTP/1.1 101 Switching Protocols\r\n\r\n'))


if __name__ == '__main__':
    unittest.main()
