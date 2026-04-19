// license:BSD-3-Clause
// copyright-holders:Erik Hemming
/***************************************************************************

    mcp_server.cpp

    MCP (Model Context Protocol) server for MAME.
    Provides structured, machine-readable introspection of a running
    MAME instance via JSON-RPC 2.0 with newline-delimited framing.

    Supports Unix domain sockets and TCP transports.

***************************************************************************/

#include "emu.h"
#include "mcp_server.h"

#include "debug/debugbuf.h"
#include "debug/debugcon.h"
#include "debug/debugcpu.h"
#include "debug/express.h"
#include "debug/points.h"
#include "debugger.h"
#include "natkeyboard.h"
#include "emuopts.h"
#include "didisasm.h"
#include "diexec.h"
#include "diimage.h"
#include "inputdev.h"
#include "distate.h"
#include "addrmap.h"
#include "screen.h"
#include "video.h"

#include "corefile.h"
#include "png.h"
#include "base64.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <poll.h>
#include <cstring>
#include <sstream>
#include <vector>


// helper: write a JSON-RPC 2.0 result envelope
static std::string jsonrpc_result(const std::string& id, const std::string& resultJson)
{
	return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + resultJson + "}\n";
}

// helper: write a JSON-RPC 2.0 error envelope
static std::string jsonrpc_error(const std::string& id, int code, const std::string& message)
{
	return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"error\":{\"code\":" + std::to_string(code) + ",\"message\":\"" + message + "\"}}\n";
}

// helper: escape a string for JSON output
static std::string json_escape(const std::string& s)
{
	std::string result;
	result.reserve(s.size() + 8);
	for (char c : s)
	{
		switch (c)
		{
		case '"':  result += "\\\""; break;
		case '\\': result += "\\\\"; break;
		case '\b': result += "\\b"; break;
		case '\f': result += "\\f"; break;
		case '\n': result += "\\n"; break;
		case '\r': result += "\\r"; break;
		case '\t': result += "\\t"; break;
		default:
			if (static_cast<unsigned char>(c) < 0x20)
			{
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
				result += buf;
			}
			else
			{
				result += c;
			}
			break;
		}
	}
	return result;
}

// helper: format a hex string
static std::string to_hex(uint64_t value, int digits = 0)
{
	char buf[32];
	if (digits > 0)
		snprintf(buf, sizeof(buf), "%0*llx", digits, (unsigned long long)value);
	else
		snprintf(buf, sizeof(buf), "%llx", (unsigned long long)value);
	return buf;
}

// helper: check if machine execution is halted (paused or debugger stopped)
static bool is_execution_halted(running_machine* machine)
{
	if (machine->paused())
		return true;
	if (machine->options().debug() && machine->debugger().cpu().is_stopped())
		return true;
	return false;
}

// helper: parse a hex string into bytes, returns false on invalid input
static bool parse_hex_string(const std::string& hex, std::vector<uint8_t>& out)
{
	if (hex.size() % 2 != 0)
		return false;
	out.clear();
	out.reserve(hex.size() / 2);
	for (size_t i = 0; i < hex.size(); i += 2)
	{
		char hi = hex[i];
		char lo = hex[i + 1];
		auto nibble = [](char c) -> int {
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return 10 + c - 'a';
			if (c >= 'A' && c <= 'F') return 10 + c - 'A';
			return -1;
		};
		int h = nibble(hi);
		int l = nibble(lo);
		if (h < 0 || l < 0)
			return false;
		out.push_back(static_cast<uint8_t>((h << 4) | l));
	}
	return true;
}

// helper: find a CPU device by tag and return its device_debug, with validation
static device_debug* find_cpu_debug(running_machine* machine, const std::string& cpuTag)
{
	device_t* dev = machine->root_device().subdevice(cpuTag.c_str());
	if (!dev)
		throw std::runtime_error("Device not found: " + cpuTag);
	device_debug* dbg = dev->debug();
	if (!dbg)
		throw std::runtime_error("Device has no debug interface: " + cpuTag);
	return dbg;
}


mcp_server::mcp_server(const char* endpoint)
	: m_transport(transport_type::UNIX_SOCKET)
	, m_machine(nullptr)
	, m_running(true)
	, m_acceptorFd(-1)
	, m_clientFd(-1)
{
	std::string ep(endpoint);
	bool ok = false;

	if (ep.substr(0, 4) == "tcp:")
	{
		// TCP transport: "tcp:port" or "tcp:host:port"
		m_transport = transport_type::TCP;
		std::string rest = ep.substr(4);
		std::string host = "127.0.0.1";
		int port = 0;

		size_t lastColon = rest.rfind(':');
		if (lastColon != std::string::npos)
		{
			host = rest.substr(0, lastColon);
			port = std::stoi(rest.substr(lastColon + 1));
		}
		else
		{
			port = std::stoi(rest);
		}

		ok = setupTcpSocket(host, port);
	}
	else
	{
		// Unix domain socket (default)
		m_socketPath = ep;
		ok = setupUnixSocket(ep);
	}

	if (ok)
	{
		osd_printf_verbose("MCP: listening on %s\n", m_listenAddress);
		m_serverThread = std::thread([this]() { acceptLoop(); });
	}
}

