#include "emu.h"

// Includes from src/emu/
#include "screen.h"
#include "speaker.h"

// Includes from src/devices/
#include "bus/sms_ctrl/controllers.h"
#include "bus/sms_ctrl/smsctrl.h"
#include "cpu/g65816/g65816.h"
#include "imagedev/cartrom.h"
#include "machine/pckeybrd.h"
#include "sound/ay8910.h"
#include "video/tms9928a.h"

// Standard library includes
#include <deque>


// --------------------------------------------------------------------------
// Flash ROM which can be attached to a W65C265SXB, as found in the RetCom87.
// The W65C265SXB monitor will automatically run code in this ROM if it is
// present and starts with the ASCII bytes "WDC". It can be provided to the
// emulator as a separate file via the -rom flag. ROMs loaded this way must
// start with the bytes "WDC" at logical address 0x8000 and with the code to
// execute starting at 0x8004.
// --------------------------------------------------------------------------

DECLARE_DEVICE_TYPE(RETCOM87_FLASH_ROM, retcom87_flash_rom_device)

class retcom87_flash_rom_device : public device_t,
                                  public device_rom_image_interface
{
public:
	retcom87_flash_rom_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
		: device_t(mconfig, RETCOM87_FLASH_ROM, tag, owner, clock)
		, device_rom_image_interface(mconfig, *this)
	{ }

	bool is_reset_on_load() const noexcept override { return true; }
	const char *file_extensions() const noexcept override { return "bin"; }

	std::pair<std::error_condition, std::string> call_load() override
	{
		// Read the ROM file's data into machine memory at 0x8000 to 0xDEFF.
		fread(machine().memory().region_find(":maincpu")->base() + 0x8000, 0x5F00);

		// Return defaults, indicating no error.
		return std::make_pair(std::error_condition(), std::string());
	}

protected:
	void device_start() override ATTR_COLD {}
};

DEFINE_DEVICE_TYPE(RETCOM87_FLASH_ROM, retcom87_flash_rom_device, "retcom87_flash_rom", "RetCom87 Flash ROM")


namespace
{

// ----------------------
// RetCom87 Driver class.
// ----------------------

class retcom87_state : public driver_device
{
public:
	retcom87_state(const machine_config &mconfig, device_type type, const char *tag)
			: driver_device(mconfig, type, tag)
			, m_maincpu(*this, "maincpu")
			, m_ymsnd_0(*this, "ym2149_0")
			, m_ymsnd_1(*this, "ym2149_1")
			, m_vdp(*this, "tms9918")
			, m_md_ctrl_ports(*this, { "md_ctrl_0", "md_ctrl_1" })
			, m_flash_rom(*this, "flash_rom")
			, m_kbd(*this, "keyboard")
	{ }

	void retcom87(machine_config &config);

protected:
	void device_start() override ATTR_COLD
	{
		driver_device::device_start();
		m_keyboard_timer = timer_alloc(FUNC(retcom87_state::keyboard_tick), this);
	}

private:
	required_device<g65265_device> m_maincpu;
	required_device<ym2149_device> m_ymsnd_0;
	required_device<ym2149_device> m_ymsnd_1;
	required_device<tms9918_device> m_vdp;
	required_device_array<sms_control_port_device, 2> m_md_ctrl_ports;
	required_device<retcom87_flash_rom_device> m_flash_rom;
	required_device<at_keyboard_device> m_kbd;

	// Pretend we have a keyboard running at 16 kHz.
	static constexpr u32 m_keyboard_frequency = 1 << 14;
	emu_timer *m_keyboard_timer;
	TIMER_CALLBACK_MEMBER(keyboard_tick);
	std::deque<u8> m_keyboard_data_queue;
	u8 m_keyboard_data = 0;

	void main_memmap(address_map &map);

	void vdp_interrupt(int data);
	void keypress(int data);

