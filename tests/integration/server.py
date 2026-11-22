#!/usr/bin/env python3
"""Serves the test page (any path, e.g. /indexInternal.es?action=...) and /test.swf."""
import http.server
import os
import sys

ROOT = sys.argv[1]
PAGE = b"""<!doctype html><html><body style="margin:0;background:#202030">
<embed src="/test.swf" type="application/x-shockwave-flash" width="800" height="600">
</body></html>"""


# Input test page (CLICK_TEST=1): a text field and a button at fixed positions; every mouse
# click and the field's value are reported back to /event and appended to events.log.
CLICK_PAGE = b"""<!doctype html><html><body style="margin:0;background:#202030">
<input id="name" style="position:absolute;left:100px;top:100px;width:200px;height:30px">
<button id="invite" style="position:absolute;left:400px;top:100px;width:100px;height:30px">Invite</button>
<embed src="/test.swf" type="application/x-shockwave-flash" width="200" height="150" style="position:absolute;left:100px;top:300px">
<script>
function report(what) { fetch('/event?' + encodeURIComponent(what)); }
document.addEventListener('mousedown', e => report('mousedown ' + (e.target.id || e.target.tagName) + ' ' + e.clientX + ',' + e.clientY));
document.getElementById('invite').addEventListener('click', () => report('invite value=' + document.getElementById('name').value));
document.getElementById('name').addEventListener('focus', () => report('focus name'));
document.getElementById('name').addEventListener('input', () => report('input ' + document.getElementById('name').value));
window.addEventListener('load', () => report('loaded ' + window.innerWidth + 'x' + window.innerHeight));
</script></body></html>"""


# Mouse test page (MOUSE_TEST=1): a plain area and the player (test.swf built with
# --report-input) at fixed positions; every mouse event the page and the player get is
# reported with its position in CSS px (the player: stage units, origin at the embed).
MOUSE_PAGE = b"""<!doctype html><html><body style="margin:0;background:#202030">
<div id="pad" style="position:absolute;left:20px;top:20px;width:300px;height:200px;background:#304050"></div>
<embed id="swf" src="/test.swf" type="application/x-shockwave-flash" width="400" height="300" style="position:absolute;left:360px;top:20px">
<script>
function report(what) { fetch('/event?' + encodeURIComponent(what)); }
function where(e) { return (e.target.id || e.target.tagName) + ' ' + e.clientX + ',' + e.clientY; }
['mousemove', 'mousedown', 'mouseup', 'click'].forEach(t => document.addEventListener(t, e => report(t + ' ' + where(e)), true));
document.addEventListener('wheel', e => report('wheel ' + where(e) + ' ' + Math.sign(e.deltaY)), true);
window.addEventListener('load', () => report('loaded ' + window.innerWidth + 'x' + window.innerHeight + ' dpr=' + window.devicePixelRatio));
</script></body></html>"""


# Display test page (DISPLAY_TEST=1, tests/display/run.sh): the player (test.swf built with
# --stage3d) renders through Stage3D in wmode=direct like the game and calls tick() once per
# frame of its main thread. Once a second the page reports how many frames ran and the longest
# gap between two of them (a stall still going on counts up to now).
DISPLAY_PAGE = b"""<!doctype html><html><body style="margin:0;background:#202030">
<embed id="swf" src="/test.swf" type="application/x-shockwave-flash" width="320" height="240"
       wmode="direct" allowscriptaccess="always">
<script>
function report(what) { fetch('/event?' + encodeURIComponent(what)); }
let frames = 0, last = 0, maxGap = 0, rafs = 0, lastReport = performance.now();
function tick(flashTime) {
    const now = performance.now();
    if (last) maxGap = Math.max(maxGap, now - last);
    last = now;
    frames++;
}
(function raf() { rafs++; requestAnimationFrame(raf); })();
setInterval(() => {
    const now = performance.now();
    const gap = Math.round(Math.max(maxGap, last ? now - last : 0));
    report('second at=' + Date.now() + ' frames=' + frames + ' max_gap_ms=' + gap + ' raf=' + rafs
           + ' interval_ms=' + Math.round(now - lastReport));
    frames = 0; maxGap = 0; rafs = 0; lastReport = now;
}, 1000);
window.addEventListener('load', () => report('loaded ' + window.innerWidth + 'x' + window.innerHeight));
</script></body></html>"""


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith('/event?'):
            import urllib.parse
            with open(os.path.join(ROOT, 'events.log'), 'a') as f:
                f.write(urllib.parse.unquote(self.path[7:]) + '\n')
            body, ctype = b'ok', 'text/plain'
        elif os.environ.get('CLICK_TEST') and not self.path.startswith('/test.swf'):
            body, ctype = CLICK_PAGE, 'text/html'
        elif os.environ.get('MOUSE_TEST') and not self.path.startswith('/test.swf'):
            body, ctype = MOUSE_PAGE, 'text/html'
        elif os.environ.get('DISPLAY_TEST') and not self.path.startswith('/test.swf'):
            body, ctype = DISPLAY_PAGE, 'text/html'
        elif self.path.startswith('/test.swf'):
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
