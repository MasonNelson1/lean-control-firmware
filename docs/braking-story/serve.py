#!/usr/bin/env python3
"""Serve the braking-story site on localhost with live reload.

    python3 docs/braking-story/serve.py          # http://localhost:8090
    python3 docs/braking-story/serve.py 8091     # another port

Any saved change under this folder reloads open pages (Server-Sent Events on
/__reload). Standard library only.
"""
import http.server, json, os, socketserver, sys, threading, time
from datetime import datetime

ROOT = os.path.dirname(os.path.abspath(__file__))
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8090
NOTES_JSON = os.path.join(ROOT, "notes.json")
NOTES_MD = os.path.join(ROOT, "notes.md")
NO_RELOAD = {NOTES_JSON, NOTES_MD}  # saving a note must not reload the page


def notes_markdown(notes):
    out = ["# Braking story notes", "", "Written by the site's Notes panel. Edit there, not here.", ""]
    for n in notes:
        try:
            when = datetime.fromisoformat(n.get("created", "").replace("Z", "+00:00")).astimezone().strftime("%Y-%m-%d %H:%M")
        except ValueError:
            when = n.get("created", "")[:16]
        out.append(f"## {when}" + (f" · {n['sectionTitle']}" if n.get("sectionTitle") else ""))
        if n.get("quote"):
            out.append("")
            out.append("> " + n["quote"].replace("\n", "\n> "))
            out.append(f"> ([jump](http://localhost:{PORT}/#note-{n['id']}))")
        if n.get("text"):
            out.append("")
            out.append(n["text"])
        out.append("")
    return "\n".join(out)


def snapshot():
    out = {}
    for d, _, files in os.walk(ROOT):
        for f in files:
            p = os.path.join(d, f)
            if p in NO_RELOAD:
                continue
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

    def do_POST(self):
        if self.path != "/__notes":
            self.send_error(404)
            return
        n = int(self.headers.get("Content-Length", 0))
        try:
            notes = json.loads(self.rfile.read(n) or b"[]")
            assert isinstance(notes, list)
        except Exception:
            self.send_error(400, "notes must be a JSON list")
            return
        tmp = NOTES_JSON + ".tmp"
        with open(tmp, "w") as f:
            json.dump(notes, f, indent=1, ensure_ascii=False)
        os.replace(tmp, NOTES_JSON)
        with open(NOTES_MD, "w") as f:
            f.write(notes_markdown(notes))
        self.send_response(204)
        self.end_headers()

    def do_GET(self):
        if self.path == "/__notes":
            body = open(NOTES_JSON, "rb").read() if os.path.exists(NOTES_JSON) else b"[]"
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
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
