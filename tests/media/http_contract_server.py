#!/usr/bin/env python3
"""Loopback-only deterministic HTTP peer for the real Windows WinHTTP fixture."""
from __future__ import annotations
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import re

CHUNK = 256 * 1024
LENGTH = 24 * CHUNK + 13


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_GET(self):
        match = re.fullmatch(r"bytes=(\d+)-(\d+)", self.headers.get("Range", "bytes=0-262143"))
        if not match:
            self.respond(400, b"")
            return
        start, end = map(int, match.groups())
        if self.path == "/error":
            self.respond(500, b"")
        elif self.path == "/whole" or (self.path == "/ignore" and start > 0):
            self.respond(200, b"whole-file-source")
        elif self.path not in {"/range", "/ignore", "/unknown"}:
            self.respond(404, b"")
        elif start >= LENGTH:
            self.respond(416, b"", f"bytes */{LENGTH}")
        else:
            end = min(end, LENGTH - 1)
            if end < start or end - start + 1 > CHUNK:
                self.respond(400, b"")
                return
            body = bytes(index % 251 for index in range(start, end + 1))
            total = "*" if self.path == "/unknown" else str(LENGTH)
            self.respond(206, body, f"bytes {start}-{end}/{total}")

    def respond(self, status, body, content_range=None):
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        if content_range:
            self.send_header("Content-Range", content_range)
        self.end_headers()
        self.wfile.write(body)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ready", required=True, type=Path)
    args = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    # Existence is the parent's readiness signal. Publish only complete content;
    # opening the final file before write() leaves a small empty-file race.
    pending = args.ready.with_name(args.ready.name + ".pending")
    pending.write_text(str(server.server_port), encoding="ascii")
    pending.replace(args.ready)
    try:
        server.serve_forever()
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