bool mcp_server::setupUnixSocket(const std::string& path)
{
	::unlink(path.c_str());

	m_acceptorFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (m_acceptorFd < 0)
	{
		osd_printf_error("MCP: failed to create unix socket: %s\n", strerror(errno));
		return false;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

	if (::bind(m_acceptorFd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
	{
		osd_printf_error("MCP: failed to bind unix socket '%s': %s\n", path, strerror(errno));
		::close(m_acceptorFd);
		m_acceptorFd = -1;
		return false;
	}

	if (::listen(m_acceptorFd, 1) < 0)
	{
		osd_printf_error("MCP: failed to listen on unix socket: %s\n", strerror(errno));
		::close(m_acceptorFd);
		m_acceptorFd = -1;
		return false;
	}

	m_listenAddress = path;
	return true;
}

bool mcp_server::setupTcpSocket(const std::string& host, int port)
{
	m_acceptorFd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (m_acceptorFd < 0)
	{
		osd_printf_error("MCP: failed to create tcp socket: %s\n", strerror(errno));
		return false;
	}

	int optval = 1;
	::setsockopt(m_acceptorFd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0)
	{
		osd_printf_error("MCP: invalid tcp address '%s'\n", host);
		::close(m_acceptorFd);
		m_acceptorFd = -1;
		return false;
	}

	if (::bind(m_acceptorFd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
	{
		osd_printf_error("MCP: failed to bind tcp socket %s:%d: %s\n", host, port, strerror(errno));
		::close(m_acceptorFd);
		m_acceptorFd = -1;
		return false;
	}

	if (::listen(m_acceptorFd, 1) < 0)
	{
		osd_printf_error("MCP: failed to listen on tcp socket: %s\n", strerror(errno));
		::close(m_acceptorFd);
		m_acceptorFd = -1;
		return false;
	}

	m_listenAddress = host + ":" + std::to_string(port);
	return true;
}

mcp_server::~mcp_server()
{
	m_running = false;

	int fd = m_clientFd.load();
	if (fd >= 0)
		::shutdown(fd, SHUT_RDWR);

	if (m_acceptorFd >= 0)
	{
		::shutdown(m_acceptorFd, SHUT_RDWR);
		::close(m_acceptorFd);
		m_acceptorFd = -1;
	}

	if (m_serverThread.joinable())
		m_serverThread.join();

	if (m_transport == transport_type::UNIX_SOCKET && !m_socketPath.empty())
		::unlink(m_socketPath.c_str());
}

void mcp_server::set_machine(running_machine* machine)
{
	std::lock_guard<std::mutex> lock(m_machineMutex);
	m_machine = machine;

	// When the machine goes away, shut down the server so
	// the process can exit cleanly.
	if (!machine)
	{
		m_running = false;

		// Close the acceptor first to prevent new connections
		if (m_acceptorFd >= 0)
		{
			::shutdown(m_acceptorFd, SHUT_RDWR);
			::close(m_acceptorFd);
			m_acceptorFd = -1;
		}

		// Disconnect any active client
		int fd = m_clientFd.load();
		if (fd >= 0)
			::shutdown(fd, SHUT_RDWR);
	}
}

void mcp_server::acceptLoop()
{
	while (m_running && m_acceptorFd >= 0)
	{
		// Use poll with timeout so we can check m_running periodically
		struct pollfd pfd = { m_acceptorFd, POLLIN, 0 };
		int ret = ::poll(&pfd, 1, 500);
		if (ret < 0 || !m_running)
			break;
		if (ret == 0)
			continue;

		int clientFd = ::accept(m_acceptorFd, nullptr, nullptr);
		if (clientFd < 0)
		{
			if (m_running)
				osd_printf_verbose("MCP: accept failed: %s\n", strerror(errno));
			break;
		}

		osd_printf_verbose("MCP: client connected\n");
		handleConnection(clientFd);
		m_clientFd.store(-1);
		::close(clientFd);
		osd_printf_verbose("MCP: client disconnected\n");
	}
}

void mcp_server::handleConnection(int clientFd)
{
	m_clientFd.store(clientFd);
	std::string buffer;
	char readBuf[4096];

	while (m_running)
	{
		// Use poll with timeout so we can check m_running periodically
		struct pollfd pfd = { clientFd, POLLIN, 0 };
		int ret = ::poll(&pfd, 1, 500); // 500ms timeout
		if (ret < 0)
			break;
		if (ret == 0)
			continue; // timeout, check m_running
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			break;

		ssize_t n = ::read(clientFd, readBuf, sizeof(readBuf));
		if (n <= 0)
			break;

		buffer.append(readBuf, n);

		// process complete lines (newline-delimited JSON-RPC)
		size_t pos;
		while ((pos = buffer.find('\n')) != std::string::npos)
		{
			std::string line = buffer.substr(0, pos);
			buffer.erase(0, pos + 1);

			if (line.empty())
				continue;

			std::string response = handle_request(line);
			if (!response.empty())
			{
				// write response (already newline-terminated)
				const char* data = response.c_str();
				size_t remaining = response.size();
				while (remaining > 0)
				{
					ssize_t written = ::write(clientFd, data, remaining);
					if (written <= 0)
						return;
					data += written;
					remaining -= written;
				}
			}
		}
	}
}

std::string mcp_server::handle_request(const std::string& line)
{
	rapidjson::Document doc;
	doc.Parse(line.c_str());

	if (doc.HasParseError() || !doc.IsObject())
		return jsonrpc_error("null", -32700, "Parse error");

	// extract method
	if (!doc.HasMember("method") || !doc["method"].IsString())
		return jsonrpc_error("null", -32600, "Invalid Request");

	std::string method = doc["method"].GetString();

	// extract id (may be absent for notifications)
	std::string id = "null";
	if (doc.HasMember("id"))
	{
		if (doc["id"].IsString())
			id = "\"" + std::string(doc["id"].GetString()) + "\"";
		else if (doc["id"].IsInt())
			id = std::to_string(doc["id"].GetInt());
	}

	// handle notifications (no response needed)
	if (method == "notifications/initialized")
		return "";

	// dispatch methods
	if (method == "initialize")
		return handle_initialize(id);

	if (method == "tools/list")
		return handle_tools_list(id);

	if (method == "tools/call")
	{
		if (!doc.HasMember("params") || !doc["params"].IsObject())
			return jsonrpc_error(id, -32602, "Invalid params");

		const auto& params = doc["params"];
		if (!params.HasMember("name") || !params["name"].IsString())
			return jsonrpc_error(id, -32602, "Missing tool name");

		std::string toolName = params["name"].GetString();

		// serialize arguments back to JSON string
		std::string argsJson = "{}";
		if (params.HasMember("arguments") && params["arguments"].IsObject())
		{
			rapidjson::StringBuffer sb;
			rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
			params["arguments"].Accept(writer);
			argsJson = sb.GetString();
		}

		return handle_tools_call(id, toolName, argsJson);
	}

	return jsonrpc_error(id, -32601, "Method not found");
}

std::string mcp_server::handle_initialize(const std::string& id)
{
	std::string result =
		"{"
			"\"protocolVersion\":\"2024-11-05\","
			"\"capabilities\":{\"tools\":{}},"
			"\"serverInfo\":{"
				"\"name\":\"mame-mcp\","
				"\"version\":\"1.0.0\""
			"}"
		"}";
	return jsonrpc_result(id, result);
}

std::string mcp_server::handle_tools_list(const std::string& id)
{
	std::string result =
		"{\"tools\":["
			"{"
				"\"name\":\"machine_info\","
				"\"description\":\"Get information about the running machine\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{}}"
			"},"
			"{"
				"\"name\":\"device_list\","
				"\"description\":\"List all devices in the machine\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{}}"
			"},"
			"{"
				"\"name\":\"cpu_list\","
				"\"description\":\"List all CPUs in the machine\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{}}"
			"},"
			"{"
				"\"name\":\"cpu_registers\","
				"\"description\":\"Get register values for a CPU (requires machine paused)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU\"}"
				"},\"required\":[\"cpu_tag\"]}"
			"},"
			"{"
				"\"name\":\"memory_read\","
				"\"description\":\"Read bytes from a CPU address space (requires machine paused)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU\"},"
					"\"space\":{\"type\":\"integer\",\"description\":\"Address space index (0=program)\"},"
					"\"address\":{\"type\":\"integer\",\"description\":\"Start address\"},"
					"\"length\":{\"type\":\"integer\",\"description\":\"Number of bytes to read\"}"
				"},\"required\":[\"cpu_tag\",\"address\",\"length\"]}"
			"},"
			"{"
				"\"name\":\"disassemble\","
				"\"description\":\"Disassemble instructions at an address (requires machine paused)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU\"},"
					"\"address\":{\"type\":\"integer\",\"description\":\"Start address\"},"
					"\"count\":{\"type\":\"integer\",\"description\":\"Number of instructions to disassemble\"}"
				"},\"required\":[\"cpu_tag\",\"address\",\"count\"]}"
			"},"
			"{"
				"\"name\":\"screen_info\","
				"\"description\":\"Get information about all screens\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{}}"
			"},"
			"{"
				"\"name\":\"media_list\","
				"\"description\":\"List all mounted media/images\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{}}"
			"},"
			"{"
				"\"name\":\"ioport_list\","
				"\"description\":\"List all I/O ports and their fields\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{}}"
			"},"
			"{"
				"\"name\":\"debugger_break\","
				"\"description\":\"Break execution (halt the machine in the debugger)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{}}"
			"},"
			"{"
				"\"name\":\"debugger_run\","
				"\"description\":\"Resume execution (continue running after a break)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{}}"
			"},"
			"{"
				"\"name\":\"soft_reset\","
				"\"description\":\"Schedule a soft reset of the machine\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{}}"
			"},"
			"{"
				"\"name\":\"hard_reset\","
				"\"description\":\"Schedule a hard reset of the machine\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{}}"
			"},"
			"{"
				"\"name\":\"exit\","
				"\"description\":\"Exit MAME\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{}}"
			"},"
			"{"
				"\"name\":\"screenshot\","
				"\"description\":\"Take a screenshot of the current screen\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{}}"
			"},"
			"{"
				"\"name\":\"step_into\","
				"\"description\":\"Step into the next instruction (requires machine halted)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU\"},"
					"\"count\":{\"type\":\"integer\",\"description\":\"Number of steps (default 1)\"}"
				"},\"required\":[\"cpu_tag\"]}"
			"},"
			"{"
				"\"name\":\"step_over\","
				"\"description\":\"Step over the next instruction (requires machine halted)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU\"},"
					"\"count\":{\"type\":\"integer\",\"description\":\"Number of steps (default 1)\"}"
				"},\"required\":[\"cpu_tag\"]}"
			"},"
			"{"
				"\"name\":\"step_out\","
				"\"description\":\"Step out of the current subroutine (requires machine halted)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU\"}"
				"},\"required\":[\"cpu_tag\"]}"
			"},"
			"{"
				"\"name\":\"breakpoint_set\","
				"\"description\":\"Set a breakpoint at an address (requires debugger)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU\"},"
					"\"address\":{\"type\":\"integer\",\"description\":\"Address to break at\"},"
					"\"condition\":{\"type\":\"string\",\"description\":\"Optional condition expression\"}"
				"},\"required\":[\"cpu_tag\",\"address\"]}"
			"},"
			"{"
				"\"name\":\"breakpoint_clear\","
				"\"description\":\"Clear a breakpoint by index (requires debugger)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU\"},"
					"\"index\":{\"type\":\"integer\",\"description\":\"Breakpoint index to clear\"}"
				"},\"required\":[\"cpu_tag\",\"index\"]}"
			"},"
			"{"
				"\"name\":\"breakpoint_clear_all\","
				"\"description\":\"Clear all breakpoints (requires debugger)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU\"}"
				"},\"required\":[\"cpu_tag\"]}"
			"},"
			"{"
				"\"name\":\"breakpoint_enable\","
				"\"description\":\"Enable or disable a breakpoint (requires debugger)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU\"},"
					"\"index\":{\"type\":\"integer\",\"description\":\"Breakpoint index\"},"
					"\"enable\":{\"type\":\"boolean\",\"description\":\"True to enable, false to disable\"}"
				"},\"required\":[\"cpu_tag\",\"index\",\"enable\"]}"
			"},"
			"{"
				"\"name\":\"breakpoint_list\","
				"\"description\":\"List all breakpoints for a CPU (requires debugger)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU\"}"
				"},\"required\":[\"cpu_tag\"]}"
			"},"
			"{"
				"\"name\":\"watchpoint_set\","
				"\"description\":\"Set a watchpoint on a memory range (requires debugger)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU\"},"
					"\"space\":{\"type\":\"integer\",\"description\":\"Address space index (default 0)\"},"
					"\"type\":{\"type\":\"string\",\"description\":\"Watch type: r, w, or rw\"},"
					"\"address\":{\"type\":\"integer\",\"description\":\"Start address\"},"
					"\"length\":{\"type\":\"integer\",\"description\":\"Length of range to watch\"},"
					"\"condition\":{\"type\":\"string\",\"description\":\"Optional condition expression\"}"
				"},\"required\":[\"cpu_tag\",\"type\",\"address\",\"length\"]}"
			"},"
			"{"
				"\"name\":\"watchpoint_clear\","
				"\"description\":\"Clear a watchpoint by index (requires debugger)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU\"},"
					"\"index\":{\"type\":\"integer\",\"description\":\"Watchpoint index to clear\"}"
				"},\"required\":[\"cpu_tag\",\"index\"]}"
			"},"
			"{"
				"\"name\":\"watchpoint_clear_all\","
				"\"description\":\"Clear all watchpoints (requires debugger)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU\"}"
				"},\"required\":[\"cpu_tag\"]}"
			"},"
			"{"
				"\"name\":\"watchpoint_enable\","
				"\"description\":\"Enable or disable a watchpoint (requires debugger)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU\"},"
					"\"index\":{\"type\":\"integer\",\"description\":\"Watchpoint index\"},"
					"\"enable\":{\"type\":\"boolean\",\"description\":\"True to enable, false to disable\"}"
				"},\"required\":[\"cpu_tag\",\"index\",\"enable\"]}"
			"},"
			"{"
				"\"name\":\"watchpoint_list\","
				"\"description\":\"List all watchpoints for a CPU (requires debugger)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU\"}"
				"},\"required\":[\"cpu_tag\"]}"
			"},"
			"{"
				"\"name\":\"memory_write\","
				"\"description\":\"Write bytes to a CPU address space (requires machine halted)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU\"},"
					"\"address\":{\"type\":\"integer\",\"description\":\"Start address\"},"
					"\"data\":{\"type\":\"string\",\"description\":\"Hex string of bytes to write\"},"
					"\"space\":{\"type\":\"integer\",\"description\":\"Address space index (default 0)\"}"
				"},\"required\":[\"cpu_tag\",\"address\",\"data\"]}"
			"},"
			"{"
				"\"name\":\"register_write\","
				"\"description\":\"Write a value to a CPU register (requires machine halted)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU\"},"
					"\"name\":{\"type\":\"string\",\"description\":\"Register name\"},"
					"\"value\":{\"type\":\"integer\",\"description\":\"Value to write\"}"
				"},\"required\":[\"cpu_tag\",\"name\",\"value\"]}"
			"},"
			"{"
				"\"name\":\"media_load\","
				"\"description\":\"Load a media image file into a device\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"tag\":{\"type\":\"string\",\"description\":\"Device tag of the media slot\"},"
					"\"filename\":{\"type\":\"string\",\"description\":\"Path to the image file\"}"
				"},\"required\":[\"tag\",\"filename\"]}"
			"},"
			"{"
				"\"name\":\"media_unload\","
				"\"description\":\"Unload media from a device\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"tag\":{\"type\":\"string\",\"description\":\"Device tag of the media slot\"}"
				"},\"required\":[\"tag\"]}"
			"},"
			"{"
				"\"name\":\"state_save\","
				"\"description\":\"Save machine state to a file\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"filename\":{\"type\":\"string\",\"description\":\"Save state filename\"}"
				"},\"required\":[\"filename\"]}"
			"},"
			"{"
				"\"name\":\"state_load\","
				"\"description\":\"Load machine state from a file\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"filename\":{\"type\":\"string\",\"description\":\"Save state filename\"}"
				"},\"required\":[\"filename\"]}"
			"},"
			"{"
				"\"name\":\"ioport_set\","
				"\"description\":\"Set an I/O port field value (press/release buttons, set analog values). This is hardware-level ioport manipulation.\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"port_tag\":{\"type\":\"string\",\"description\":\"I/O port tag (from ioport_list)\"},"
					"\"mask\":{\"type\":\"integer\",\"description\":\"Field mask value (from ioport_list)\"},"
					"\"value\":{\"type\":\"integer\",\"description\":\"Value to set (1=pressed, 0=released for digital; analog value for analog fields)\"},"
					"\"clear\":{\"type\":\"boolean\",\"description\":\"If true, clear the override and return to normal input (default false)\"}"
				"},\"required\":[\"port_tag\",\"mask\",\"value\"]}"
			"},"
			"{"
				"\"name\":\"ioport_get\","
				"\"description\":\"Read current value of an I/O port field\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"port_tag\":{\"type\":\"string\",\"description\":\"I/O port tag (from ioport_list)\"},"
					"\"mask\":{\"type\":\"integer\",\"description\":\"Field mask value (from ioport_list)\"}"
				"},\"required\":[\"port_tag\",\"mask\"]}"
			"},"
			"{"
				"\"name\":\"input_post\","
				"\"description\":\"Type text via the natural keyboard (simulates keypresses)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"text\":{\"type\":\"string\",\"description\":\"UTF-8 text to type\"},"
					"\"rate_ms\":{\"type\":\"integer\",\"description\":\"Delay between characters in milliseconds (0 = default timing)\"}"
				"},\"required\":[\"text\"]}"
			"},"
			"{"
				"\"name\":\"mouse_input\","
				"\"description\":\"Inject mouse movement (relative) and button state via host-level input injection (requires machine halted)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"dx\":{\"type\":\"integer\",\"description\":\"Relative X movement in pixels\"},"
					"\"dy\":{\"type\":\"integer\",\"description\":\"Relative Y movement in pixels\"},"
					"\"buttons\":{\"type\":\"object\",\"description\":\"Button state: {left: bool, right: bool, middle: bool}\","
						"\"properties\":{"
							"\"left\":{\"type\":\"boolean\"},\"right\":{\"type\":\"boolean\"},\"middle\":{\"type\":\"boolean\"}"
						"}"
					"},"
					"\"device_index\":{\"type\":\"integer\",\"description\":\"Which mouse device (default 0)\"}"
				"}}"
			"},"
			"{"
				"\"name\":\"joystick_input\","
				"\"description\":\"Inject joystick axis positions and button state via host-level input injection (requires machine halted)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"axes\":{\"type\":\"object\",\"description\":\"Axis values normalized -1.0 to 1.0: {x: float, y: float, z: float, ...}\"},"
					"\"buttons\":{\"type\":\"object\",\"description\":\"Button state: {1: bool, 2: bool, ...}\"},"
					"\"device_index\":{\"type\":\"integer\",\"description\":\"Which joystick device (default 0)\"}"
				"}}"
			"},"
			"{"
				"\"name\":\"input_list\","
				"\"description\":\"List all host input devices and their items\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"class\":{\"type\":\"string\",\"description\":\"Filter by class: keyboard, mouse, joystick, lightgun (all if omitted)\"}"
				"}}"
			"},"
			"{"
				"\"name\":\"eval_expression\","
				"\"description\":\"Evaluate a MAME debugger expression (supports registers, memory access with b@/w@/d@, arithmetic, symbols)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU for symbol context\"},"
					"\"expression\":{\"type\":\"string\",\"description\":\"Expression to evaluate (e.g. 'pc+4', 'b@(a0)', 'd@(d@(a5+$22)+4)')\"}"
				"},\"required\":[\"cpu_tag\",\"expression\"]}"
			"},"
			"{"
				"\"name\":\"memory_search\","
				"\"description\":\"Search memory for a byte pattern or string\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU\"},"
					"\"space\":{\"type\":\"integer\",\"description\":\"Address space index (default 0)\"},"
					"\"start\":{\"type\":\"integer\",\"description\":\"Start address\"},"
					"\"end\":{\"type\":\"integer\",\"description\":\"End address\"},"
					"\"pattern\":{\"type\":\"string\",\"description\":\"Hex byte pattern to search for (e.g. 'deadbeef')\"},"
					"\"string\":{\"type\":\"string\",\"description\":\"UTF-8 string to search for (alternative to pattern)\"},"
					"\"max_results\":{\"type\":\"integer\",\"description\":\"Maximum number of results (default 100)\"}"
				"},\"required\":[\"cpu_tag\",\"start\",\"end\"]}"
			"},"
			"{"
				"\"name\":\"run_to\","
				"\"description\":\"Run until the PC reaches a specific address (requires halted)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU\"},"
					"\"address\":{\"type\":\"integer\",\"description\":\"Target address to stop at\"}"
				"},\"required\":[\"cpu_tag\",\"address\"]}"
			"},"
			"{"
				"\"name\":\"run_vblank\","
				"\"description\":\"Run for one or more vertical blanks (frames). Blocks until all frames complete (requires halted)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU\"},"
					"\"count\":{\"type\":\"integer\",\"description\":\"Number of vblanks to run (default 1)\"}"
				"},\"required\":[\"cpu_tag\"]}"
			"},"
			"{"
				"\"name\":\"run_msec\","
				"\"description\":\"Run for a specified number of milliseconds of emulated time (requires halted)\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU\"},"
					"\"milliseconds\":{\"type\":\"integer\",\"description\":\"Number of milliseconds to run\"}"
				"},\"required\":[\"cpu_tag\",\"milliseconds\"]}"
			"},"
			"{"
				"\"name\":\"memory_map\","
				"\"description\":\"List the memory map entries for a CPU address space\","
				"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
					"\"cpu_tag\":{\"type\":\"string\",\"description\":\"Device tag of the CPU\"},"
					"\"space\":{\"type\":\"integer\",\"description\":\"Address space index (default 0)\"}"
				"},\"required\":[\"cpu_tag\"]}"
			"}"
		"]}";
	return jsonrpc_result(id, result);
}

std::string mcp_server::handle_tools_call(const std::string& id, const std::string& toolName, const std::string& params)
{
	std::string content;

	try
	{
		if (toolName == "machine_info")
			content = tool_machine_info();
		else if (toolName == "device_list")
			content = tool_device_list();
		else if (toolName == "cpu_list")
			content = tool_cpu_list();
		else if (toolName == "cpu_registers")
			content = tool_cpu_registers(params);
		else if (toolName == "memory_read")
			content = tool_memory_read(params);
		else if (toolName == "disassemble")
			content = tool_disassemble(params);
		else if (toolName == "screen_info")
			content = tool_screen_info();
		else if (toolName == "media_list")
			content = tool_media_list();
		else if (toolName == "ioport_list")
			content = tool_ioport_list();
		else if (toolName == "debugger_break")
			content = tool_debugger_break();
		else if (toolName == "debugger_run")
			content = tool_debugger_run();
		else if (toolName == "soft_reset")
			content = tool_soft_reset();
		else if (toolName == "hard_reset")
			content = tool_hard_reset();
		else if (toolName == "exit")
			content = tool_exit();
		else if (toolName == "screenshot")
		{
			std::string b64 = tool_screenshot();
			std::string result = "{\"content\":[{\"type\":\"image\",\"data\":\"" + b64 + "\",\"mimeType\":\"image/png\"}]}";
			return jsonrpc_result(id, result);
		}
		else if (toolName == "step_into")
			content = tool_step_into(params);
		else if (toolName == "step_over")
			content = tool_step_over(params);
		else if (toolName == "step_out")
			content = tool_step_out(params);
		else if (toolName == "breakpoint_set")
			content = tool_breakpoint_set(params);
		else if (toolName == "breakpoint_clear")
			content = tool_breakpoint_clear(params);
		else if (toolName == "breakpoint_clear_all")
			content = tool_breakpoint_clear_all(params);
		else if (toolName == "breakpoint_enable")
			content = tool_breakpoint_enable(params);
		else if (toolName == "breakpoint_list")
			content = tool_breakpoint_list(params);
		else if (toolName == "watchpoint_set")
			content = tool_watchpoint_set(params);
		else if (toolName == "watchpoint_clear")
			content = tool_watchpoint_clear(params);
		else if (toolName == "watchpoint_clear_all")
			content = tool_watchpoint_clear_all(params);
		else if (toolName == "watchpoint_enable")
			content = tool_watchpoint_enable(params);
		else if (toolName == "watchpoint_list")
			content = tool_watchpoint_list(params);
		else if (toolName == "memory_write")
			content = tool_memory_write(params);
		else if (toolName == "register_write")
			content = tool_register_write(params);
		else if (toolName == "media_load")
			content = tool_media_load(params);
		else if (toolName == "media_unload")
			content = tool_media_unload(params);
		else if (toolName == "state_save")
			content = tool_state_save(params);
		else if (toolName == "state_load")
			content = tool_state_load(params);
		else if (toolName == "ioport_set")
			content = tool_ioport_set(params);
		else if (toolName == "ioport_get")
			content = tool_ioport_get(params);
		else if (toolName == "input_post")
			content = tool_input_post(params);
		else if (toolName == "mouse_input")
			content = tool_mouse_input(params);
		else if (toolName == "joystick_input")
			content = tool_joystick_input(params);
		else if (toolName == "input_list")
			content = tool_input_list(params);
		else if (toolName == "eval_expression")
			content = tool_eval_expression(params);
		else if (toolName == "memory_search")
			content = tool_memory_search(params);
		else if (toolName == "run_to")
			content = tool_run_to(params);
		else if (toolName == "run_vblank")
			content = tool_run_vblank(params);
		else if (toolName == "run_msec")
			content = tool_run_msec(params);
		else if (toolName == "memory_map")
			content = tool_memory_map(params);
		else
			return jsonrpc_error(id, -32602, "Unknown tool: " + toolName);
	}
	catch (std::exception& e)
	{
		std::string result = "{\"content\":[{\"type\":\"text\",\"text\":\"Error: " + json_escape(e.what()) + "\"}],\"isError\":true}";
		return jsonrpc_result(id, result);
	}

	std::string result = "{\"content\":[{\"type\":\"text\",\"text\":\"" + json_escape(content) + "\"}]}";
	return jsonrpc_result(id, result);
}


//**************************************************************************
//  TOOL IMPLEMENTATIONS
//**************************************************************************

std::string mcp_server::tool_machine_info()
{
	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");

	const game_driver& driver = m_machine->system();

	std::string paused = m_machine->paused() ? "true" : "false";
	std::string phase;
	switch (m_machine->phase())
	{
	case machine_phase::PREINIT:  phase = "preinit"; break;
	case machine_phase::INIT:     phase = "init"; break;
	case machine_phase::RESET:    phase = "reset"; break;
	case machine_phase::RUNNING:  phase = "running"; break;
	case machine_phase::EXIT:     phase = "exit"; break;
	default:                      phase = "unknown"; break;
	}

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartObject();
	w.Key("name"); w.String(driver.type.fullname());
	w.Key("shortname"); w.String(driver.name);
	w.Key("source_file"); w.String(driver.type.source());
	w.Key("paused"); w.Bool(is_execution_halted(m_machine));
	w.Key("phase"); w.String(phase.c_str());
	w.EndObject();

	return sb.GetString();
}

std::string mcp_server::tool_device_list()
{
	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartArray();

	for (device_t& dev : device_enumerator(m_machine->root_device()))
	{
		w.StartObject();
		w.Key("tag"); w.String(dev.tag());
		w.Key("type"); w.String(dev.shortname());
		w.Key("name"); w.String(dev.name());
		w.Key("clock"); w.Uint(dev.clock());

		device_t* parent = dev.owner();
		if (parent)
		{
			w.Key("parent_tag"); w.String(parent->tag());
		}
		else
		{
			w.Key("parent_tag"); w.Null();
		}

		w.EndObject();
	}

	w.EndArray();
	return sb.GetString();
}

std::string mcp_server::tool_cpu_list()
{
	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartArray();

	for (device_execute_interface& exec : execute_interface_enumerator(m_machine->root_device()))
	{
		device_t& dev = exec.device();
		w.StartObject();
		w.Key("tag"); w.String(dev.tag());
		w.Key("type"); w.String(dev.shortname());
		w.Key("name"); w.String(dev.name());
		w.Key("clock"); w.Uint(dev.clock());
		w.Key("suspended"); w.Bool(exec.suspended() != 0);
		w.EndObject();
	}

	w.EndArray();
	return sb.GetString();
}

std::string mcp_server::tool_cpu_registers(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");

	std::string cpuTag = args["cpu_tag"].GetString();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!is_execution_halted(m_machine))
		throw std::runtime_error("Machine must be paused or stopped in debugger");

	// find the device
	device_t* dev = m_machine->root_device().subdevice(cpuTag.c_str());
	if (!dev)
		throw std::runtime_error("Device not found: " + cpuTag);

	device_state_interface* state = nullptr;
	if (!dev->interface(state))
		throw std::runtime_error("Device has no state interface: " + cpuTag);

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartArray();

	for (auto& entry : state->state_entries())
	{
		if (!entry->visible())
			continue;

		w.StartObject();
		w.Key("name"); w.String(entry->symbol());

		if (entry->is_float())
		{
			w.Key("value"); w.String(entry->to_string().c_str());
		}
		else
		{
			w.Key("value"); w.String(to_hex(entry->value()).c_str());
		}

		w.EndObject();
	}

	w.EndArray();
	return sb.GetString();
}

std::string mcp_server::tool_memory_read(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");
	if (!args.HasMember("address") || !args["address"].IsNumber())
		throw std::runtime_error("Missing required parameter: address");
	if (!args.HasMember("length") || !args["length"].IsNumber())
		throw std::runtime_error("Missing required parameter: length");

	std::string cpuTag = args["cpu_tag"].GetString();
	uint64_t address = args["address"].GetUint64();
	uint32_t length = args["length"].GetUint();
	int spaceNum = 0;
	if (args.HasMember("space") && args["space"].IsNumber())
		spaceNum = args["space"].GetInt();

	// cap length to prevent abuse
	if (length > 65536)
		throw std::runtime_error("Length exceeds maximum (65536)");

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!is_execution_halted(m_machine))
		throw std::runtime_error("Machine must be paused or stopped in debugger");

	device_t* dev = m_machine->root_device().subdevice(cpuTag.c_str());
	if (!dev)
		throw std::runtime_error("Device not found: " + cpuTag);

	device_memory_interface* memintf = nullptr;
	if (!dev->interface(memintf))
		throw std::runtime_error("Device has no memory interface: " + cpuTag);

	if (!memintf->has_space(spaceNum))
		throw std::runtime_error("Address space not found: " + std::to_string(spaceNum));

	address_space& space = memintf->space(spaceNum);
	auto dis = m_machine->disable_side_effects();

	// read bytes into hex string
	std::string hexResult;
	hexResult.reserve(length * 2);
	for (uint32_t i = 0; i < length; i++)
	{
		uint8_t byte = space.read_byte(address + i);
		char hex[3];
		snprintf(hex, sizeof(hex), "%02x", byte);
		hexResult += hex;
	}

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartObject();
	w.Key("address"); w.String(to_hex(address).c_str());
	w.Key("length"); w.Uint(length);
	w.Key("hex"); w.String(hexResult.c_str());
	w.EndObject();

	return sb.GetString();
}

std::string mcp_server::tool_disassemble(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");
	if (!args.HasMember("address") || !args["address"].IsNumber())
		throw std::runtime_error("Missing required parameter: address");
	if (!args.HasMember("count") || !args["count"].IsNumber())
		throw std::runtime_error("Missing required parameter: count");

	std::string cpuTag = args["cpu_tag"].GetString();
	uint64_t address = args["address"].GetUint64();
	uint32_t count = args["count"].GetUint();

	if (count > 1000)
		throw std::runtime_error("Count exceeds maximum (1000)");

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!is_execution_halted(m_machine))
		throw std::runtime_error("Machine must be paused or stopped in debugger");

	device_t* dev = m_machine->root_device().subdevice(cpuTag.c_str());
	if (!dev)
		throw std::runtime_error("Device not found: " + cpuTag);

	device_disasm_interface* dasmintf = nullptr;
	if (!dev->interface(dasmintf))
		throw std::runtime_error("Device has no disassembly interface: " + cpuTag);

	device_memory_interface* memintf = nullptr;
	if (!dev->interface(memintf))
		throw std::runtime_error("Device has no memory interface: " + cpuTag);

	debug_disasm_buffer buffer(*dev);

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartArray();

	offs_t pc = address;
	for (uint32_t i = 0; i < count; i++)
	{
		std::string instruction;
		offs_t nextPc;
		offs_t size;
		u32 info;

		buffer.disassemble(pc, instruction, nextPc, size, info);

		std::string bytes = buffer.data_to_string(pc, size, true);

		w.StartObject();
		w.Key("address"); w.String(to_hex(pc).c_str());
		w.Key("bytes"); w.String(bytes.c_str());
		w.Key("instruction"); w.String(instruction.c_str());
		w.Key("size"); w.Uint(size);
		w.EndObject();

		pc = nextPc;
	}

	w.EndArray();
	return sb.GetString();
}

std::string mcp_server::tool_screen_info()
{
	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartArray();

	for (screen_device& screen : screen_device_enumerator(m_machine->root_device()))
	{
		w.StartObject();
		w.Key("tag"); w.String(screen.tag());

		const char* screenType = "unknown";
		switch (screen.screen_type())
		{
		case SCREEN_TYPE_RASTER:  screenType = "raster"; break;
		case SCREEN_TYPE_VECTOR:  screenType = "vector"; break;
		case SCREEN_TYPE_LCD:     screenType = "lcd"; break;
		case SCREEN_TYPE_SVG:     screenType = "svg"; break;
		default: break;
		}
		w.Key("type"); w.String(screenType);
		w.Key("width"); w.Uint(screen.width());
		w.Key("height"); w.Uint(screen.height());

		double refreshHz = ATTOSECONDS_TO_HZ(screen.refresh_attoseconds());
		w.Key("refresh_hz"); w.Double(refreshHz);

		w.Key("frame_number"); w.Uint64(screen.frame_number());

		const rectangle& visarea = screen.visible_area();
		w.Key("visible_area");
		w.StartObject();
		w.Key("left"); w.Int(visarea.left());
		w.Key("top"); w.Int(visarea.top());
		w.Key("right"); w.Int(visarea.right());
		w.Key("bottom"); w.Int(visarea.bottom());
		w.EndObject();

		w.EndObject();
	}

	w.EndArray();
	return sb.GetString();
}

std::string mcp_server::tool_media_list()
{
	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartArray();

	for (device_image_interface& image : image_interface_enumerator(m_machine->root_device()))
	{
		w.StartObject();
		w.Key("tag"); w.String(image.device().tag());
		w.Key("instance_name"); w.String(image.instance_name().c_str());
		w.Key("brief_name"); w.String(image.brief_instance_name().c_str());

		if (image.exists())
		{
			w.Key("is_loaded"); w.Bool(true);
			if (image.filename())
			{
				w.Key("filename"); w.String(image.filename());
			}
			if (image.basename())
			{
				w.Key("basename"); w.String(image.basename());
			}
		}
		else
		{
			w.Key("is_loaded"); w.Bool(false);
		}

		w.Key("is_readonly"); w.Bool(image.is_readonly());
		w.EndObject();
	}

	w.EndArray();
	return sb.GetString();
}

std::string mcp_server::tool_ioport_list()
{
	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");

	if (!m_machine->ioport().safe_to_read())
		throw std::runtime_error("I/O ports not yet initialized");

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartArray();

	for (auto& port : m_machine->ioport().ports())
	{
		w.StartObject();
		w.Key("tag"); w.String(port.second->tag());

		w.Key("fields");
		w.StartArray();

		for (ioport_field& field : port.second->fields())
		{
			w.StartObject();
			if (field.specific_name())
			{
				w.Key("name"); w.String(field.specific_name());
			}
			else
			{
				std::string fieldName = field.name();
				w.Key("name"); w.String(fieldName.c_str());
			}
			w.Key("mask"); w.String(to_hex(field.mask()).c_str());
			w.Key("type"); w.Uint(field.type());
			w.EndObject();
		}

		w.EndArray();
		w.EndObject();
	}

	w.EndArray();
	return sb.GetString();
}

std::string mcp_server::tool_debugger_break()
{
	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!m_machine->options().debug())
		throw std::runtime_error("Debugger is not active (launch with -debug)");

	debugger_cpu& dbgcpu = m_machine->debugger().cpu();
	if (dbgcpu.is_stopped())
		throw std::runtime_error("Already stopped");

	device_t* visibleCpu = m_machine->debugger().console().get_visible_cpu();
	visibleCpu->debug()->halt_on_next_instruction("MCP break\n");

	return "{\"status\":\"break_requested\"}";
}

std::string mcp_server::tool_debugger_run()
{
	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!m_machine->options().debug())
		throw std::runtime_error("Debugger is not active (launch with -debug)");

	debugger_cpu& dbgcpu = m_machine->debugger().cpu();
	if (!dbgcpu.is_stopped())
		throw std::runtime_error("Already running");

	device_t* visibleCpu = m_machine->debugger().console().get_visible_cpu();
	visibleCpu->debug()->go();

	return "{\"status\":\"running\"}";
}

