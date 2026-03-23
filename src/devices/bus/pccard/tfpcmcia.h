// license:BSD-3-Clause
// copyright-holders: Erik Hemming
/***************************************************************************

    TerribleFire PCMCIA - CH32V307 MCU + SD card + 4MB SPIRAM

    Custom PCMCIA board for Amiga 1200 with:
    - 4MB SPIRAM (common memory space)
    - SPI SD card interface (I/O space registers)
    - Boot ROM (attribute memory for XIP, common memory switchable with SPIRAM)

***************************************************************************/

#ifndef MAME_BUS_PCCARD_TFPCMCIA_H
#define MAME_BUS_PCCARD_TFPCMCIA_H

#pragma once

#include "pccard.h"

class spi_sdcard_device;

class pccard_tfpcmcia_device :
	public device_t,
	public device_pccard_interface
{
public:
	pccard_tfpcmcia_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock = 0);

	virtual uint16_t read_memory(offs_t offset, uint16_t mem_mask = ~0) override;
	virtual uint16_t read_reg(offs_t offset, uint16_t mem_mask = ~0) override;
	virtual uint16_t read_io(offs_t offset, uint16_t mem_mask = ~0) override;
	virtual void write_memory(offs_t offset, uint16_t data, uint16_t mem_mask = ~0) override;
	virtual void write_reg(offs_t offset, uint16_t data, uint16_t mem_mask = ~0) override;
	virtual void write_io(offs_t offset, uint16_t data, uint16_t mem_mask = ~0) override;

protected:
	virtual void device_start() override ATTR_COLD;
	virtual void device_reset() override ATTR_COLD;
	virtual void device_add_mconfig(machine_config &config) override ATTR_COLD;
	virtual const tiny_rom_entry *device_rom_region() const override ATTR_COLD;

private:
	static constexpr u32 RAM_SIZE = 0x400000; // 4MB SPIRAM

	// I/O space register offsets
	static constexpr offs_t REG_SPI_DATA    = 0x100;
	static constexpr offs_t REG_SPI_CS      = 0x101;
	static constexpr offs_t REG_SPI_STATUS  = 0x102;
	static constexpr offs_t REG_BOARD_CTRL  = 0x103;
	static constexpr offs_t REG_BOARD_ID    = 0x104;

	static constexpr u8 BOARD_ID_VALUE = 0x01;

	// BOARD_CTRL bits
	static constexpr u8 BOARD_CTRL_XIP = 0x01; // bit 0: 0=DIAG CIS/bootrom, 1=XIP CIS/SPIRAM

	void spi_miso_w(int state);
	u8 spi_transfer_byte(u8 data);

	required_device<spi_sdcard_device> m_sdcard;
	required_memory_region m_bootrom;

	std::unique_ptr<u8[]> m_ram;
	std::vector<u8> m_cis_diag;
	std::vector<u8> m_cis_xip;
	u8 m_spi_data;
	u8 m_spi_cs;
	u8 m_board_ctrl;
	u8 m_miso_bit;
};

DECLARE_DEVICE_TYPE(PCCARD_TFPCMCIA, pccard_tfpcmcia_device)

#endif // MAME_BUS_PCCARD_TFPCMCIA_H
