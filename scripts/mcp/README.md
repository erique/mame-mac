# MAME MCP Bridge

MAME exposes a JSON-RPC 2.0 server (the MCP server) for machine-readable introspection and debugger control. This directory contains the canonical documentation and a small stdio bridge (`mcp_bridge.py`) that lets MCP clients speaking stdio (such as Claude Code) talk to it.

## Architecture

```
MCP stdio client <--stdio--> mcp_bridge.py <--unix socket or tcp--> MAME
```

- **MAME** is the server: when launched with `-mcp <endpoint>`, it listens on a Unix domain socket or TCP port and accepts **one client at a time**.
- **The bridge** is a per-session adapter. It:
  - Handles the MCP protocol (`initialize`, `tools/list`) itself, so the client always sees a healthy server even when MAME isn't running.
  - Monitors the MAME connection, reconnects automatically when MAME (re)starts, and sends `notifications/tools/list_changed` so the client refreshes its tool list.
  - Exposes one local meta-tool, `bridge_info`, that reports where to tell MAME to connect.

Each client session launches its own bridge; each bridge owns exactly one MAME endpoint.

## Prerequisites

- Python 3.6+
- MAME built with MCP support (the `mcp_server` compiled into the frontend)
- MAME launched with the `-mcp` flag to enable the server

## Transport Options

| Transport | Bridge argument | MAME flag | Use case |
|-----------|-----------------|-----------|----------|
| Unix socket (auto) | *(no argument)* | `-mcp /tmp/mame-mcp-<bridge_pid>.sock` | Local, default |
| Unix socket (explicit path) | `/tmp/mame-mcp.sock` | `-mcp /tmp/mame-mcp.sock` | Local, known path |
| TCP localhost | `tcp:6789` | `-mcp tcp:6789` | Local, no socket file |
| TCP remote | `tcp:host:6789` | `-mcp tcp:0.0.0.0:6789` | Remote / headless |

**Auto mode is recommended for local use**: the bridge picks a unique socket path based on its own PID (e.g. `/tmp/mame-mcp-12345.sock`), which:

- Eliminates races between multiple bridges on one machine (each listens for its own path).
- Lets multiple concurrent client sessions each drive their own MAME.
- Tags MAME's command line with a session-unique string for surgical process management (see [Process management](#process-management) below).

## Discovering the socket

Every bridge session exposes a `bridge_info` MCP tool that returns:

```json
{
  "transport": "unix",
  "endpoint": "/tmp/mame-mcp-12345.sock",
  "mame_arg": "/tmp/mame-mcp-12345.sock",
  "connected": false,
  "bridge_pid": 12345
}
```

`mame_arg` is the exact string to pass to MAME's `-mcp` flag.

For use from outside MCP (shell scripts, logs), the bridge also prints a structured marker to stderr on startup:

```
BRIDGE_INFO: {"transport": "unix", "endpoint": "/tmp/mame-mcp-12345.sock", ...}
```

Grep for `^BRIDGE_INFO:` in the bridge's stderr to recover it.

## Setup

Configure your MCP client to launch the bridge as an stdio MCP server. The command is `python3 /path/to/mcp_bridge.py` — with **no arguments** for auto mode, or a transport arg for explicit mode.

### Example: Claude Code

Add an `mcpServers` entry in `~/.claude.json`. The entry can live at the **top level** (available to every project) or be **scoped to a specific project**:

**Top-level (global, available in any project):**

```json
{
  "mcpServers": {
    "mame": {
      "type": "stdio",
      "command": "python3",
      "args": ["/path/to/mame/scripts/mcp/mcp_bridge.py"],
      "env": {}
    }
  }
}
```

**Per-project (only active when running `claude` from that directory):**

```json
{
  "projects": {
    "/path/to/mame": {
      "mcpServers": {
        "mame": {
          "type": "stdio",
          "command": "python3",
          "args": ["/path/to/mame/scripts/mcp/mcp_bridge.py"],
          "env": {}
        }
      }
    }
  }
}
```

For an explicit Unix socket path or TCP endpoint, add it as a second arg:

```json
"args": ["/path/to/mame/scripts/mcp/mcp_bridge.py", "/tmp/mame-mcp.sock"]
"args": ["/path/to/mame/scripts/mcp/mcp_bridge.py", "tcp:6789"]
```

## Launch flow

1. **Start the MCP client.** The bridge comes up, prints its `BRIDGE_INFO:` line, and is immediately responsive. Tool list contains `bridge_info` only (MAME is not yet connected).
2. **Ask the bridge for its endpoint** — call the `bridge_info` tool. The returned `mame_arg` is what you pass to MAME.
3. **Launch MAME** with that endpoint:
   ```
   ./mamed <machine> -debug -mcp <mame_arg> [other flags...]
   ```
4. **Bridge auto-connects** within ~2 seconds, fetches the tool list from MAME, and notifies the client via `notifications/tools/list_changed`.
5. **All MAME MCP tools are now callable** alongside `bridge_info`.

