// license:BSD-3-Clause
// copyright-holders:Erik Hemming
#ifndef MAME_BUS_NSCSI_DAYNAPORT_H
#define MAME_BUS_NSCSI_DAYNAPORT_H

#pragma once

#include "machine/nscsi_bus.h"
#include "dinetwork.h"

class nscsi_daynaport_device : public nscsi_full_device, public device_network_interface
{
public:
	nscsi_daynaport_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);

protected:
	virtual void device_start() override ATTR_COLD;
	virtual void device_reset() override ATTR_COLD;

	// NSCSI interface
	virtual void scsi_command() override;
	virtual uint8_t scsi_get_data(int id, int pos) override;
	virtual void scsi_put_data(int id, int pos, uint8_t data) override;

	// Network interface callbacks
	virtual int recv_start_cb(u8 *buf, int len) override;
	virtual void recv_complete_cb(int result) override;
	virtual void send_complete_cb(int result) override;

private:
	// DaynaPORT-specific SCSI commands
	static constexpr uint8_t DP_CMD_READ           = 0x08;
	static constexpr uint8_t DP_CMD_GET_STATS      = 0x09;
	static constexpr uint8_t DP_CMD_WRITE          = 0x0a;
	static constexpr uint8_t DP_CMD_SET_IFACE_MODE = 0x0c;
	static constexpr uint8_t DP_CMD_SET_MCAST      = 0x0d;
	static constexpr uint8_t DP_CMD_ENABLE_IFACE   = 0x0e;

	// Command handlers
	void do_read();
	void do_write();
	void do_get_stats();
	void do_set_interface_mode();
	void do_set_multicast();
	void do_enable_interface();
	void do_inquiry();

	// Packet queue helpers
	bool rx_queue_empty() const { return m_rx_queue_head == m_rx_queue_tail; }
	bool rx_queue_full() const { return ((m_rx_queue_tail + 1) % PACKET_QUEUE_SIZE) == m_rx_queue_head; }
	int rx_queue_count() const;

	// Packet queue (incoming packets from network)
	static constexpr int PACKET_QUEUE_SIZE = 128;
	static constexpr int MAX_PACKET_SIZE = 1522;  // ETH_FRAME_LEN + CRC + VLAN

	struct Packet
	{
		uint8_t data[MAX_PACKET_SIZE];
		int size;
	};

	Packet m_rx_queue[PACKET_QUEUE_SIZE];
	int m_rx_queue_head;
	int m_rx_queue_tail;

	// Transmit buffer (for receiving data from SCSI host)
	uint8_t m_tx_buffer[MAX_PACKET_SIZE];
	int m_tx_size;

	// State
	uint8_t m_mac_address[6];
	bool m_interface_enabled;
	uint8_t m_current_command;  // Track current command for data-out handling

	// Statistics
	uint32_t m_rx_count;
	uint32_t m_tx_count;

	// Transfer buffer for SCSI data-in phases
	static constexpr int DATA_BUFFER_SIZE = 2048;
	uint8_t m_data_buffer[DATA_BUFFER_SIZE];
	int m_data_buffer_size;

	// Two-phase read state (Mac driver needs header and data as separate transfers)
	int m_read_phase;      // 0 = header (6 bytes), 1 = packet data
	int m_read_data_size;  // Size of packet data for phase 1
};

DECLARE_DEVICE_TYPE(NSCSI_DAYNAPORT, nscsi_daynaport_device)

#endif // MAME_BUS_NSCSI_DAYNAPORT_H
