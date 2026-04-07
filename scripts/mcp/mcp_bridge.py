#!/usr/bin/env python3
"""
Bridge between an MCP stdio client and a MAME MCP server.

The bridge handles the MCP protocol itself (initialize, tools/list) so that
the client always sees a healthy MCP server, even when MAME isn't running.
When MAME connects or disconnects, the bridge sends a tools/list_changed
notification so the client refreshes the tool list automatically.

Supports both Unix domain sockets and TCP:
  mcp_bridge.py /tmp/mame-mcp.sock       Unix socket
  mcp_bridge.py tcp:6789                  TCP localhost
  mcp_bridge.py tcp:host:port             TCP remote
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


def log(msg):
    print(f"mcp_bridge: {msg}", file=sys.stderr, flush=True)


def send_to_client(obj):
    """Send a JSON-RPC message to the stdio client via stdout."""
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def jsonrpc_result(req_id, result):
    return {"jsonrpc": "2.0", "id": req_id, "result": result}


def jsonrpc_error(req_id, code, message):
    return {"jsonrpc": "2.0", "id": req_id, "error": {"code": code, "message": message}}


class MameBridge:
    def __init__(self, endpoint):
        self.family, self.address = parse_endpoint(endpoint)
        self.endpoint = endpoint
        self.sock = None
        self.lock = threading.Lock()
        self.reader_thread = None
        self.monitor_thread = None
        self.cached_tools = []
        self.pending = {}  # id -> threading.Event, response
        self.pending_lock = threading.Lock()
        self.was_connected = False

    def connect(self):
        """Try to connect to MAME. Returns True on success."""
        with self.lock:
            self._close_unlocked()
            try:
                s = socket.socket(self.family, socket.SOCK_STREAM)
                s.settimeout(2)
                s.connect(self.address)
                s.settimeout(None)
                self.sock = s
                return True
            except (ConnectionRefusedError, FileNotFoundError, OSError):
                return False

    def _close_unlocked(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def is_connected(self):
        with self.lock:
            return self.sock is not None

    def send(self, data):
        with self.lock:
            if not self.sock:
                return False
            try:
                self.sock.sendall(data)
                return True
            except OSError:
                self._close_unlocked()
                return False

    def send_request(self, method, params=None, timeout=10):
        """Send a JSON-RPC request to MAME and wait for the response."""
        import random
        req_id = random.randint(100000, 999999)
        msg = {"jsonrpc": "2.0", "id": req_id, "method": method}
        if params is not None:
            msg["params"] = params

        event = threading.Event()
        entry = {"event": event, "response": None}
        with self.pending_lock:
            self.pending[req_id] = entry

        if not self.send((json.dumps(msg) + "\n").encode()):
            with self.pending_lock:
                del self.pending[req_id]
            return None

        event.wait(timeout=timeout)
        with self.pending_lock:
            entry = self.pending.pop(req_id, entry)
        return entry["response"]

    def _reader_loop(self):
        """Read from MAME socket, dispatch responses and forward tool results."""
        buf = b""
        while True:
            with self.lock:
                sock = self.sock
            if not sock:
                break
            try:
                data = sock.recv(65536)
                if not data:
                    break
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        msg = json.loads(line)
                    except json.JSONDecodeError:
                        continue

                    msg_id = msg.get("id")
                    with self.pending_lock:
                        entry = self.pending.get(msg_id)
                    if entry is not None:
                        # internal request (tools/list fetch etc)
                        entry["response"] = msg
                        entry["event"].set()
                    else:
                        # proxied tool call response - forward to the stdio client
                        send_to_client(msg)
            except OSError:
                break

        with self.lock:
            self._close_unlocked()

    def start_reader(self):
        if self.reader_thread and self.reader_thread.is_alive():
            self.reader_thread.join(timeout=1)
        self.reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self.reader_thread.start()

    def fetch_tools(self):
        """Fetch tool list from MAME and cache it."""
        resp = self.send_request("tools/list")
        if resp and "result" in resp:
            tools = resp["result"].get("tools", [])
            self.cached_tools = tools
            log(f"fetched {len(tools)} tools from MAME")
            return True
        return False

    def on_connected(self):
        """Called when MAME connection is established."""
        log(f"connected to MAME at {self.endpoint}")
        self.start_reader()
        # initialize the MCP session with MAME
        self.send_request("initialize", {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "mcp_bridge", "version": "1.0"}
        })
        self.send((json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n").encode())
        self.fetch_tools()
        self.was_connected = True
        # notify the client that tools changed
        send_to_client({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})

    def on_disconnected(self):
        """Called when MAME connection is lost."""
        if self.was_connected:
            log("MAME disconnected")
            self.cached_tools = []
            self.was_connected = False
            # notify the client that tools changed (now empty)
            send_to_client({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})

    def start_monitor(self):
        """Background thread that monitors connection to MAME."""
        def monitor():
            while True:
                time.sleep(2)
                if not self.is_connected():
                    if self.was_connected:
                        self.on_disconnected()
                    if self.connect():
                        self.on_connected()

        self.monitor_thread = threading.Thread(target=monitor, daemon=True)
        self.monitor_thread.start()


def main():
    if len(sys.argv) < 2:
        print("Usage: mcp_bridge.py <endpoint>", file=sys.stderr)
        print("  endpoint: /path/to/socket | tcp:port | tcp:host:port", file=sys.stderr)
        sys.exit(1)

    bridge = MameBridge(sys.argv[1])

    # try initial connection
    if bridge.connect():
        bridge.on_connected()
    else:
        log(f"waiting for MAME on {bridge.endpoint}...")

    # start background monitor for (re)connections
    bridge.start_monitor()

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue

        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue

        req_id = msg.get("id")
        method = msg.get("method", "")

        # handle MCP protocol locally
        if method == "initialize":
            send_to_client(jsonrpc_result(req_id, {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {"listChanged": True}},
                "serverInfo": {"name": "mame-mcp", "version": "1.0.0"}
            }))
            continue

        if method == "notifications/initialized":
            # client ack, nothing to do
            continue

        if method == "tools/list":
            send_to_client(jsonrpc_result(req_id, {"tools": bridge.cached_tools}))
            continue

        # proxy everything else (tools/call etc) to MAME
        if not bridge.is_connected():
            if req_id is not None:
                send_to_client(jsonrpc_error(req_id, -32000, "MAME is not running"))
            continue

        if not bridge.send((json.dumps(msg) + "\n").encode()):
            # send failed
            if req_id is not None:
                send_to_client(jsonrpc_error(req_id, -32000, "MAME connection lost"))
            continue
        # response will be forwarded by the reader thread


if __name__ == "__main__":
    main()
