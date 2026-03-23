// license:BSD-3-Clause
// copyright-holders: Erik Hemming
/***************************************************************************

    TerribleFire PCMCIA - CH32V307 MCU + SD card + 4MB SPIRAM

    Behavioral emulation of a custom PCMCIA board for Amiga 1200.

    Common memory ($600000-$9FFFFF):
      - Boot ROM mode (board_ctrl bit 0 = 0): reads return boot ROM data
      - SPIRAM mode (board_ctrl bit 0 = 1): 4MB read/write RAM

    Attribute memory ($A00000+):
      $000-$0FF  CIS tuples (read-only)
      $100+      Boot ROM (read-only, for XIP RomTag)

    I/O space registers ($A20000+):
      $100       SPI_DATA    - SPI byte transfer (R/W)
      $101       SPI_CS      - SD card chip select, active low (R/W)
      $102       SPI_STATUS  - bit 0: SD card present (R)
      $103       BOARD_CTRL  - bit 0: 0=bootrom, 1=spiram (R/W)
      $104       BOARD_ID    - board version (R)

***************************************************************************/

#include "emu.h"
#include "tfpcmcia.h"

#include "machine/spi_sdcard.h"

#define LOG_SPI      (1U << 1)
#define LOG_MEMORY   (1U << 2)
#define LOG_REG      (1U << 3)

//#define VERBOSE (LOG_GENERAL | LOG_SPI | LOG_MEMORY | LOG_REG)
#define VERBOSE (LOG_GENERAL)
#include "logmacro.h"


DEFINE_DEVICE_TYPE(PCCARD_TFPCMCIA, pccard_tfpcmcia_device, "tfpcmcia", "TerribleFire PCMCIA (SD + 4MB SPIRAM)")


pccard_tfpcmcia_device::pccard_tfpcmcia_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock) :
	device_t(mconfig, PCCARD_TFPCMCIA, tag, owner, clock),
	device_pccard_interface(mconfig, *this),
	m_sdcard(*this, "sdcard"),
	m_bootrom(*this, "bootrom"),
	m_spi_data(0xff),
	m_spi_cs(0xff),
	m_board_ctrl(0),
	m_miso_bit(1)
{
}


void pccard_tfpcmcia_device::device_start()
{
	m_ram = std::make_unique<u8[]>(RAM_SIZE);

	save_item(NAME(m_spi_data));
	save_item(NAME(m_spi_cs));
	save_item(NAME(m_board_ctrl));
	save_item(NAME(m_miso_bit));
	save_pointer(NAME(m_ram), RAM_SIZE);

	// assert card detect lines (active low = card present)
	m_cd1_cb(0);
	m_cd2_cb(0);
}