When MAME exits, the bridge drops MAME's tools and goes back to serving only `bridge_info`. Launching MAME again reconnects automatically.

## Common patterns

These are composable tool sequences. They are pattern recipes, not bound to any specific client's skill format.

### Machine status

```
Parallel: machine_info, cpu_list, screen_info, media_list
If halted also: cpu_registers for each CPU
```

### Take a screenshot

```
screenshot  →  returns PNG (base64)
```

Works whether the machine is running or halted. Describe what you see: boot state, OS desktop, title screen, error text, etc.

### Debug at current PC

```
debugger_break
Parallel: cpu_registers <tag>, cpu_list
disassemble <tag> <PC> count=20
memory_map <tag>
Present: registers table + disasm with PC marked + region hit
```

### Boot monitor

```
Loop N times:
  run_vblank count=<frames>      # blocks until frames complete, leaves halted
  screenshot
  describe what's on screen
```

### Step trace

```
debugger_break
Loop N times:
  disassemble <tag> <PC> count=1   # show next instruction
  step_into   (or step_over if you don't want to enter subroutines)
  cpu_registers <tag>
  diff registers, record changes
```

### Memory inspection

```
debugger_break
memory_read <tag> <addr> <len>     # returns bytes
Format as hex dump with ASCII sidebar
If region is code: disassemble <tag> <addr> count=16
```

Alternatives: `memory_search` with a pattern to find occurrences; `memory_map` to list the address space regions.

### Type text into the running machine

```
input_post "<text>"     # natural keyboard, no halt required
```

For hardware buttons / DIP switches:

```
ioport_list                           # discover
ioport_set <tag> <mask> value=1
run_vblank count=<few frames>
ioport_set <tag> <mask> clear=true
```

### Launch flow (from a tool-capable client)

```
bridge_info                             → { mame_arg, ... }
ps aux | grep mamed                      (ensure nothing already running for this session)
rm -f <unix socket path>                 (clean stale socket, only for unix)
./mamed <machine> -debug -mcp <mame_arg> [...] &
wait ~2s for bridge auto-connect
machine_info                             (verify)
```

## Gotchas

- **`-debug` is required** for any debugger-level tool (`debugger_break`, `step_into`, breakpoints, `memory_read`, `cpu_registers`, …). Without it MAME runs in non-debug mode and these tools error out.
- **Most tools require the machine to be halted** (`debugger_break` first). Exceptions: `screenshot`, `input_post`, `mouse_input`, `keyboard_input`, and the read-only introspection tools (`machine_info`, `device_list`, `cpu_list`, `screen_info`, `media_list`, `ioport_list`, `input_list`).
- **`run_vblank` is synchronous**: it blocks until the requested frames have run and leaves the machine halted. Use `count=N` to advance several frames in one call.
- **Mouse delta chunking**: many emulated systems use 8-bit mouse counters. **Never inject more than 50 delta units per axis per frame** — split large moves into chunks with `run_vblank count=1` between them.
- **Stale Unix socket**: if MAME crashed without cleaning up, the socket file may still exist. MAME unlinks before bind, so this usually sorts itself out; `rm -f <path>` is a safe preflight step.
- **Single-client server**: MAME accepts exactly one MCP client at a time. With auto-mode sockets, each bridge has its own socket path so there is no contention. With explicit shared paths, only the first bridge to connect wins.
- **`bridge_info` shows `connected: false` before MAME launches** — that's expected. The client is connected to the bridge; the bridge is not yet connected to MAME.

## Process management

With auto-mode socket paths, every MAME process for a given bridge session has its socket path baked into its command line (e.g. `-mcp /tmp/mame-mcp-12345.sock`). This enables surgical process operations:

- **Find just this session's MAME**: `pgrep -f mame-mcp-<bridge_pid>`
- **Kill just this session's MAME**: `pkill -f mame-mcp-<bridge_pid>`
- **Preferred shutdown**: use the `exit` MCP tool (cooperative), falling back to `pgrep`/`kill -TERM` by PID only if `exit` is unresponsive.

**Do not** use broad patterns like `pkill -f mame` or `pkill -f mamed` — they will also kill other users' MAME processes on the machine, other active bridges, and the bridge itself (whose script path contains `mame`).

## Troubleshooting

- **"MAME is not running"**: The bridge returns this for any tool call other than `bridge_info` when it is not connected to MAME. Launch MAME with the `-mcp` flag matching `bridge_info`'s `mame_arg`.
- **Bridge exits immediately**: Check that `python3` is in your PATH and the bridge script path is correct.
- **Tools return errors about "No machine running"**: MAME is connected, but no emulated machine has started yet (still in the internal menu or loading).
- **TCP connection refused**: Ensure MAME and the bridge use the same port, and check firewall rules for remote connections.
- **Tools list didn't refresh after MAME started**: The bridge sends `notifications/tools/list_changed`; if the client doesn't act on it, re-issue a `tools/list` or reconnect the MCP session.