std::string mcp_server::tool_soft_reset()
{
	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");

	m_machine->schedule_soft_reset();

	// If the debugger has halted execution, resume it so the reset can be processed
	if (m_machine->options().debug() && m_machine->debugger().cpu().is_stopped())
	{
		device_t* visibleCpu = m_machine->debugger().console().get_visible_cpu();
		visibleCpu->debug()->go();
	}

	return "{\"status\":\"soft_reset_scheduled\"}";
}

std::string mcp_server::tool_hard_reset()
{
	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");

	m_machine->schedule_hard_reset();

	// If the debugger has halted execution, resume it so the reset can be processed
	if (m_machine->options().debug() && m_machine->debugger().cpu().is_stopped())
	{
		device_t* visibleCpu = m_machine->debugger().console().get_visible_cpu();
		visibleCpu->debug()->go();
	}

	return "{\"status\":\"hard_reset_scheduled\"}";
}

std::string mcp_server::tool_exit()
{
	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");

	m_machine->schedule_exit();

	// If the debugger has halted execution, resume it so the exit can be processed
	if (m_machine->options().debug() && m_machine->debugger().cpu().is_stopped())
	{
		device_t* visibleCpu = m_machine->debugger().console().get_visible_cpu();
		visibleCpu->debug()->go();
	}

	return "{\"status\":\"exit_scheduled\"}";
}

