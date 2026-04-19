// license:BSD-3-Clause
// copyright-holders:Erik Hemming
/***************************************************************************

    mcp_server.h

    MCP (Model Context Protocol) server for MAME.
    Provides structured, machine-readable introspection of a running
    MAME instance via JSON-RPC 2.0 with newline-delimited framing.

    Supports Unix domain sockets and TCP transports:
      -mcp /tmp/mame-mcp.sock       Unix socket (default)
      -mcp tcp:6789                  TCP on localhost
      -mcp tcp:0.0.0.0:6789         TCP on all interfaces

***************************************************************************/

#ifndef MAME_FRONTEND_MAME_MCP_SERVER_H
#define MAME_FRONTEND_MAME_MCP_SERVER_H

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

// forward declarations
class running_machine;

class mcp_server
{
public:
	mcp_server(const char* endpoint);
	~mcp_server();

	// called from main thread when machine starts/stops
	void set_machine(running_machine* machine);

private:
	// JSON-RPC dispatch
	std::string handle_request(const std::string& line);

	// MCP protocol handlers
	std::string handle_initialize(const std::string& id);
	std::string handle_tools_list(const std::string& id);
	std::string handle_tools_call(const std::string& id, const std::string& toolName, const std::string& params);

	// tool implementations
	std::string tool_machine_info();
	std::string tool_device_list();
	std::string tool_cpu_list();
	std::string tool_cpu_registers(const std::string& params);
	std::string tool_memory_read(const std::string& params);
	std::string tool_disassemble(const std::string& params);
	std::string tool_screen_info();
	std::string tool_media_list();
	std::string tool_ioport_list();
	std::string tool_debugger_break();
	std::string tool_debugger_run();
	std::string tool_soft_reset();
	std::string tool_hard_reset();
	std::string tool_exit();
	std::string tool_screenshot();
	std::string tool_step_into(const std::string& params);
	std::string tool_step_over(const std::string& params);
	std::string tool_step_out(const std::string& params);
	std::string tool_breakpoint_set(const std::string& params);
	std::string tool_breakpoint_clear(const std::string& params);
	std::string tool_breakpoint_clear_all(const std::string& params);
	std::string tool_breakpoint_enable(const std::string& params);
	std::string tool_breakpoint_list(const std::string& params);
	std::string tool_watchpoint_set(const std::string& params);
	std::string tool_watchpoint_clear(const std::string& params);
	std::string tool_watchpoint_clear_all(const std::string& params);
	std::string tool_watchpoint_enable(const std::string& params);
	std::string tool_watchpoint_list(const std::string& params);
	std::string tool_memory_write(const std::string& params);
	std::string tool_register_write(const std::string& params);
	std::string tool_media_load(const std::string& params);
	std::string tool_media_unload(const std::string& params);
	std::string tool_state_save(const std::string& params);
	std::string tool_state_load(const std::string& params);
	std::string tool_ioport_set(const std::string& params);
	std::string tool_ioport_get(const std::string& params);
	std::string tool_input_post(const std::string& params);
	std::string tool_mouse_input(const std::string& params);
	std::string tool_joystick_input(const std::string& params);
	std::string tool_input_list(const std::string& params);
	std::string tool_eval_expression(const std::string& params);
	std::string tool_memory_search(const std::string& params);
	std::string tool_run_to(const std::string& params);
	std::string tool_run_vblank(const std::string& params);
	std::string tool_run_msec(const std::string& params);
	std::string tool_memory_map(const std::string& params);

	// socket I/O
	void acceptLoop();
	void handleConnection(int clientFd);
	bool setupUnixSocket(const std::string& path);
	bool setupTcpSocket(const std::string& host, int port);

	enum class transport_type { UNIX_SOCKET, TCP };

	transport_type m_transport;
	std::string m_socketPath;      // Unix socket path (for cleanup)
	std::string m_listenAddress;   // display string for logging
	std::thread m_serverThread;
	running_machine* m_machine;
	std::mutex m_machineMutex;
	std::atomic<bool> m_running;
	int m_acceptorFd;
	std::atomic<int> m_clientFd;
};

#endif // MAME_FRONTEND_MAME_MCP_SERVER_H