void pccard_tfpcmcia_device::device_reset()
{
	m_spi_data = 0xff;
	m_spi_cs = 0xff;
	m_board_ctrl = 0;
	m_miso_bit = 1;

	std::fill_n(m_ram.get(), RAM_SIZE, 0x00);

	// deassert SD card chip select
	m_sdcard->spi_ss_w(0);

	// build DIAG CIS — coldstart expects CISTPL_AMIGAXIP at offset 0
	m_cis_diag.clear();
	m_cis_diag.resize(256, 0xff);

	int i = 0;

	// CISTPL_AMIGAXIP (0x91) - DIAG mode
	m_cis_diag[i++] = 0x91; // tuple code
	m_cis_diag[i++] = 0x05; // link (5 bytes)
	m_cis_diag[i++] = 0x23; // flag: DIAG mode
	m_cis_diag[i++] = 0x00; // offset byte 0 (little-endian $00400200)
	m_cis_diag[i++] = 0x02; // offset byte 1
	m_cis_diag[i++] = 0x40; // offset byte 2
	m_cis_diag[i++] = 0x00; // offset byte 3

	// CISTPL_END (0xFF)
	m_cis_diag[i++] = 0xff;

	// build XIP CIS — full tuple chain for card.resource / IfAmigaXIP()
	m_cis_xip.clear();
	m_cis_xip.resize(256, 0xff);

	i = 0;

	// CISTPL_DEVICE (0x01) - device information
	m_cis_xip[i++] = 0x01; // tuple code
	m_cis_xip[i++] = 0x03; // link (3 bytes)
	m_cis_xip[i++] = 0xd1; // device type: SRAM(0xd), no WPS, speed=250ns
	m_cis_xip[i++] = 0x27; // device size: 4MB (size code 0x27 = 4M)
	m_cis_xip[i++] = 0xff; // end of device chain

	// CISTPL_VERS_1 (0x15) - level 1 version/product info
	m_cis_xip[i++] = 0x15; // tuple code

	int linkPos = i++;  // save position for link byte

	int bodyStart = i;
	m_cis_xip[i++] = 0x04; // major version 4
	m_cis_xip[i++] = 0x01; // minor version 1

	static const char manufacturer[] = "TerribleFire";
	for (const char* p = manufacturer; *p; p++)
		m_cis_xip[i++] = *p;
	m_cis_xip[i++] = 0x00;

	static const char product[] = "PCMCIA SD+RAM";
	for (const char* p = product; *p; p++)
		m_cis_xip[i++] = *p;
	m_cis_xip[i++] = 0x00;

	static const char version[] = "1.0";
	for (const char* p = version; *p; p++)
		m_cis_xip[i++] = *p;
	m_cis_xip[i++] = 0x00;

	m_cis_xip[i++] = 0xff; // end of strings

	m_cis_xip[linkPos] = i - bodyStart; // fill in link

	// CISTPL_FUNCID (0x21) - function identification
	m_cis_xip[i++] = 0x21; // tuple code
	m_cis_xip[i++] = 0x02; // link (2 bytes)
	m_cis_xip[i++] = 0x01; // memory card function
	m_cis_xip[i++] = 0x00; // system initialization byte

	// CISTPL_AMIGAXIP (0x91) - Amiga Execute-In-Place
	// TP_XIPLOC = $400204 → Kickstart adds $600000 → RomTag at $A00204
	m_cis_xip[i++] = 0x91; // tuple code
	m_cis_xip[i++] = 0x06; // link (6 bytes)
	m_cis_xip[i++] = 0x04; // TP_XIPLOC byte 0 (little-endian $00400204)
	m_cis_xip[i++] = 0x02; // TP_XIPLOC byte 1
	m_cis_xip[i++] = 0x40; // TP_XIPLOC byte 2
	m_cis_xip[i++] = 0x00; // TP_XIPLOC byte 3
	m_cis_xip[i++] = 0x01; // TP_XIPFLAGS: AUTORUN
	m_cis_xip[i++] = 0x00; // TP_XIPRESRV: reserved

	// CISTPL_END (0xFF)
	m_cis_xip[i++] = 0xff;
}


//**************************************************************************
//  COMMON MEMORY SPACE
//**************************************************************************

uint16_t pccard_tfpcmcia_device::read_memory(offs_t offset, uint16_t mem_mask)
{
	u32 addr = offset * 2;
	if (addr + 1 < RAM_SIZE)
	{
		u16 data = m_ram[addr] | (m_ram[addr + 1] << 8);
		LOGMASKED(LOG_MEMORY, "spiram read: %06x = %04x\n", addr, data);
		return data;
	}
	return 0xffff;
}


void pccard_tfpcmcia_device::write_memory(offs_t offset, uint16_t data, uint16_t mem_mask)
{
	u32 addr = offset * 2;
	if (addr + 1 < RAM_SIZE)
	{
		if (ACCESSING_BITS_0_7)
			m_ram[addr] = data & 0xff;
		if (ACCESSING_BITS_8_15)
			m_ram[addr + 1] = (data >> 8) & 0xff;
		LOGMASKED(LOG_MEMORY, "spiram write: %06x = %04x & %04x\n", addr, data, mem_mask);
	}
}


//**************************************************************************
//  ATTRIBUTE MEMORY SPACE
//**************************************************************************

uint16_t pccard_tfpcmcia_device::read_reg(offs_t offset, uint16_t mem_mask)
{
	if (offset < 0x100)
	{
		// CIS tuples — select DIAG or XIP based on BOARD_CTRL
		const auto& cis = (m_board_ctrl & BOARD_CTRL_XIP) ? m_cis_xip : m_cis_diag;
		if (offset < cis.size())
			return cis[offset];
		return 0xff;
	}

	// boot ROM exposed in attribute memory for XIP RomTag
	u32 romoff = (offset - 0x100) * 2;
	u8* rom = m_bootrom->base();
	u32 romSize = m_bootrom->bytes();
	if (romoff + 1 < romSize)
	{
		u16 data = rom[romoff] | (rom[romoff + 1] << 8);
		LOGMASKED(LOG_MEMORY, "attr bootrom read: offset %06x = %04x\n", romoff, data);
		return data;
	}
	return 0xffff;
}