std::string mcp_server::tool_screenshot()
{
	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");

	// Find the first screen
	screen_device_enumerator iter(m_machine->root_device());
	screen_device* screen = iter.first();
	if (!screen)
		throw std::runtime_error("No screen device found");

	// Get the current screen bitmap directly (avoids render target which can deadlock)
	screen_bitmap& sb = screen->curbitmap();
	if (!sb.valid() || sb.width() == 0 || sb.height() == 0)
		throw std::runtime_error("Screen bitmap not available");

	bitmap_t& bitmap = static_cast<bitmap_t&>(sb);

	// Get palette if needed
	const rgb_t* palette = nullptr;
	int paletteEntries = 0;
	if (screen->has_palette())
	{
		palette = screen->palette().palette()->entry_list_adjusted();
		paletteEntries = screen->palette().entries();
	}

	// Write PNG to temp file
	std::string tempPath = "/tmp/mame-mcp-screenshot.png";

	util::core_file::ptr file;
	std::error_condition err = util::core_file::open(tempPath, OPEN_FLAG_WRITE | OPEN_FLAG_CREATE, file);
	if (err)
		throw std::runtime_error("Failed to create temp file");

	err = util::png_write_bitmap(*file, nullptr, bitmap, paletteEntries, palette);
	file.reset();
	if (err)
	{
		::unlink(tempPath.c_str());
		throw std::runtime_error("Failed to write PNG");
	}

	// Read the file back
	std::vector<uint8_t> data;
	err = util::core_file::load(tempPath, data);
	if (err)
		throw std::runtime_error("Failed to read screenshot file");

	// Clean up temp file
	::unlink(tempPath.c_str());

	// Base64 encode
	return base64_encode(data.data(), data.size());
}


