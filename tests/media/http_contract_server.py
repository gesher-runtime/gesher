#!/usr/bin/env python3
"""Loopback-only deterministic HTTP peer for the real Windows WinHTTP fixture."""
from __future__ import annotations
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import re
import threading

CHUNK = 256 * 1024
LENGTH = 24 * CHUNK + 13
STREAM_LENGTH = 2 * 1024 * 1024 + 13
GATE = threading.Event()


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
        if self.path == "/release-gated":
            GATE.set()
            self.respond(200, b"1")
        elif self.path.startswith("/stream-"):
            self.stream()
        elif self.path == "/error":
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

    def stream(self):
        unknown = self.path == "/stream-unknown"
        truncated = self.path == "/stream-truncated"
        length = 1000 if truncated else STREAM_LENGTH
        if self.path == "/stream-empty":
            length = 0
        self.send_response(200)
        if not unknown:
            self.send_header("Content-Length", str(2000 if truncated else length))
        if unknown or truncated:
            self.send_header("Connection", "close")
            self.close_connection = True
        self.end_headers()
        pattern = bytes(range(251)) * 263
        try:
            for at in range(0, length, 65536):
                count = min(65536, length - at)
                start = at % 251
                self.wfile.write(pattern[start:start + count])
                self.wfile.flush()
                if self.path == "/stream-gated" and at == 0 and not GATE.wait(10):
                    # An eager consumer cannot finish its first read. Do not
                    # hang the gate forever when testing the old implementation.
                    self.close_connection = True
                    return
        except (BrokenPipeError, ConnectionResetError):
            pass  # expected when the source cancels or closes an unread tail

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