void pccard_tfpcmcia_device::write_reg(offs_t offset, uint16_t data, uint16_t mem_mask)
{
	// attribute memory is read-only (CIS + boot ROM)
}


//**************************************************************************
//  I/O SPACE
//**************************************************************************

uint16_t pccard_tfpcmcia_device::read_io(offs_t offset, uint16_t mem_mask)
{
	switch (offset)
	{
	case REG_SPI_DATA:
		LOGMASKED(LOG_REG, "SPI_DATA read: %02x\n", m_spi_data);
		return m_spi_data;

	case REG_SPI_CS:
		return m_spi_cs;

	case REG_SPI_STATUS:
	{
		u8 status = m_sdcard->get_card_present() ? 0x01 : 0x00;
		LOGMASKED(LOG_REG, "SPI_STATUS read: %02x\n", status);
		return status;
	}

	case REG_BOARD_CTRL:
		return m_board_ctrl;

	case REG_BOARD_ID:
		return BOARD_ID_VALUE;

	default:
		return 0xffff;
	}
}


void pccard_tfpcmcia_device::write_io(offs_t offset, uint16_t data, uint16_t mem_mask)
{
	switch (offset)
	{
	case REG_SPI_DATA:
		LOGMASKED(LOG_SPI, "SPI_DATA write: %02x\n", data & 0xff);
		m_spi_data = spi_transfer_byte(data & 0xff);
		LOGMASKED(LOG_SPI, "SPI_DATA result: %02x\n", m_spi_data);
		break;

	case REG_SPI_CS:
		m_spi_cs = data & 0xff;
		// spi_sdcard_device uses 1=selected, 0=deselected
		// our register is active-low: bit 0 = 0 means selected
		m_sdcard->spi_ss_w((m_spi_cs & 0x01) ? 0 : 1);
		LOGMASKED(LOG_REG, "SPI_CS write: %02x (sd %s)\n", m_spi_cs, (m_spi_cs & 0x01) ? "deselected" : "selected");
		break;

	case REG_BOARD_CTRL:
		m_board_ctrl = data & 0xff;
		LOG("BOARD_CTRL write: %02x (%s)\n", m_board_ctrl, (m_board_ctrl & BOARD_CTRL_XIP) ? "XIP/SPIRAM" : "DIAG/bootrom");
		break;

	default:
		break;
	}
}


//**************************************************************************
//  SPI INTERFACE
//**************************************************************************

void pccard_tfpcmcia_device::spi_miso_w(int state)
{
	m_miso_bit = state;
}


u8 pccard_tfpcmcia_device::spi_transfer_byte(u8 data)
{
	u8 result = 0;

	for (int bit = 7; bit >= 0; bit--)
	{
		// set MOSI
		m_sdcard->spi_mosi_w(BIT(data, bit));

		// rising edge: SD card latches MOSI, master samples MISO
		m_sdcard->spi_clock_w(1);

		// capture MISO bit (shifted out on previous falling edge)
		result = (result << 1) | m_miso_bit;

		// falling edge: SD card shifts out next MISO bit
		m_sdcard->spi_clock_w(0);
	}

	return result;
}


//**************************************************************************
//  MACHINE CONFIGURATION
//**************************************************************************

void pccard_tfpcmcia_device::device_add_mconfig(machine_config &config)
{
	SPI_SDCARD(config, m_sdcard, 0);
	m_sdcard->spi_miso_callback().set(FUNC(pccard_tfpcmcia_device::spi_miso_w));
}


ROM_START(tfpcmcia)
	ROM_REGION(0x1fe00, "bootrom", ROMREGION_ERASEFF)
	// boot ROM served via attribute memory for XIP (128KB minus $200 for CIS)
	ROM_LOAD("tfpcmcia.rom", 0x0000, 0x1fe00, BAD_DUMP CRC(00000000) SHA1(0000000000000000000000000000000000000000))
ROM_END

const tiny_rom_entry *pccard_tfpcmcia_device::device_rom_region() const
{
	return ROM_NAME(tfpcmcia);
}
