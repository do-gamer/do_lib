#!/usr/bin/env python3
"""Serves the test page (any path, e.g. /indexInternal.es?action=...) and /test.swf."""
import http.server
import os
import sys

ROOT = sys.argv[1]
PAGE = b"""<!doctype html><html><body style="margin:0;background:#202030">
<embed src="/test.swf" type="application/x-shockwave-flash" width="800" height="600">
</body></html>"""


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith('/test.swf'):
            with open(os.path.join(ROOT, 'test.swf'), 'rb') as f:
                body, ctype = f.read(), 'application/x-shockwave-flash'
        else:
            body, ctype = PAGE, 'text/html'
        self.send_response(200)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


http.server.ThreadingHTTPServer(('127.0.0.1', int(sys.argv[2])), Handler).serve_forever()