//**************************************************************************
//  STEP / BREAKPOINT / WATCHPOINT TOOLS
//**************************************************************************

std::string mcp_server::tool_step_into(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");

	std::string cpuTag = args["cpu_tag"].GetString();
	int count = 1;
	if (args.HasMember("count") && args["count"].IsNumber())
		count = args["count"].GetInt();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!m_machine->options().debug())
		throw std::runtime_error("Debugger is not active (launch with -debug)");
	if (!is_execution_halted(m_machine))
		throw std::runtime_error("Machine must be halted");

	device_debug* dbg = find_cpu_debug(m_machine, cpuTag);
	dbg->single_step(count);

	return "{\"status\":\"stepping\"}";
}

std::string mcp_server::tool_step_over(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");

	std::string cpuTag = args["cpu_tag"].GetString();
	int count = 1;
	if (args.HasMember("count") && args["count"].IsNumber())
		count = args["count"].GetInt();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!m_machine->options().debug())
		throw std::runtime_error("Debugger is not active (launch with -debug)");
	if (!is_execution_halted(m_machine))
		throw std::runtime_error("Machine must be halted");

	device_debug* dbg = find_cpu_debug(m_machine, cpuTag);
	dbg->single_step_over(count);

	return "{\"status\":\"stepping\"}";
}

std::string mcp_server::tool_step_out(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");

	std::string cpuTag = args["cpu_tag"].GetString();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!m_machine->options().debug())
		throw std::runtime_error("Debugger is not active (launch with -debug)");
	if (!is_execution_halted(m_machine))
		throw std::runtime_error("Machine must be halted");

	device_debug* dbg = find_cpu_debug(m_machine, cpuTag);
	dbg->single_step_out();

	return "{\"status\":\"stepping\"}";
}

std::string mcp_server::tool_breakpoint_set(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");
	if (!args.HasMember("address") || !args["address"].IsNumber())
		throw std::runtime_error("Missing required parameter: address");

	std::string cpuTag = args["cpu_tag"].GetString();
	offs_t address = args["address"].GetUint64();
	const char* condition = nullptr;
	std::string condStr;
	if (args.HasMember("condition") && args["condition"].IsString())
	{
		condStr = args["condition"].GetString();
		condition = condStr.c_str();
	}

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!m_machine->options().debug())
		throw std::runtime_error("Debugger is not active (launch with -debug)");

	device_debug* dbg = find_cpu_debug(m_machine, cpuTag);
	int index = dbg->breakpoint_set(address, condition);

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartObject();
	w.Key("index"); w.Int(index);
	w.Key("address"); w.String(to_hex(address).c_str());
	w.EndObject();

	return sb.GetString();
}

std::string mcp_server::tool_breakpoint_clear(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");
	if (!args.HasMember("index") || !args["index"].IsNumber())
		throw std::runtime_error("Missing required parameter: index");

	std::string cpuTag = args["cpu_tag"].GetString();
	int index = args["index"].GetInt();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!m_machine->options().debug())
		throw std::runtime_error("Debugger is not active (launch with -debug)");

	device_debug* dbg = find_cpu_debug(m_machine, cpuTag);
	bool cleared = dbg->breakpoint_clear(index);
	if (!cleared)
		throw std::runtime_error("Breakpoint not found: " + std::to_string(index));

	return "{\"status\":\"cleared\"}";
}

std::string mcp_server::tool_breakpoint_clear_all(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");

	std::string cpuTag = args["cpu_tag"].GetString();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!m_machine->options().debug())
		throw std::runtime_error("Debugger is not active (launch with -debug)");

	device_debug* dbg = find_cpu_debug(m_machine, cpuTag);
	dbg->breakpoint_clear_all();

	return "{\"status\":\"cleared_all\"}";
}

std::string mcp_server::tool_breakpoint_enable(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");
	if (!args.HasMember("index") || !args["index"].IsNumber())
		throw std::runtime_error("Missing required parameter: index");
	if (!args.HasMember("enable") || !args["enable"].IsBool())
		throw std::runtime_error("Missing required parameter: enable");

	std::string cpuTag = args["cpu_tag"].GetString();
	int index = args["index"].GetInt();
	bool enable = args["enable"].GetBool();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!m_machine->options().debug())
		throw std::runtime_error("Debugger is not active (launch with -debug)");

	device_debug* dbg = find_cpu_debug(m_machine, cpuTag);
	bool ok = dbg->breakpoint_enable(index, enable);
	if (!ok)
		throw std::runtime_error("Breakpoint not found: " + std::to_string(index));

	return enable ? "{\"status\":\"enabled\"}" : "{\"status\":\"disabled\"}";
}

std::string mcp_server::tool_breakpoint_list(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");

	std::string cpuTag = args["cpu_tag"].GetString();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!m_machine->options().debug())
		throw std::runtime_error("Debugger is not active (launch with -debug)");

	device_debug* dbg = find_cpu_debug(m_machine, cpuTag);

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartArray();

	for (const auto& bp : dbg->breakpoint_list())
	{
		w.StartObject();
		w.Key("index"); w.Int(bp.second->index());
		w.Key("enabled"); w.Bool(bp.second->enabled());
		w.Key("address"); w.String(to_hex(bp.second->address()).c_str());
		const char* cond = bp.second->condition();
		if (cond && cond[0])
		{
			w.Key("condition"); w.String(cond);
		}
		w.EndObject();
	}

	w.EndArray();
	return sb.GetString();
}


//**************************************************************************
//  WATCHPOINT TOOLS
//**************************************************************************

std::string mcp_server::tool_watchpoint_set(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");
	if (!args.HasMember("type") || !args["type"].IsString())
		throw std::runtime_error("Missing required parameter: type");
	if (!args.HasMember("address") || !args["address"].IsNumber())
		throw std::runtime_error("Missing required parameter: address");
	if (!args.HasMember("length") || !args["length"].IsNumber())
		throw std::runtime_error("Missing required parameter: length");

	std::string cpuTag = args["cpu_tag"].GetString();
	std::string typeStr = args["type"].GetString();
	offs_t address = args["address"].GetUint64();
	offs_t length = args["length"].GetUint64();
	int spaceNum = 0;
	if (args.HasMember("space") && args["space"].IsNumber())
		spaceNum = args["space"].GetInt();
	const char* condition = nullptr;
	std::string condStr;
	if (args.HasMember("condition") && args["condition"].IsString())
	{
		condStr = args["condition"].GetString();
		condition = condStr.c_str();
	}

	read_or_write rwType;
	if (typeStr == "r")
		rwType = read_or_write::READ;
	else if (typeStr == "w")
		rwType = read_or_write::WRITE;
	else if (typeStr == "rw")
		rwType = read_or_write::READWRITE;
	else
		throw std::runtime_error("Invalid type: must be 'r', 'w', or 'rw'");

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!m_machine->options().debug())
		throw std::runtime_error("Debugger is not active (launch with -debug)");

	device_t* dev = m_machine->root_device().subdevice(cpuTag.c_str());
	if (!dev)
		throw std::runtime_error("Device not found: " + cpuTag);

	device_memory_interface* memintf = nullptr;
	if (!dev->interface(memintf))
		throw std::runtime_error("Device has no memory interface: " + cpuTag);

	if (!memintf->has_space(spaceNum))
		throw std::runtime_error("Address space not found: " + std::to_string(spaceNum));

	address_space& space = memintf->space(spaceNum);
	device_debug* dbg = dev->debug();
	if (!dbg)
		throw std::runtime_error("Device has no debug interface: " + cpuTag);

	int index = dbg->watchpoint_set(space, rwType, address, length, condition);

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartObject();
	w.Key("index"); w.Int(index);
	w.Key("address"); w.String(to_hex(address).c_str());
	w.Key("length"); w.Uint64(length);
	w.EndObject();

	return sb.GetString();
}

