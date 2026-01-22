// license:BSD-3-Clause
// copyright-holders:Erik Hemming
//
// DaynaPORT SCSI/Link Ethernet Adapter Emulation
//
// This emulates the Dayna Communications SCSI/Link adapter, which provides
// Ethernet connectivity over SCSI. Used by classic Macs, Amigas, and other
// systems with SCSI buses.
//
// Protocol reference: BlueSCSI, PiSCSI implementations

#include "emu.h"
#include "daynaport.h"

#include "multibyte.h"

#include <functional>
#include <sstream>

#define LOG_COMMAND  (1U << 1)
#define LOG_DATA     (1U << 2)
#define LOG_NETWORK  (1U << 3)

// #define VERBOSE (LOG_GENERAL | LOG_COMMAND | LOG_NETWORK | LOG_DATA)
#include "logmacro.h"

DEFINE_DEVICE_TYPE(NSCSI_DAYNAPORT, nscsi_daynaport_device, "nscsi_daynaport", "DaynaPORT SCSI/Link")

// Format MAC address as string
static std::string format_mac(const uint8_t *mac)
{
	return util::string_format("%02x:%02x:%02x:%02x:%02x:%02x",
		mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Format EtherType as string
static const char *format_ethertype(uint16_t ethertype)
{
	switch (ethertype)
	{
	case 0x0800: return "IPv4";
	case 0x0806: return "ARP";
	case 0x8035: return "RARP";
	case 0x809b: return "AppleTalk";
	case 0x86dd: return "IPv6";
	default:     return "???";
	}
}

// Hex dump helper - outputs buffer contents in hex+ASCII format
static void dump_buffer(std::function<void(const char *)> output, const uint8_t *buf, int size)
{
	for (int i = 0; i < size; i += 16)
	{
		std::ostringstream line;
		line << util::string_format("%04x: ", i);

		// Hex bytes
		int len = std::min(16, size - i);
		for (int j = 0; j < 16; j++)
		{
			if (j < len)
				line << util::string_format("%02x ", buf[i + j]);
			else
				line << "   ";
			if (j == 7)
				line << " ";
		}

		// ASCII
		line << " '";
		for (int j = 0; j < len; j++)
		{
			uint8_t c = buf[i + j];
			line << (char)((c >= 0x20 && c < 0x7f) ? c : '.');
		}
		line << "'";

		output(line.str().c_str());
	}
}

nscsi_daynaport_device::nscsi_daynaport_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: nscsi_full_device(mconfig, NSCSI_DAYNAPORT, tag, owner, clock)
	, device_network_interface(mconfig, *this, 10)  // 10 Mbit/s
	, m_rx_queue_head(0)
	, m_rx_queue_tail(0)
	, m_tx_size(0)
	, m_interface_enabled(false)
	, m_current_command(0)
	, m_rx_count(0)
	, m_tx_count(0)
	, m_data_buffer_size(0)
	, m_read_phase(0)
	, m_read_data_size(0)
{
	std::fill(std::begin(m_mac_address), std::end(m_mac_address), 0);
}

void nscsi_daynaport_device::device_start()
{
	nscsi_full_device::device_start();

	save_item(NAME(m_rx_queue_head));
	save_item(NAME(m_rx_queue_tail));
	save_item(NAME(m_tx_buffer));
	save_item(NAME(m_tx_size));
	save_item(NAME(m_mac_address));
	save_item(NAME(m_interface_enabled));
	save_item(NAME(m_current_command));
	save_item(NAME(m_rx_count));
	save_item(NAME(m_tx_count));
	save_item(NAME(m_data_buffer));
	save_item(NAME(m_data_buffer_size));
	save_item(NAME(m_read_phase));
	save_item(NAME(m_read_data_size));

	// Generate a random MAC address with Dayna OUI (00:00:c6)
	// In a real implementation you might want this configurable
	m_mac_address[0] = 0x00;
	m_mac_address[1] = 0x00;
	m_mac_address[2] = 0xc6;
	m_mac_address[3] = machine().rand() & 0xff;
	m_mac_address[4] = machine().rand() & 0xff;
	m_mac_address[5] = machine().rand() & 0xff;

	set_mac(m_mac_address);
}

void nscsi_daynaport_device::device_reset()
{
	nscsi_full_device::device_reset();

	m_rx_queue_head = 0;
	m_rx_queue_tail = 0;
	m_tx_size = 0;
	m_interface_enabled = false;
	m_current_command = 0;
	m_rx_count = 0;
	m_tx_count = 0;
	m_data_buffer_size = 0;
	m_read_phase = 0;
	m_read_data_size = 0;
}

int nscsi_daynaport_device::rx_queue_count() const
{
	if (m_rx_queue_tail >= m_rx_queue_head)
		return m_rx_queue_tail - m_rx_queue_head;
	else
		return PACKET_QUEUE_SIZE - m_rx_queue_head + m_rx_queue_tail;
}

int nscsi_daynaport_device::recv_start_cb(u8 *buf, int len)
{
	if (len >= 14)
	{
		uint16_t ethertype = (buf[12] << 8) | buf[13];
		LOGMASKED(LOG_NETWORK, "recv: %d bytes  dst=%s src=%s type=%04x (%s)\n",
			len, format_mac(&buf[0]), format_mac(&buf[6]), ethertype, format_ethertype(ethertype));
	}
	else
	{
		LOGMASKED(LOG_NETWORK, "recv: %d bytes (truncated)\n", len);
	}

	if (!m_interface_enabled)
	{
		LOGMASKED(LOG_NETWORK, "recv_start_cb: interface disabled, dropping packet\n");
		return 0;
	}

	if (rx_queue_full())
	{
		LOGMASKED(LOG_NETWORK, "recv_start_cb: queue full, dropping packet\n");
		return 0;
	}

	if (len > MAX_PACKET_SIZE)
	{
		LOGMASKED(LOG_NETWORK, "recv_start_cb: packet too large (%d), dropping\n", len);
		return 0;
	}

	// Queue the packet (FCS already stripped by network stack)
	Packet &pkt = m_rx_queue[m_rx_queue_tail];
	memcpy(pkt.data, buf, len);
	pkt.size = len;
	m_rx_queue_tail = (m_rx_queue_tail + 1) % PACKET_QUEUE_SIZE;
	m_rx_count++;

	return len;
}

void nscsi_daynaport_device::recv_complete_cb(int result)
{
	LOGMASKED(LOG_NETWORK, "recv_complete_cb: result=%d\n", result);
}

void nscsi_daynaport_device::send_complete_cb(int result)
{
	LOGMASKED(LOG_NETWORK, "send_complete_cb: result=%d\n", result);
}

void nscsi_daynaport_device::scsi_command()
{
	LOGMASKED(LOG_COMMAND, "command: %02x %02x %02x %02x %02x %02x\n",
		scsi_cmdbuf[0], scsi_cmdbuf[1], scsi_cmdbuf[2],
		scsi_cmdbuf[3], scsi_cmdbuf[4], scsi_cmdbuf[5]);

	switch (scsi_cmdbuf[0])
	{
	case SC_TEST_UNIT_READY:
		LOGMASKED(LOG_COMMAND, "command TEST UNIT READY\n");
		scsi_status_complete(SS_GOOD);
		break;

	case SC_INQUIRY:
		LOGMASKED(LOG_COMMAND, "command INQUIRY\n");
		do_inquiry();
		break;

	case DP_CMD_READ:
		LOGMASKED(LOG_COMMAND, "command READ\n");
		do_read();
		break;

	case DP_CMD_GET_STATS:
		LOGMASKED(LOG_COMMAND, "command GET STATS\n");
		do_get_stats();
		break;

	case DP_CMD_WRITE:
		LOGMASKED(LOG_COMMAND, "command WRITE\n");
		do_write();
		break;

	case DP_CMD_SET_IFACE_MODE:
		LOGMASKED(LOG_COMMAND, "command SET IFACE MODE\n");
		do_set_interface_mode();
		break;

	case DP_CMD_SET_MCAST:
		LOGMASKED(LOG_COMMAND, "command SET MCAST\n");
		do_set_multicast();
		break;

	case DP_CMD_ENABLE_IFACE:
		LOGMASKED(LOG_COMMAND, "command ENABLE IFACE\n");
		do_enable_interface();
		break;

	case SC_REQUEST_SENSE:
		LOGMASKED(LOG_COMMAND, "command REQUEST SENSE\n");
		// Return no sense data
		std::fill_n(scsi_cmdbuf, 18, 0);
		scsi_cmdbuf[0] = 0x70;  // Current errors
		scsi_cmdbuf[7] = 10;    // Additional sense length
		scsi_data_in(SBUF_MAIN, std::min(18, (int)scsi_cmdbuf[4]));
		scsi_status_complete(SS_GOOD);
		break;

	default:
		LOGMASKED(LOG_COMMAND, "command %02x ***UNKNOWN***\n", scsi_cmdbuf[0]);
		nscsi_full_device::scsi_command();
		break;
	}
}

void nscsi_daynaport_device::do_inquiry()
{
	int size = scsi_cmdbuf[4];

	LOGMASKED(LOG_COMMAND, "command INQUIRY alloc=%d\n", size);

	std::fill_n(scsi_cmdbuf, 96, 0);

	// Device type: 0x03 = Processor (used by DaynaPORT)
	scsi_cmdbuf[0] = 0x03;
	scsi_cmdbuf[1] = 0x00;  // Not removable
	scsi_cmdbuf[2] = 0x02;  // SCSI-2 compliance
	scsi_cmdbuf[3] = 0x02;  // Response data format
	scsi_cmdbuf[4] = 32;    // Additional length

	// Vendor ID (8 bytes, space-padded)
	memcpy(&scsi_cmdbuf[8], "Dayna   ", 8);

	// Product ID (16 bytes, space-padded)
	memcpy(&scsi_cmdbuf[16], "SCSI/Link       ", 16);

	// Product revision (4 bytes)
	memcpy(&scsi_cmdbuf[32], "1.4a", 4);

	if (size > 36)
		size = 36;

	scsi_data_in(SBUF_MAIN, size);
	scsi_status_complete(SS_GOOD);
}

void nscsi_daynaport_device::do_read()
{
	// CDB: 08 00 00 LL LL XX
	// LL LL = allocation length
	// XX: bit 7 = wait for data, bit 6 = ???
	int alloc_len = get_u16be(&scsi_cmdbuf[3]);
	bool wait_for_data = (scsi_cmdbuf[5] & 0x80) != 0;

	LOGMASKED(LOG_COMMAND, "command READ alloc=%d wait=%d queue=%d\n", alloc_len, wait_for_data, rx_queue_count());

	m_current_command = DP_CMD_READ;

	if (rx_queue_empty())
	{
		// No packets available - return zero-length packet
		// Format: [2-byte length=0][4-byte flags=0] = 6 bytes minimum
		LOGMASKED(LOG_DATA, "do_read: no packets, returning zero-length response\n");

		memset(m_data_buffer, 0, 6);
		m_data_buffer_size = 6;
		m_read_phase = 0;
		m_read_data_size = 0;

		// For empty response, single transfer is fine
		scsi_data_in(2, 6);
		scsi_status_complete(SS_GOOD);
		return;
	}

	// Dequeue a packet
	Packet &pkt = m_rx_queue[m_rx_queue_head];
	m_rx_queue_head = (m_rx_queue_head + 1) % PACKET_QUEUE_SIZE;

	// Format response: [2-byte length][4-byte flags][packet data]
	int pkt_len = pkt.size;

	if (pkt_len > DATA_BUFFER_SIZE - 6)  // 6 header
	{
		LOGMASKED(LOG_DATA, "do_read: packet too large (%d), truncating\n", pkt_len);
		pkt_len = DATA_BUFFER_SIZE - 6;
	}

	// Header: Length (big-endian) - packet data only, excluding header and CRC
	put_u16be(&m_data_buffer[0], pkt_len);

	// Header: Flags (byte 5 bit 4 = more packets available)
	m_data_buffer[2] = 0x00;
	m_data_buffer[3] = 0x00;
	m_data_buffer[4] = 0x00;
	m_data_buffer[5] = (rx_queue_empty() ? 0 : 0x10);

	// Packet data (starts at offset 6)
	memcpy(&m_data_buffer[6], pkt.data, pkt_len);

	m_data_buffer_size = 6 + pkt_len;
	m_read_phase = 0;
	m_read_data_size = pkt_len;

	LOGMASKED(LOG_DATA, "do_read: returning %d byte packet in two transfers (6 + %d bytes) with delay\n", pkt_len, pkt_len);

	// Queue two separate DATA-IN transfers with a delay between them:
	// 1. Header (6 bytes) - Mac reads this, parses length
	// 2. Delay - gives Mac time to process header and reset DMA
	//    (BlueSCSI uses 80µs on real hardware; emulation needs ~180µs)
	// 3. Packet data (pkt_len bytes) - Mac reads this based on parsed length
	scsi_data_in(2, 6);
	scsi_delay(180);
	scsi_data_in(2, pkt_len);
	scsi_status_complete(SS_GOOD);
}

void nscsi_daynaport_device::do_write()
{
	// CDB: 0a 00 00 LL LL XX
	// LL LL = data length
	// XX: bit 7 = padded format
	int data_len = get_u16be(&scsi_cmdbuf[3]);
	bool padded = (scsi_cmdbuf[5] & 0x80) != 0;

	if (padded)
		data_len += 8;  // Account for 8-byte header in padded format

	LOGMASKED(LOG_COMMAND, "command WRITE len=%d padded=%d\n", data_len, padded);

	if (data_len == 0 || data_len > MAX_PACKET_SIZE)
	{
		LOGMASKED(LOG_DATA, "do_write: invalid length %d\n", data_len);
		scsi_status_complete(SS_CHECK_CONDITION);
		sense(false, SK_ILLEGAL_REQUEST, SK_ASC_INVALID_FIELD_IN_CDB);
		return;
	}

	m_tx_size = 0;
	m_data_buffer_size = data_len;
	m_current_command = DP_CMD_WRITE;

	// Request data from initiator, then queue status completion
	// (operations are queued - status phase happens after data-out completes)
	scsi_data_out(2, data_len);
	scsi_status_complete(SS_GOOD);
}

void nscsi_daynaport_device::do_get_stats()
{
	// CDB: 09 00 00 00 LL 00
	// LL = allocation length (typically 0x12 = 18 bytes)
	int alloc_len = scsi_cmdbuf[4];

	LOGMASKED(LOG_COMMAND, "command GET STATS alloc=%d\n", alloc_len);

	// Response: [6-byte MAC][4-byte rx count][4-byte tx count][4-byte ???]
	// All counters are little-endian

	// MAC address
	memcpy(&m_data_buffer[0], m_mac_address, 6);

	// Receive count (little-endian)
	put_u32le(&m_data_buffer[6], m_rx_count);

	// Transmit count (little-endian)
	put_u32le(&m_data_buffer[10], m_tx_count);

	// Unknown counter (zero)
	put_u32le(&m_data_buffer[14], 0);

	m_data_buffer_size = 18;

	scsi_data_in(2, std::min(alloc_len, 18));
	scsi_status_complete(SS_GOOD);
}

void nscsi_daynaport_device::do_set_interface_mode()
{
	// CDB: 0c 00 00 00 FF XX
	// XX: 0x80 = set mode, 0x40 = set MAC address
	int mode = scsi_cmdbuf[5];

	LOGMASKED(LOG_COMMAND, "command SET INTERFACE MODE mode=%02x\n", mode);

	// For now, just accept and ignore
	// A real implementation might handle MAC address changes here
	scsi_status_complete(SS_GOOD);
}

void nscsi_daynaport_device::do_set_multicast()
{
	// CDB: 0d 00 00 00 LL 00
	// Host sends multicast address to register
	int data_len = scsi_cmdbuf[4];

	LOGMASKED(LOG_COMMAND, "command SET MULTICAST len=%d\n", data_len);

	if (data_len > 0)
	{
		// Request the multicast data from initiator
		m_data_buffer_size = data_len;
		m_tx_size = 0;
		m_current_command = DP_CMD_SET_MCAST;
		scsi_data_out(2, data_len);
	}

	// Queue status completion (for now, just accept and ignore multicast settings)
	scsi_status_complete(SS_GOOD);
}

void nscsi_daynaport_device::do_enable_interface()
{
	// CDB: 0e 00 00 00 00 XX
	// XX: 0x80 = enable, 0x00 = disable
	bool enable = (scsi_cmdbuf[5] & 0x80) != 0;

	LOGMASKED(LOG_COMMAND, "command ENABLE INTERFACE enable=%d\n", enable);

	m_interface_enabled = enable;

	if (enable)
	{
		// Connect to network interface if not already connected
		if (!has_net_device())
		{
			set_interface(0);  // Use first available interface
		}
	}

	scsi_status_complete(SS_GOOD);
}

uint8_t nscsi_daynaport_device::scsi_get_data(int id, int pos)
{
	if (id == 2)
	{
		// Two-phase READ: header (6 bytes) then data (m_read_data_size bytes)
		// pos resets to 0 for each BC_DATA_IN transfer
		if (m_current_command == DP_CMD_READ && m_read_data_size > 0)
		{
			if (m_read_phase == 0)
			{
				// Phase 0: Header bytes (pos 0-5)
				if (pos < 6)
				{
					uint8_t data = m_data_buffer[pos];
					if (pos == 5)
					{
						LOGMASKED(LOG_DATA, "scsi_get_data: header transfer complete, switching to data phase\n");
						m_read_phase = 1;
					}
					return data;
				}
			}
			else
			{
				// Phase 1: Packet data (pos 0 to m_read_data_size-1, offset by 6 in buffer)
				if (pos < m_read_data_size)
				{
					uint8_t data = m_data_buffer[6 + pos];
					if (pos == m_read_data_size - 1)
					{
						LOGMASKED(LOG_DATA, "scsi_get_data: data transfer complete (%d bytes)\n", m_read_data_size);
						dump_buffer([this](const char *line) { LOGMASKED(LOG_DATA, "  %s\n", line); },
							m_data_buffer, m_data_buffer_size);
						m_read_phase = 0;
						m_read_data_size = 0;
						m_current_command = 0;
					}
					return data;
				}
			}
			return 0;
		}

		// Single-phase transfer (other commands)
		if (pos < m_data_buffer_size)
		{
			uint8_t data = m_data_buffer[pos];
			if (pos == m_data_buffer_size - 1)
			{
				LOGMASKED(LOG_DATA, "scsi_get_data: transfer complete (%d bytes)\n", m_data_buffer_size);
				dump_buffer([this](const char *line) { LOGMASKED(LOG_DATA, "  %s\n", line); },
					m_data_buffer, m_data_buffer_size);
			}
			return data;
		}
		return 0;
	}

	return nscsi_full_device::scsi_get_data(id, pos);
}

void nscsi_daynaport_device::scsi_put_data(int id, int pos, uint8_t data)
{
	if (id == 2)
	{
		// Accumulate data
		if (m_tx_size < MAX_PACKET_SIZE)
		{
			m_tx_buffer[m_tx_size++] = data;
		}

		// Check if we've received all expected data
		if (m_tx_size >= m_data_buffer_size && m_data_buffer_size > 0)
		{
			LOGMASKED(LOG_DATA, "scsi_put_data: transfer complete (%d bytes)\n", m_tx_size);
			dump_buffer([this](const char *line) { LOGMASKED(LOG_DATA, "  %s\n", line); },
				m_tx_buffer, m_tx_size);
			if (m_current_command == DP_CMD_WRITE)
			{
				// Check if this is padded format (indicated by command byte 5 bit 7)
				bool padded = (scsi_cmdbuf[5] & 0x80) != 0;

				int pkt_offset = 0;
				int pkt_len = m_tx_size;

				if (padded)
				{
					// Padded format: [2-byte length][2-byte flags][packet data][padding]
					// Length field contains the actual Ethernet frame size
					// Padding fills to the allocation length specified in CDB
					if (m_tx_size >= 4)
					{
						pkt_len = get_u16be(&m_tx_buffer[0]);
						pkt_offset = 4;

						LOGMASKED(LOG_DATA, "padded format: embedded_len=%d total_received=%d\n", pkt_len, m_tx_size);

						// Sanity check: packet data must fit within received data
						if (pkt_offset + pkt_len > m_tx_size)
						{
							LOGMASKED(LOG_DATA, "scsi_put_data: embedded length %d exceeds data size %d\n", pkt_len, m_tx_size - pkt_offset);
							pkt_len = m_tx_size - pkt_offset;
						}
					}
					else
					{
						LOGMASKED(LOG_DATA, "scsi_put_data: padded packet too short (%d bytes)\n", m_tx_size);
						m_tx_size = 0;
						m_current_command = 0;
						return;
					}
				}

				LOGMASKED(LOG_DATA, "scsi_put_data: WRITE complete, sending %d byte packet (enabled=%d)\n", pkt_len, m_interface_enabled);

				if (pkt_len > 0 && m_interface_enabled)
				{
					int result = send(&m_tx_buffer[pkt_offset], pkt_len);
					LOGMASKED(LOG_NETWORK, "send() returned %d\n", result);
					m_tx_count++;
				}
			}
			else if (m_current_command == DP_CMD_SET_MCAST)
			{
				// Multicast data received - just accept and ignore for now
				LOGMASKED(LOG_DATA, "scsi_put_data: received %d bytes of multicast data\n", m_tx_size);
			}

			m_tx_size = 0;
			m_current_command = 0;
		}
		return;
	}

	nscsi_full_device::scsi_put_data(id, pos, data);
}
