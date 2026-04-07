# MAME MCP Bridge

Bridge between an MCP (Model Context Protocol) stdio client and MAME's MCP server.

## Architecture

```
MCP client <--stdio--> mcp_bridge.py <--unix socket or tcp--> MAME MCP server
```

MAME exposes a JSON-RPC 2.0 server on a unix domain socket or TCP port. MCP clients typically speak MCP over stdio. The bridge connects the two by forwarding JSON-RPC messages between stdin/stdout and the server.

The bridge handles the MCP protocol itself (initialize, tools/list) so that
Claude Code always sees a healthy MCP server, even when MAME isn't running.
When MAME connects or disconnects, the bridge sends a `tools/list_changed`
notification so Claude Code refreshes the tool list automatically.

## Prerequisites

- Python 3.6+
- MAME built with MCP support (the `mcp_server` compiled into the frontend)
- MAME launched with the `-mcp` flag to enable the server

## Transport Options

MAME supports two transport mechanisms:

| Transport | MAME flag | Bridge argument | Use case |
|-----------|-----------|-----------------|----------|
| Unix socket | `-mcp /tmp/mame-mcp.sock` | `/tmp/mame-mcp.sock` | Local, recommended |
| TCP localhost | `-mcp tcp:8371` | `tcp:8371` | Local, no socket file |
| TCP remote | `-mcp tcp:0.0.0.0:8371` | `tcp:host:8371` | Remote/headless |

The MAME `-mcp` flag and the bridge argument use the same format.

## Setup

Configure your MCP client to launch the bridge as an stdio MCP server. The exact configuration depends on the client, but the command and arguments are:

```
command: python3
args: /path/to/mame/scripts/mcp/mcp_bridge.py /tmp/mame-mcp.sock
```

(or `tcp:6789` for TCP).

### Example: Claude Code

For Claude Code, add an `mcpServers` entry under your project in `~/.claude.json`:

#### Unix socket (recommended)

```json
{
  "projects": {
    "/path/to/mame": {
      "mcpServers": {
        "mame": {
          "type": "stdio",
          "command": "python3",
          "args": [
            "/path/to/mame/scripts/mcp/mcp_bridge.py",
            "/tmp/mame-mcp.sock"
          ],
          "env": {}
        }
      }
    }
  }
}
```

#### TCP

```json
{
  "projects": {
    "/path/to/mame": {
      "mcpServers": {
        "mame": {
          "type": "stdio",
          "command": "python3",
          "args": [
            "/path/to/mame/scripts/mcp/mcp_bridge.py",
            "tcp:8371"
          ],
          "env": {}
        }
      }
    }
  }
}
```

## Usage

1. Start MAME with the MCP server enabled:
   ```
   # Unix socket (recommended)
   ./mamed -debug -mcp /tmp/mame-mcp.sock a4000t

   # TCP
   ./mamed -debug -mcp tcp:8371 a4000t
   ```

2. Start your MCP client. The bridge will connect automatically when MAME is running.

3. The client can now use the MAME MCP tools: `machine_info`, `cpu_registers`, `memory_read`, `screenshot`, `mouse_input`, `input_post`, etc.

## Troubleshooting

- **"MAME is not running"**: The bridge returns this error if it cannot connect to the server. Start MAME with the `-mcp` flag.
- **Bridge exits immediately**: Check that `python3` is in your PATH and the bridge script path is correct.
- **Tools return errors about "No machine running"**: MAME is connected but the emulated machine hasn't started yet (still in the menu or loading).
- **TCP connection refused**: Ensure MAME and the bridge use the same port, and check firewall rules for remote connections.