std::string mcp_server::tool_watchpoint_clear(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");
	if (!args.HasMember("index") || !args["index"].IsNumber())
		throw std::runtime_error("Missing required parameter: index");

	std::string cpuTag = args["cpu_tag"].GetString();
	int index = args["index"].GetInt();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!m_machine->options().debug())
		throw std::runtime_error("Debugger is not active (launch with -debug)");

	device_debug* dbg = find_cpu_debug(m_machine, cpuTag);
	bool cleared = dbg->watchpoint_clear(index);
	if (!cleared)
		throw std::runtime_error("Watchpoint not found: " + std::to_string(index));

	return "{\"status\":\"cleared\"}";
}

std::string mcp_server::tool_watchpoint_clear_all(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");

	std::string cpuTag = args["cpu_tag"].GetString();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!m_machine->options().debug())
		throw std::runtime_error("Debugger is not active (launch with -debug)");

	device_debug* dbg = find_cpu_debug(m_machine, cpuTag);
	dbg->watchpoint_clear_all();

	return "{\"status\":\"cleared_all\"}";
}

std::string mcp_server::tool_watchpoint_enable(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");
	if (!args.HasMember("index") || !args["index"].IsNumber())
		throw std::runtime_error("Missing required parameter: index");
	if (!args.HasMember("enable") || !args["enable"].IsBool())
		throw std::runtime_error("Missing required parameter: enable");

	std::string cpuTag = args["cpu_tag"].GetString();
	int index = args["index"].GetInt();
	bool enable = args["enable"].GetBool();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!m_machine->options().debug())
		throw std::runtime_error("Debugger is not active (launch with -debug)");

	device_debug* dbg = find_cpu_debug(m_machine, cpuTag);
	bool ok = dbg->watchpoint_enable(index, enable);
	if (!ok)
		throw std::runtime_error("Watchpoint not found: " + std::to_string(index));

	return enable ? "{\"status\":\"enabled\"}" : "{\"status\":\"disabled\"}";
}

std::string mcp_server::tool_watchpoint_list(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");

	std::string cpuTag = args["cpu_tag"].GetString();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!m_machine->options().debug())
		throw std::runtime_error("Debugger is not active (launch with -debug)");

	device_t* dev = m_machine->root_device().subdevice(cpuTag.c_str());
	if (!dev)
		throw std::runtime_error("Device not found: " + cpuTag);

	device_debug* dbg = dev->debug();
	if (!dbg)
		throw std::runtime_error("Device has no debug interface: " + cpuTag);

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartArray();

	for (int spaceNum = 0; spaceNum < dbg->watchpoint_space_count(); spaceNum++)
	{
		for (const auto& wp : dbg->watchpoint_vector(spaceNum))
		{
			w.StartObject();
			w.Key("index"); w.Int(wp->index());
			w.Key("enabled"); w.Bool(wp->enabled());
			w.Key("space"); w.Int(spaceNum);

			const char* typeStr = "?";
			switch (wp->type())
			{
			case read_or_write::READ:      typeStr = "r"; break;
			case read_or_write::WRITE:     typeStr = "w"; break;
			case read_or_write::READWRITE: typeStr = "rw"; break;
			}
			w.Key("type"); w.String(typeStr);

			w.Key("address"); w.String(to_hex(wp->address()).c_str());
			w.Key("length"); w.Uint64(wp->length());

			const char* cond = wp->condition();
			if (cond && cond[0])
			{
				w.Key("condition"); w.String(cond);
			}
			w.EndObject();
		}
	}

	w.EndArray();
	return sb.GetString();
}


//**************************************************************************
//  MEMORY WRITE / REGISTER WRITE TOOLS
//**************************************************************************

std::string mcp_server::tool_memory_write(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");
	if (!args.HasMember("address") || !args["address"].IsNumber())
		throw std::runtime_error("Missing required parameter: address");
	if (!args.HasMember("data") || !args["data"].IsString())
		throw std::runtime_error("Missing required parameter: data");

	std::string cpuTag = args["cpu_tag"].GetString();
	uint64_t address = args["address"].GetUint64();
	std::string dataHex = args["data"].GetString();
	int spaceNum = 0;
	if (args.HasMember("space") && args["space"].IsNumber())
		spaceNum = args["space"].GetInt();

	std::vector<uint8_t> bytes;
	if (!parse_hex_string(dataHex, bytes))
		throw std::runtime_error("Invalid hex string");
	if (bytes.empty())
		throw std::runtime_error("No data to write");
	if (bytes.size() > 65536)
		throw std::runtime_error("Data exceeds maximum (65536 bytes)");

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!is_execution_halted(m_machine))
		throw std::runtime_error("Machine must be paused or stopped in debugger");

	device_t* dev = m_machine->root_device().subdevice(cpuTag.c_str());
	if (!dev)
		throw std::runtime_error("Device not found: " + cpuTag);

	device_memory_interface* memintf = nullptr;
	if (!dev->interface(memintf))
		throw std::runtime_error("Device has no memory interface: " + cpuTag);

	if (!memintf->has_space(spaceNum))
		throw std::runtime_error("Address space not found: " + std::to_string(spaceNum));

	address_space& space = memintf->space(spaceNum);

	for (size_t i = 0; i < bytes.size(); i++)
	{
		space.write_byte(address + i, bytes[i]);
	}

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartObject();
	w.Key("address"); w.String(to_hex(address).c_str());
	w.Key("length"); w.Uint64(bytes.size());
	w.EndObject();

	return sb.GetString();
}

std::string mcp_server::tool_register_write(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");
	if (!args.HasMember("name") || !args["name"].IsString())
		throw std::runtime_error("Missing required parameter: name");
	if (!args.HasMember("value") || !args["value"].IsNumber())
		throw std::runtime_error("Missing required parameter: value");

	std::string cpuTag = args["cpu_tag"].GetString();
	std::string regName = args["name"].GetString();
	uint64_t value = args["value"].GetUint64();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!is_execution_halted(m_machine))
		throw std::runtime_error("Machine must be paused or stopped in debugger");

	device_t* dev = m_machine->root_device().subdevice(cpuTag.c_str());
	if (!dev)
		throw std::runtime_error("Device not found: " + cpuTag);

	device_state_interface* state = nullptr;
	if (!dev->interface(state))
		throw std::runtime_error("Device has no state interface: " + cpuTag);

	// find the register by symbol name
	for (auto& entry : state->state_entries())
	{
		if (entry->symbol() == regName)
		{
			entry->set_value(value);

			rapidjson::StringBuffer sb;
			rapidjson::Writer<rapidjson::StringBuffer> w(sb);
			w.StartObject();
			w.Key("name"); w.String(regName.c_str());
			w.Key("value"); w.String(to_hex(value).c_str());
			w.EndObject();

			return sb.GetString();
		}
	}

	throw std::runtime_error("Register not found: " + regName);
}


//**************************************************************************
//  MEDIA LOAD / UNLOAD TOOLS
//**************************************************************************

std::string mcp_server::tool_media_load(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("tag") || !args["tag"].IsString())
		throw std::runtime_error("Missing required parameter: tag");
	if (!args.HasMember("filename") || !args["filename"].IsString())
		throw std::runtime_error("Missing required parameter: filename");

	std::string tag = args["tag"].GetString();
	std::string filename = args["filename"].GetString();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");

	device_t* dev = m_machine->root_device().subdevice(tag.c_str());
	if (!dev)
		throw std::runtime_error("Device not found: " + tag);

	device_image_interface* image = nullptr;
	if (!dev->interface(image))
		throw std::runtime_error("Device has no image interface: " + tag);

	auto [err, message] = image->load(filename);
	if (err)
		throw std::runtime_error("Load failed: " + message);

	return "{\"status\":\"loaded\"}";
}

std::string mcp_server::tool_media_unload(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("tag") || !args["tag"].IsString())
		throw std::runtime_error("Missing required parameter: tag");

	std::string tag = args["tag"].GetString();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");

	device_t* dev = m_machine->root_device().subdevice(tag.c_str());
	if (!dev)
		throw std::runtime_error("Device not found: " + tag);

	device_image_interface* image = nullptr;
	if (!dev->interface(image))
		throw std::runtime_error("Device has no image interface: " + tag);

	image->unload();

	return "{\"status\":\"unloaded\"}";
}


//**************************************************************************
//  SAVE STATE TOOLS
//**************************************************************************

std::string mcp_server::tool_state_save(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("filename") || !args["filename"].IsString())
		throw std::runtime_error("Missing required parameter: filename");

	std::string filename = args["filename"].GetString();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");

	if (is_execution_halted(m_machine))
	{
		m_machine->immediate_save(filename);
		return "{\"status\":\"saved\"}";
	}
	else
	{
		m_machine->schedule_save(std::string(filename));
		return "{\"status\":\"save_scheduled\"}";
	}
}

std::string mcp_server::tool_state_load(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("filename") || !args["filename"].IsString())
		throw std::runtime_error("Missing required parameter: filename");

	std::string filename = args["filename"].GetString();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");

	if (is_execution_halted(m_machine))
	{
		m_machine->immediate_load(filename);
		return "{\"status\":\"loaded\"}";
	}
	else
	{
		m_machine->schedule_load(std::string(filename));
		return "{\"status\":\"load_scheduled\"}";
	}
}


//**************************************************************************
//  INPUT INJECTION TOOLS
//**************************************************************************

std::string mcp_server::tool_ioport_set(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("port_tag") || !args["port_tag"].IsString())
		throw std::runtime_error("Missing required parameter: port_tag");
	if (!args.HasMember("mask") || !args["mask"].IsNumber())
		throw std::runtime_error("Missing required parameter: mask");
	if (!args.HasMember("value") || !args["value"].IsNumber())
		throw std::runtime_error("Missing required parameter: value");

	std::string portTag = args["port_tag"].GetString();
	ioport_value mask = args["mask"].GetUint();
	ioport_value value = args["value"].GetUint();
	bool clear = false;
	if (args.HasMember("clear") && args["clear"].IsBool())
		clear = args["clear"].GetBool();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");

	// Find the port
	const ioport_list& portlist = m_machine->ioport().ports();
	auto it = portlist.find(portTag);
	if (it == portlist.end())
		throw std::runtime_error("I/O port not found: " + portTag);
	ioport_port* port = it->second.get();

	// Find the field matching the mask
	ioport_field* target = nullptr;
	for (ioport_field& field : port->fields())
	{
		if (field.mask() == mask)
		{
			target = &field;
			break;
		}
	}

	if (!target)
		throw std::runtime_error("Field with mask " + to_hex(mask) + " not found in port " + portTag);

	if (clear)
		target->clear_value();
	else
		target->set_value(value);

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartObject();
	w.Key("status"); w.String(clear ? "cleared" : "set");
	w.Key("port_tag"); w.String(portTag.c_str());
	w.Key("mask"); w.String(to_hex(mask).c_str());
	w.Key("value"); w.Uint(value);
	w.EndObject();

	return sb.GetString();
}