	u8 pd4_read();
	void pd5_write(u8 data);
};

void retcom87_state::retcom87(machine_config &config)
{
	G65265(config, m_maincpu, XTAL(32'768), XTAL(3'686'400));
	m_maincpu->set_addrmap(AS_PROGRAM, &retcom87_state::main_memmap);

	// sound chip
	YM2149(config, m_ymsnd_0, XTAL(1'843'200));
	YM2149(config, m_ymsnd_1, XTAL(1'843'200));
	m_ymsnd_0->set_flags(AY8910_SINGLE_OUTPUT);
	m_ymsnd_1->set_flags(AY8910_SINGLE_OUTPUT);

	// define speaker output
	SPEAKER(config, "speaker", 2).front();
	m_ymsnd_0->add_route(0, "speaker", 1.0, 0);
	m_ymsnd_1->add_route(0, "speaker", 1.0, 1);

	// display chip
	// referenced colecovision which uses TMS9928A: src/mame/coleco/coleco.cpp
	// 10.738633MHz clock frequency and 16K vram
	TMS9918(config, m_vdp, XTAL(10'738'635));
	m_vdp->set_vram_size(0x4000);

	// define screen output
	SCREEN(config, "screen", SCREEN_TYPE_RASTER);
	m_vdp->set_screen("screen");

	// display chip interrupt
	m_vdp->int_callback().set(FUNC(retcom87_state::vdp_interrupt));

	// controllers
	for (auto &port : m_md_ctrl_ports) {
		SMS_CONTROL_PORT(config, port, sms_control_port_devices, SMS_CTRL_OPTION_MD_PAD);
	}

	m_maincpu->out_pd5_cb().set(FUNC(retcom87_state::pd5_write));

	// flash rom
	RETCOM87_FLASH_ROM(config, m_flash_rom, XTAL(3'686'400));

	// keyboard
	AT_KEYB(config, m_kbd, at_keyboard_device::KEYBOARD_TYPE::AT, /*default_set=*/2);
	m_kbd->keypress().set(FUNC(retcom87_state::keypress));

	m_maincpu->in_pd4_cb().set(FUNC(retcom87_state::pd4_read));
}

// see MAME docs on memory: https://docs.mamedev.org/techspecs/memory.html
//
// see RetCom87 docs on memory map:
// https://github.com/lantertronics/RetCom87-hardware/wiki/RetCom87-Memory-Map
void retcom87_state::main_memmap(address_map &map)
{
	// 32kB SRAM
	map(0x0000, 0x7FFF).ram();

	// 32kB flash memory
	// some of this range is used for I/O (below)
	map(0x8000, 0xffff).rom();

	// I/O

	// display
	// DFC0: TMS9118 VRAM Access (aliased to DFC2, DFC4, DFC6) (not $C000 like in datasheet)
	// DFC1: TMS9118 Register Access (aliased to DFC3, DFC5, DFC7) (not $C002 like in datasheet)
	// referenced Tomy Tutor(?)g: src/mame/tomy/tutor.cpp
	// but referencing it as a device instead of tag for consistency
	map(0xdfc0, 0xdfc0).rw(m_vdp, FUNC(tms9918_device::vram_read), FUNC(tms9918_device::vram_write));         /*VDP data*/
	map(0xdfc1, 0xdfc1).rw(m_vdp, FUNC(tms9918_device::register_read), FUNC(tms9918_device::register_write)); /*VDP status*/

	// sound
	// DF10: Data Send for YM2149 soundchip #1
	// DF11: Register Select for YM2149 soundchip #1
	// DF12: Data Send for YM2149 soundchip #2
	// DF13: Register Select for YM2149 soundchip #2
	// referenced src/mame/bandai/sv8000.cpp, src/mame/atari/atarist.cpp
	map(0xdf10, 0xdf10).w(m_ymsnd_0, FUNC(ay8910_device::data_w));
	map(0xdf11, 0xdf11).w(m_ymsnd_0, FUNC(ay8910_device::address_w));
	map(0xdf12, 0xdf12).w(m_ymsnd_1, FUNC(ay8910_device::data_w));
	map(0xdf13, 0xdf13).w(m_ymsnd_1, FUNC(ay8910_device::address_w));

	// controllers
	// DF00-DF03: Controller 1 through 4 inputs (aliased to DF04-DF07)
	// When Controller Select Pin output (P51, pin 4, J4-P5x connector) is 1: [C B C B Right Left Down Up]
	// When Controller Select Pin output (P51, pin 4, J4-P5x connector) is 0: [Start A Start A 0 0 Down Up]
	map(0xdf00, 0xdf00).r(m_md_ctrl_ports[0], FUNC(sms_control_port_device::in_r));
	map(0xdf01, 0xdf01).r(m_md_ctrl_ports[1], FUNC(sms_control_port_device::in_r));
}

void retcom87_state::vdp_interrupt(int data)
{
	// Display chip interrupt output is wired to the IRQB pin (P41)
	m_maincpu->g65816_set_reg(g65816_device::G65816_IRQ_STATE, data);
}

void retcom87_state::keypress(int data)
{
	u8 chr = m_kbd->read();

	m_keyboard_data_queue.push_back(0);

	u8 parity = 1;
	for (int i = 0; i < 8; ++i)
	{
		m_keyboard_data_queue.push_back(BIT(chr, i));
		parity ^= BIT(chr, i);
	}

	m_keyboard_data_queue.push_back(parity);
	m_keyboard_data_queue.push_back(1);

	if (!m_keyboard_timer->enabled())
	{
		attotime duration = attotime::from_hz(m_keyboard_frequency);
		m_keyboard_timer->adjust(duration, 0);
	}
}

TIMER_CALLBACK_MEMBER(retcom87_state::keyboard_tick)
{
	int nmi_signal = param;
	if (nmi_signal)
	{
		m_keyboard_data = m_keyboard_data_queue.front();
		m_keyboard_data_queue.pop_front();
	}

	m_maincpu->g65816_set_reg(g65816_device::G65816_NMI_STATE, nmi_signal);

	if (!m_keyboard_data_queue.empty())
	{
		attotime duration = attotime::from_hz(m_keyboard_frequency);
		m_keyboard_timer->adjust(duration, !nmi_signal);
	}
}

u8 retcom87_state::pd4_read()
{
	// Port 4 bit 2 (P42) maps to keyboard data bit
	return BIT(m_keyboard_data, 0) << 2;
}

// Write to port 5 data register
void retcom87_state::pd5_write(u8 data)
{
	// Port 5 bit 1 (P51) maps to bit 6 of controller input
	data = BIT(data, 1) << 6;
	for (auto &ctrl_port : m_md_ctrl_ports) {
		ctrl_port->out_w(data, /*mask=*/0x40);
	}
}

INPUT_PORTS_START(retcom87_inputs)
INPUT_PORTS_END

ROM_START(retcom87)
ROM_REGION(0x10000, "maincpu", 0)

// Monitor rom
ROM_LOAD("monitor.bin", 0xE000, 0x2000, CRC(9575d641) SHA1(56ca218c0ed3d8fd631ee03690c0815b1441d0d4))

ROM_END

} // namespace


// ------------------------------
// RetCom87 emulator declaration.
// ------------------------------

COMP(2023, retcom87, 0, 0, retcom87, retcom87_inputs, retcom87_state, empty_init, "Lantertronics", "RetCom87", MACHINE_NOT_WORKING)
