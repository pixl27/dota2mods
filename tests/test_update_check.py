"""Exercise the real asynchronous updater against a loopback release server.

No installer is downloaded or run; no GitHub or game connection is used.
"""
import http.server
import json
import os
from pathlib import Path
import subprocess
import threading


class Handler(http.server.BaseHTTPRequestHandler):
    payload = b'{}'
    status = 200

    def do_GET(self):
        self.send_response(self.status)
        self.send_header('Content-Length', str(len(self.payload)))
        self.end_headers()
        self.wfile.write(self.payload)

    def log_message(self, *args):
        pass


def main():
    server = http.server.HTTPServer(('127.0.0.1', 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    endpoint = 'http://127.0.0.1:%s/' % server.server_port
    env = dict(os.environ, WARDROBE_TEST_RELEASES=endpoint)
    exe = Path(__file__).resolve().parents[1] / 'build/update_check_test.exe'
    # State::Current=2, Available=3, Failed=6.
    release = {'tag_name': 'v2099.01.01.1', 'assets': []}
    cases = [(200, b'{}', 6), (200, b'{broken', 6),
             (200, json.dumps(release).encode(), 6), (404, b'{}', 6),
             (200, b'{"tag_name":"v2026.10.07.2","assets":[]}', 2)]
    release['assets'] = [{'name': 'Wardrobe-Setup-2099.01.01.1.exe',
                          'browser_download_url': endpoint + 'Wardrobe-Setup-2099.01.01.1.exe'}]
    cases.append((200, json.dumps(release).encode(), 3))
    try:
        for status, payload, expected in cases:
            Handler.status, Handler.payload = status, payload
            subprocess.run([str(exe), str(expected)], env=env, check=True, timeout=15)
    finally:
        server.shutdown()
        worker.join()
        server.server_close()
    print('PASS: %d asynchronous update checks' % len(cases))


if __name__ == '__main__':
    main()