std::string mcp_server::tool_ioport_get(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("port_tag") || !args["port_tag"].IsString())
		throw std::runtime_error("Missing required parameter: port_tag");
	if (!args.HasMember("mask") || !args["mask"].IsNumber())
		throw std::runtime_error("Missing required parameter: mask");

	std::string portTag = args["port_tag"].GetString();
	ioport_value mask = args["mask"].GetUint();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");

	// Find the port
	const ioport_list& portlist = m_machine->ioport().ports();
	auto it = portlist.find(portTag);
	if (it == portlist.end())
		throw std::runtime_error("I/O port not found: " + portTag);
	ioport_port* port = it->second.get();

	// Find the field matching the mask
	ioport_field* target = nullptr;
	for (ioport_field& field : port->fields())
	{
		if (field.mask() == mask)
		{
			target = &field;
			break;
		}
	}

	if (!target)
		throw std::runtime_error("Field with mask " + to_hex(mask) + " not found in port " + portTag);

	ioport_value portVal = port->read();
	ioport_value maskedVal = (portVal & mask);

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartObject();
	w.Key("port_tag"); w.String(portTag.c_str());
	w.Key("mask"); w.String(to_hex(mask).c_str());
	w.Key("value"); w.Uint(maskedVal);
	w.Key("defvalue"); w.Uint(target->defvalue() & mask);
	w.EndObject();

	return sb.GetString();
}

std::string mcp_server::tool_input_post(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("text") || !args["text"].IsString())
		throw std::runtime_error("Missing required parameter: text");

	std::string text = args["text"].GetString();
	int rateMs = 0;
	if (args.HasMember("rate_ms") && args["rate_ms"].IsNumber())
		rateMs = args["rate_ms"].GetInt();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");

	natural_keyboard& natkb = m_machine->natkeyboard();
	if (!natkb.can_post())
		throw std::runtime_error("Natural keyboard is not available for this machine");

	attotime rate = attotime::zero;
	if (rateMs > 0)
		rate = attotime::from_msec(rateMs);

	natkb.post_utf8(text, rate);

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartObject();
	w.Key("status"); w.String("posted");
	w.Key("length"); w.Uint(text.size());
	w.EndObject();

	return sb.GetString();
}


//**************************************************************************
//  HOST-LEVEL INPUT INJECTION TOOLS
//**************************************************************************

// helper: map device class name to enum
static input_device_class parse_device_class(const std::string& name)
{
	if (name == "keyboard") return DEVICE_CLASS_KEYBOARD;
	if (name == "mouse")    return DEVICE_CLASS_MOUSE;
	if (name == "lightgun") return DEVICE_CLASS_LIGHTGUN;
	if (name == "joystick") return DEVICE_CLASS_JOYSTICK;
	return DEVICE_CLASS_INVALID;
}

// helper: device class enum to name
static const char* device_class_name(input_device_class cls)
{
	switch (cls)
	{
	case DEVICE_CLASS_KEYBOARD: return "keyboard";
	case DEVICE_CLASS_MOUSE:    return "mouse";
	case DEVICE_CLASS_LIGHTGUN: return "lightgun";
	case DEVICE_CLASS_JOYSTICK: return "joystick";
	default:                    return "unknown";
	}
}

// helper: item class enum to name
static const char* item_class_name(input_item_class cls)
{
	switch (cls)
	{
	case ITEM_CLASS_SWITCH:   return "switch";
	case ITEM_CLASS_ABSOLUTE: return "absolute";
	case ITEM_CLASS_RELATIVE: return "relative";
	default:                  return "unknown";
	}
}

std::string mcp_server::tool_mouse_input(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	int dx = 0, dy = 0;
	if (args.HasMember("dx") && args["dx"].IsNumber())
		dx = args["dx"].GetInt();
	if (args.HasMember("dy") && args["dy"].IsNumber())
		dy = args["dy"].GetInt();

	int deviceIndex = 0;
	if (args.HasMember("device_index") && args["device_index"].IsNumber())
		deviceIndex = args["device_index"].GetInt();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!is_execution_halted(m_machine))
		throw std::runtime_error("Machine must be paused or stopped in debugger");

	input_class& mouseClass = m_machine->input().device_class(DEVICE_CLASS_MOUSE);
	input_device* dev = mouseClass.device(deviceIndex);
	if (!dev)
		throw std::runtime_error("Mouse device " + std::to_string(deviceIndex) + " not found");

	// Inject relative mouse movement via input_device_item::inject().
	// Uses persistent mode so the value is visible to ALL callers of
	// update_value() within the frame (the MAME UI reads mouse before
	// the ioport system does). The delta applies every frame until the
	// next mouse_input call overrides it (axes always default to 0).
	{
		input_device_item* itemX = dev->item(ITEM_ID_XAXIS);
		if (itemX)
			itemX->inject(dx * osd::input_device::RELATIVE_PER_PIXEL, false);
		input_device_item* itemY = dev->item(ITEM_ID_YAXIS);
		if (itemY)
			itemY->inject(dy * osd::input_device::RELATIVE_PER_PIXEL, false);
	}

	// Inject button state (persistent until explicitly changed)
	if (args.HasMember("buttons") && args["buttons"].IsObject())
	{
		const auto& buttons = args["buttons"];
		struct { const char* name; input_item_id id; } btnMap[] = {
			{ "left",   ITEM_ID_BUTTON1 },
			{ "right",  ITEM_ID_BUTTON2 },
			{ "middle", ITEM_ID_BUTTON3 },
		};
		for (auto& btn : btnMap)
		{
			if (buttons.HasMember(btn.name) && buttons[btn.name].IsBool())
			{
				input_device_item* item = dev->item(btn.id);
				if (item)
				{
					s32 val = buttons[btn.name].GetBool() ? 0x80 : 0;
					item->inject(val, false);
				}
			}
		}
	}

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartObject();
	w.Key("status"); w.String("injected");
	w.Key("device_index"); w.Int(deviceIndex);
	w.Key("dx"); w.Int(dx);
	w.Key("dy"); w.Int(dy);
	w.EndObject();

	return sb.GetString();
}

std::string mcp_server::tool_joystick_input(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	int deviceIndex = 0;
	if (args.HasMember("device_index") && args["device_index"].IsNumber())
		deviceIndex = args["device_index"].GetInt();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!is_execution_halted(m_machine))
		throw std::runtime_error("Machine must be paused or stopped in debugger");

	input_class& joyClass = m_machine->input().device_class(DEVICE_CLASS_JOYSTICK);
	input_device* dev = joyClass.device(deviceIndex);
	if (!dev)
		throw std::runtime_error("Joystick device " + std::to_string(deviceIndex) + " not found");

	// Inject axis positions (persistent until changed)
	if (args.HasMember("axes") && args["axes"].IsObject())
	{
		const auto& axes = args["axes"];
		struct { const char* name; input_item_id id; } axisMap[] = {
			{ "x", ITEM_ID_XAXIS },
			{ "y", ITEM_ID_YAXIS },
			{ "z", ITEM_ID_ZAXIS },
		};
		for (auto& axis : axisMap)
		{
			if (axes.HasMember(axis.name) && axes[axis.name].IsNumber())
			{
				double val = axes[axis.name].GetDouble();
				if (val < -1.0) val = -1.0;
				if (val > 1.0) val = 1.0;
				s32 rawVal = static_cast<s32>(val * input_device::ABSOLUTE_MAX);

				input_device_item* item = dev->item(axis.id);
				if (item)
					item->inject(rawVal, false);
			}
		}
	}

	// Inject button state (persistent until changed)
	if (args.HasMember("buttons") && args["buttons"].IsObject())
	{
		const auto& buttons = args["buttons"];
		for (auto it = buttons.MemberBegin(); it != buttons.MemberEnd(); ++it)
		{
			if (!it->value.IsBool())
				continue;
			int btnNum = std::atoi(it->name.GetString());
			if (btnNum < 1 || btnNum > 32)
				continue;
			input_item_id btnId = static_cast<input_item_id>(ITEM_ID_BUTTON1 + btnNum - 1);
			input_device_item* item = dev->item(btnId);
			if (item)
				item->inject(it->value.GetBool() ? 0x80 : 0x00, false);
		}
	}

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartObject();
	w.Key("status"); w.String("injected");
	w.Key("device_index"); w.Int(deviceIndex);
	w.EndObject();

	return sb.GetString();
}

std::string mcp_server::tool_input_list(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");

	// Determine which classes to enumerate
	std::vector<input_device_class> classes;
	if (args.HasMember("class") && args["class"].IsString())
	{
		input_device_class cls = parse_device_class(args["class"].GetString());
		if (cls == DEVICE_CLASS_INVALID)
			throw std::runtime_error("Invalid device class: " + std::string(args["class"].GetString()));
		classes.push_back(cls);
	}
	else
	{
		classes = { DEVICE_CLASS_KEYBOARD, DEVICE_CLASS_MOUSE, DEVICE_CLASS_LIGHTGUN, DEVICE_CLASS_JOYSTICK };
	}

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartArray();

	for (input_device_class devclass : classes)
	{
		input_class& inputClass = m_machine->input().device_class(devclass);
		for (int devnum = 0; devnum <= inputClass.maxindex(); devnum++)
		{
			input_device* dev = inputClass.device(devnum);
			if (!dev)
				continue;

			w.StartObject();
			w.Key("class"); w.String(device_class_name(devclass));
			w.Key("device_index"); w.Int(devnum);
			w.Key("name"); w.String(dev->name().c_str());
			w.Key("id"); w.String(dev->id().c_str());

			w.Key("items");
			w.StartArray();
			for (input_item_id itemid = ITEM_ID_FIRST_VALID; itemid <= dev->maxitem(); ++itemid)
			{
				input_device_item* item = dev->item(itemid);
				if (!item)
					continue;

				w.StartObject();
				w.Key("item_id"); w.Int(static_cast<int>(itemid));
				w.Key("name"); w.String(item->name().c_str());
				w.Key("item_class"); w.String(item_class_name(item->itemclass()));
				w.Key("current_value"); w.Int(item->current());
				w.EndObject();
			}
			w.EndArray();

			w.EndObject();
		}
	}

	w.EndArray();
	return sb.GetString();
}


