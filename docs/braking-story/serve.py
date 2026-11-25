#!/usr/bin/env python3
"""Serve the braking-story site on localhost with live reload.

    python3 docs/braking-story/serve.py          # http://localhost:8090
    python3 docs/braking-story/serve.py 8091     # another port

Any saved change under this folder reloads open pages (Server-Sent Events on
/__reload). Standard library only.
"""
import http.server, os, socketserver, sys, threading, time

ROOT = os.path.dirname(os.path.abspath(__file__))
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8090


def snapshot():
    out = {}
    for d, _, files in os.walk(ROOT):
        for f in files:
            p = os.path.join(d, f)
            try:
                out[p] = os.stat(p).st_mtime
            except OSError:
                pass
    return out


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=ROOT, **kw)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path != "/__reload":
            return super().do_GET()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        last = snapshot()
        try:
            while True:
                time.sleep(0.5)
                now = snapshot()
                if now != last:
                    last = now
                    self.wfile.write(b"data: reload\n\n")
                else:
                    self.wfile.write(b": ping\n\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


if __name__ == "__main__":
    with Server(("0.0.0.0", PORT), Handler) as s:
        print(f"braking story: http://localhost:{PORT}  (live reload on)", flush=True)
        s.serve_forever()
