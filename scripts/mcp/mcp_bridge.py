#!/usr/bin/env python3
"""
Bridge between an MCP stdio client and a MAME MCP server.
Reads JSON-RPC from stdin, forwards to socket. Reads responses from socket, writes to stdout.

Supports both Unix domain sockets and TCP:
  mcp_bridge.py /tmp/mame-mcp.sock       Unix socket
  mcp_bridge.py tcp:6789                  TCP localhost
  mcp_bridge.py tcp:host:port             TCP remote

Resilient: waits for MAME to appear, reconnects if MAME restarts.
"""

import json
import socket
import sys
import threading
import time


def parse_endpoint(endpoint):
    """Parse endpoint string into (socket_family, address) tuple."""
    if endpoint.startswith("tcp:"):
        rest = endpoint[4:]
        # "tcp:port" or "tcp:host:port"
        last_colon = rest.rfind(":")
        if last_colon >= 0:
            host = rest[:last_colon]
            port = int(rest[last_colon + 1:])
        else:
            host = "127.0.0.1"
            port = int(rest)
        return (socket.AF_INET, (host, port))
    else:
        return (socket.AF_UNIX, endpoint)


class MameBridge:
    def __init__(self, endpoint):
        self.family, self.address = parse_endpoint(endpoint)
        self.endpoint = endpoint
        self.sock = None
        self.lock = threading.Lock()
        self.reader_thread = None
        self.initialized = False

    def connect(self):
        """Try to connect to MAME. Returns True on success."""
        with self.lock:
            self.close_unlocked()
            try:
                s = socket.socket(self.family, socket.SOCK_STREAM)
                s.connect(self.address)
                self.sock = s
                self.initialized = False
                return True
            except (ConnectionRefusedError, FileNotFoundError, OSError):
                return False

    def close_unlocked(self):
        """Close current socket if open. Must hold self.lock."""
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def send(self, data):
        """Send data to MAME socket. Returns True on success."""
        with self.lock:
            if not self.sock:
                return False
            try:
                self.sock.sendall(data)
                return True
            except OSError:
                self.close_unlocked()
                return False

    def socket_to_stdout(self):
        """Read from socket, write to stdout. Exits when socket closes."""
        buf = b""
        while True:
            with self.lock:
                sock = self.sock
            if not sock:
                break
            try:
                data = sock.recv(4096)
                if not data:
                    break
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if line.strip():
                        sys.stdout.write(line.decode() + "\n")
                        sys.stdout.flush()
            except OSError:
                break
        with self.lock:
            self.close_unlocked()

    def start_reader(self):
        """Start background thread reading socket -> stdout."""
        if self.reader_thread and self.reader_thread.is_alive():
            self.reader_thread.join(timeout=1)
        self.reader_thread = threading.Thread(target=self.socket_to_stdout, daemon=True)
        self.reader_thread.start()

    def ensure_connected(self):
        """Block until connected to MAME, retrying every second."""
        with self.lock:
            if self.sock:
                return True
        while True:
            if self.connect():
                self.start_reader()
                return True
            time.sleep(1)

    def make_error_response(self, req_id, code, message):
        resp = {"jsonrpc": "2.0", "id": req_id, "error": {"code": code, "message": message}}
        sys.stdout.write(json.dumps(resp) + "\n")
        sys.stdout.flush()


def main():
    if len(sys.argv) < 2:
        print("Usage: mcp_bridge.py <endpoint>", file=sys.stderr)
        print("  endpoint: /path/to/socket | tcp:port | tcp:host:port", file=sys.stderr)
        sys.exit(1)

    bridge = MameBridge(sys.argv[1])

    # Try initial connect but don't block - MAME may not be up yet
    if bridge.connect():
        bridge.start_reader()
        print(f"mcp_bridge: connected to MAME at {bridge.endpoint}", file=sys.stderr)
    else:
        print(f"mcp_bridge: waiting for MAME on {bridge.endpoint}...", file=sys.stderr)

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue

        # Parse to extract id for error responses
        req_id = None
        try:
            msg = json.loads(line)
            req_id = msg.get("id")
        except json.JSONDecodeError:
            pass

        # Try to send; if not connected, try to reconnect
        with bridge.lock:
            connected = bridge.sock is not None
        if not connected:
            if not bridge.connect():
                if req_id is not None:
                    bridge.make_error_response(req_id, -32000, "MAME is not running")
                continue
            bridge.start_reader()
            print(f"mcp_bridge: connected to MAME at {bridge.endpoint}", file=sys.stderr)

        if not bridge.send((line + "\n").encode()):
            # Connection died, try reconnect once
            if bridge.connect():
                bridge.start_reader()
                print(f"mcp_bridge: reconnected to MAME at {bridge.endpoint}", file=sys.stderr)
                if not bridge.send((line + "\n").encode()):
                    if req_id is not None:
                        bridge.make_error_response(req_id, -32000, "Failed to send to MAME")
            else:
                if req_id is not None:
                    bridge.make_error_response(req_id, -32000, "MAME is not running")


if __name__ == "__main__":
    main()