//**************************************************************************
//  EXPRESSION EVALUATION TOOL
//**************************************************************************

std::string mcp_server::tool_eval_expression(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");
	if (!args.HasMember("expression") || !args["expression"].IsString())
		throw std::runtime_error("Missing required parameter: expression");

	std::string cpuTag = args["cpu_tag"].GetString();
	std::string expr = args["expression"].GetString();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!m_machine->options().debug())
		throw std::runtime_error("Debugger is not active (launch with -debug)");
	if (!is_execution_halted(m_machine))
		throw std::runtime_error("Machine must be paused or stopped in debugger");

	device_debug* dbg = find_cpu_debug(m_machine, cpuTag);
	symbol_table& symtable = dbg->symtable();

	try
	{
		parsed_expression parsedExpr(symtable, expr, 16);
		u64 result = parsedExpr.execute();

		rapidjson::StringBuffer sb;
		rapidjson::Writer<rapidjson::StringBuffer> w(sb);
		w.StartObject();
		w.Key("expression"); w.String(expr.c_str());
		w.Key("value"); w.Uint64(result);
		w.Key("hex"); w.String(to_hex(result).c_str());
		w.EndObject();

		return sb.GetString();
	}
	catch (expression_error& e)
	{
		throw std::runtime_error("Expression error at offset " + std::to_string(e.offset()) + ": " + e.code_string());
	}
}


//**************************************************************************
//  MEMORY SEARCH TOOL
//**************************************************************************

std::string mcp_server::tool_memory_search(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");
	if (!args.HasMember("start") || !args["start"].IsNumber())
		throw std::runtime_error("Missing required parameter: start");
	if (!args.HasMember("end") || !args["end"].IsNumber())
		throw std::runtime_error("Missing required parameter: end");

	std::string cpuTag = args["cpu_tag"].GetString();
	uint64_t startAddr = args["start"].GetUint64();
	uint64_t endAddr = args["end"].GetUint64();
	int spaceNum = 0;
	if (args.HasMember("space") && args["space"].IsNumber())
		spaceNum = args["space"].GetInt();
	int maxResults = 100;
	if (args.HasMember("max_results") && args["max_results"].IsNumber())
		maxResults = args["max_results"].GetInt();

	// Get search pattern - either hex pattern or string
	std::vector<uint8_t> searchBytes;
	if (args.HasMember("pattern") && args["pattern"].IsString())
	{
		std::string patternHex = args["pattern"].GetString();
		if (!parse_hex_string(patternHex, searchBytes))
			throw std::runtime_error("Invalid hex pattern");
	}
	else if (args.HasMember("string") && args["string"].IsString())
	{
		std::string searchStr = args["string"].GetString();
		searchBytes.assign(searchStr.begin(), searchStr.end());
	}
	else
	{
		throw std::runtime_error("Must provide either 'pattern' (hex) or 'string' parameter");
	}

	if (searchBytes.empty())
		throw std::runtime_error("Search pattern is empty");
	if (endAddr < startAddr)
		throw std::runtime_error("End address must be >= start address");

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!is_execution_halted(m_machine))
		throw std::runtime_error("Machine must be paused or stopped in debugger");

	device_t* dev = m_machine->root_device().subdevice(cpuTag.c_str());
	if (!dev)
		throw std::runtime_error("Device not found: " + cpuTag);

	device_memory_interface* memintf = nullptr;
	if (!dev->interface(memintf))
		throw std::runtime_error("Device has no memory interface: " + cpuTag);

	if (!memintf->has_space(spaceNum))
		throw std::runtime_error("Address space not found: " + std::to_string(spaceNum));

	address_space& space = memintf->space(spaceNum);
	auto dis = m_machine->disable_side_effects();

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartObject();
	w.Key("matches");
	w.StartArray();

	int found = 0;
	uint64_t searchEnd = endAddr - searchBytes.size() + 1;
	for (uint64_t addr = startAddr; addr <= searchEnd && found < maxResults; addr++)
	{
		bool match = true;
		for (size_t i = 0; i < searchBytes.size(); i++)
		{
			if (space.read_byte(addr + i) != searchBytes[i])
			{
				match = false;
				break;
			}
		}
		if (match)
		{
			w.String(to_hex(addr).c_str());
			found++;
		}
	}

	w.EndArray();
	w.Key("count"); w.Int(found);
	w.Key("pattern_length"); w.Uint(searchBytes.size());
	w.EndObject();

	return sb.GetString();
}


//**************************************************************************
//  RUN CONTROL TOOLS
//**************************************************************************

std::string mcp_server::tool_run_to(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");
	if (!args.HasMember("address") || !args["address"].IsNumber())
		throw std::runtime_error("Missing required parameter: address");

	std::string cpuTag = args["cpu_tag"].GetString();
	offs_t address = args["address"].GetUint64();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!m_machine->options().debug())
		throw std::runtime_error("Debugger is not active (launch with -debug)");
	if (!is_execution_halted(m_machine))
		throw std::runtime_error("Machine must be halted");

	device_debug* dbg = find_cpu_debug(m_machine, cpuTag);
	dbg->go(address);

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartObject();
	w.Key("status"); w.String("running");
	w.Key("target"); w.String(to_hex(address).c_str());
	w.EndObject();

	return sb.GetString();
}

std::string mcp_server::tool_run_vblank(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");

	std::string cpuTag = args["cpu_tag"].GetString();

	int count = 1;
	if (args.HasMember("count") && args["count"].IsInt())
		count = args["count"].GetInt();
	if (count < 1)
		count = 1;

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!m_machine->options().debug())
		throw std::runtime_error("Debugger is not active (launch with -debug)");
	if (!is_execution_halted(m_machine))
		throw std::runtime_error("Machine must be halted");

	device_debug* dbg = find_cpu_debug(m_machine, cpuTag);

	// Compute target frame number from first screen (0 = next vblank)
	u64 targetFrame = 0;
	u64 currentFrame = 0;
	if (count > 1)
	{
		for (screen_device& screen : screen_device_enumerator(m_machine->root_device()))
		{
			currentFrame = screen.frame_number();
			targetFrame = currentFrame + count;
			break;
		}
	}

	dbg->go_vblank(targetFrame);

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartObject();
	w.Key("status"); w.String("running");
	w.Key("count"); w.Int(count);
	if (count > 1)
	{
		w.Key("start_frame"); w.Uint64(currentFrame);
		w.Key("target_frame"); w.Uint64(targetFrame);
	}
	w.EndObject();
	return sb.GetString();
}

std::string mcp_server::tool_run_msec(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");
	if (!args.HasMember("milliseconds") || !args["milliseconds"].IsNumber())
		throw std::runtime_error("Missing required parameter: milliseconds");

	std::string cpuTag = args["cpu_tag"].GetString();
	u64 msec = args["milliseconds"].GetUint64();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");
	if (!m_machine->options().debug())
		throw std::runtime_error("Debugger is not active (launch with -debug)");
	if (!is_execution_halted(m_machine))
		throw std::runtime_error("Machine must be halted");

	device_debug* dbg = find_cpu_debug(m_machine, cpuTag);
	dbg->go_milliseconds(msec);

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartObject();
	w.Key("status"); w.String("running");
	w.Key("milliseconds"); w.Uint64(msec);
	w.EndObject();

	return sb.GetString();
}


//**************************************************************************
//  MEMORY MAP TOOL
//**************************************************************************

std::string mcp_server::tool_memory_map(const std::string& params)
{
	rapidjson::Document args;
	args.Parse(params.c_str());

	if (!args.HasMember("cpu_tag") || !args["cpu_tag"].IsString())
		throw std::runtime_error("Missing required parameter: cpu_tag");

	std::string cpuTag = args["cpu_tag"].GetString();
	int spaceNum = 0;
	if (args.HasMember("space") && args["space"].IsNumber())
		spaceNum = args["space"].GetInt();

	std::lock_guard<std::mutex> lock(m_machineMutex);
	if (!m_machine)
		throw std::runtime_error("No machine running");

	device_t* dev = m_machine->root_device().subdevice(cpuTag.c_str());
	if (!dev)
		throw std::runtime_error("Device not found: " + cpuTag);

	device_memory_interface* memintf = nullptr;
	if (!dev->interface(memintf))
		throw std::runtime_error("Device has no memory interface: " + cpuTag);

	if (!memintf->has_space(spaceNum))
		throw std::runtime_error("Address space not found: " + std::to_string(spaceNum));

	address_space& space = memintf->space(spaceNum);
	address_map* map = space.map();
	if (!map)
		throw std::runtime_error("No address map available for this space");

	auto handlerTypeName = [](map_handler_type type) -> const char* {
		switch (type)
		{
		case AMH_NONE:               return "none";
		case AMH_RAM:                return "ram";
		case AMH_ROM:                return "rom";
		case AMH_NOP:                return "nop";
		case AMH_UNMAP:              return "unmap";
		case AMH_DEVICE_DELEGATE:    return "device";
		case AMH_DEVICE_DELEGATE_M:  return "device";
		case AMH_DEVICE_DELEGATE_S:  return "device";
		case AMH_DEVICE_DELEGATE_SM: return "device";
		case AMH_DEVICE_DELEGATE_MO: return "device";
		case AMH_DEVICE_DELEGATE_SMO:return "device";
		case AMH_PORT:               return "port";
		case AMH_BANK:               return "bank";
		case AMH_DEVICE_SUBMAP:      return "submap";
		case AMH_VIEW:               return "view";
		default:                     return "unknown";
		}
	};

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartObject();
	w.Key("space_name"); w.String(space.name());
	w.Key("data_width"); w.Int(space.data_width());
	w.Key("addr_width"); w.Int(space.addr_width());
	w.Key("entries");
	w.StartArray();

	for (address_map_entry& entry : map->m_entrylist)
	{
		offs_t start = entry.m_addrstart & space.addrmask();
		offs_t end = entry.m_addrend & space.addrmask();

		w.StartObject();
		w.Key("start"); w.String(to_hex(start).c_str());
		w.Key("end"); w.String(to_hex(end).c_str());
		w.Key("read_type"); w.String(handlerTypeName(entry.m_read.m_type));
		w.Key("write_type"); w.String(handlerTypeName(entry.m_write.m_type));

		if (entry.m_read.m_name)
		{
			w.Key("read_name"); w.String(entry.m_read.m_name);
		}
		if (entry.m_write.m_name)
		{
			w.Key("write_name"); w.String(entry.m_write.m_name);
		}
		if (entry.m_region)
		{
			w.Key("region"); w.String(entry.m_region);
		}
		if (entry.m_share)
		{
			w.Key("share"); w.String(entry.m_share);
		}

		w.EndObject();
	}

	w.EndArray();
	w.EndObject();

	return sb.GetString();
}
