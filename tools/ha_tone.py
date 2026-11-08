#!/usr/bin/env python3
"""Serve quiet synthetic test streams on HA's private LAN for a bounded lifetime.

Endpoints (all MP3, all low level):
  /tone.mp3     3 s 660 Hz tone at -30 dB
  /slow.mp3     valid headers, then one zero byte per second until the server expires
  /slow-open.mp3  the tone, but response headers only after 5 s (keeps a start opening)
  /invalid.mp3  deterministic non-MP3 bytes, to force a decoder failure
  any other     HTTP 404 on a kept-alive HTTP/1.1 connection, to fail the open after
                the connection is established; the server holds the socket for up to
                120 s and logs KEEPALIVE_END with who ended it (client-closed/server-timeout)
  /shutdown     end the server early (the validator calls it when finished)
Every request and the end of each slow stream are logged for the validator.
"""

import argparse
import base64
import subprocess
from types import SimpleNamespace

from ha_build import guest

SERVER = r'''
import base64, http.server, random, sys, threading, time
lifetime = float(sys.argv[1])
tone = base64.b64decode(sys.stdin.buffer.read(), validate=True)
invalid = bytes(random.Random(8794).getrandbits(8) & 0x7F for _ in range(256 * 1024))
start = time.monotonic()
deadline = start + lifetime
stop = threading.Event()
held = {}
def log(*parts):
    print(f"{time.monotonic() - start:.1f}", *parts, flush=True)
class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def setup(self):
        super().setup()
        self.connection.settimeout(2)
        self.held, self.end_reason = None, "client-closed"
    def handle(self):
        try:
            super().handle()
        except OSError as error:
            self.end_reason = f"client-reset:{type(error).__name__}"
        if self.held is not None:
            held.pop(id(self), None)
            log("KEEPALIVE_END", self.client_address[0], self.held[0],
                f"held={time.monotonic() - self.held[1]:.1f}", f"reason={self.end_reason}")
    def log_error(self, fmt, *args):
        if "timed out" in fmt:
            self.end_reason = "server-timeout"
    def body(self, data):
        self.send_response(200)
        self.send_header("Content-Type", "audio/mpeg")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)
    def do_GET(self):
        log("REQ", self.client_address[0], self.path)
        if self.path == "/slow.mp3":
            self.send_response(200)
            self.send_header("Content-Type", "audio/mpeg")
            self.send_header("Content-Length", str(1 << 30))
            self.send_header("Connection", "close")
            self.end_headers()
            sent, reason = 0, "server-expired"
            try:
                while time.monotonic() < deadline and not stop.is_set():
                    self.wfile.write(b"\0")
                    self.wfile.flush()
                    sent += 1
                    time.sleep(1)
            except OSError as error:
                reason = f"client-closed:{type(error).__name__}"
            log("SLOW_END", self.client_address[0], f"sent={sent}", f"reason={reason}")
        elif self.path == "/tone.mp3":
            self.body(tone)
        elif self.path == "/slow-open.mp3":
            stop.wait(5)
            try:
                self.body(tone)
                reason = "served"
            except OSError as error:
                reason = f"client-closed:{type(error).__name__}"
            log("SLOW_OPEN_END", self.client_address[0], f"reason={reason}")
        elif self.path == "/invalid.mp3":
            self.body(invalid)
        elif self.path == "/shutdown":
            stop.set()
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
        else:
            body = b"not found"
            self.send_response(404)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()
            self.connection.settimeout(120)
            self.held = (self.path, time.monotonic())
            held[id(self)] = (self.client_address[0], self.path, self.held[1])
            log("KEEPALIVE_404", self.client_address[0], self.path)
    def log_message(self, *args):
        pass
server = http.server.ThreadingHTTPServer(("0.0.0.0", 8794), Handler)
server.daemon_threads = True
server.timeout = 1
log("READY", f"lifetime={lifetime:.0f}")
try:
    while time.monotonic() < deadline and not stop.is_set():
        server.handle_request()
finally:
    server.server_close()
    time.sleep(1.5)  # let open slow streams record their end reason
    for address, path, since in list(held.values()):
        log("KEEPALIVE_OPEN", address, path, f"held={time.monotonic() - since:.1f}")
    log("STOPPED")
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=90)
    args = parser.parse_args()
    if not 10 <= args.seconds <= 3600:
        parser.error("--seconds must be between 10 and 3600")
    tone = subprocess.check_output([
        "ffmpeg", "-v", "error", "-f", "lavfi",
        "-i", "sine=frequency=660:duration=3:sample_rate=44100",
        "-af", "volume=0.03", "-ac", "2", "-codec:a", "libmp3lame",
        "-b:a", "128k", "-f", "mp3", "pipe:1",
    ])
    guest(SimpleNamespace(host="proxmox", vm=100, container="homeassistant"),
          ["python3", "-c", SERVER, str(args.seconds)], base64.b64encode(tone))


if __name__ == "__main__":
    main()
