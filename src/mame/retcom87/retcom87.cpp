#include "emu.h"

#include "bus/sms_ctrl/controllers.h"
#include "bus/sms_ctrl/smsctrl.h"
#include "cpu/g65816/g65816.h"
#include "sound/ay8910.h"
#include "video/tms9928a.h"

#include "speaker.h"

namespace
{

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
	{ }

	void init() {}
	void retcom87(machine_config &config);

private:
	required_device<g65265_device> m_maincpu;
	required_device<ym2149_device> m_ymsnd_0;
	required_device<ym2149_device> m_ymsnd_1;
	required_device<tms9918_device> m_vdp;
	required_device_array<sms_control_port_device, 2> m_md_ctrl_ports;

	void main_memmap(address_map &map);

	void pd5_write(u8 data);
};

void retcom87_state::retcom87(machine_config &config)
{
	G65265(config, m_maincpu, XTAL(3'686'400));
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

	// controllers
	for (auto &port : m_md_ctrl_ports) {
		SMS_CONTROL_PORT(config, port, sms_control_port_devices, SMS_CTRL_OPTION_MD_PAD);
	}

	m_maincpu->out_pd5_cb().set(FUNC(retcom87_state::pd5_write));
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
	// DF14: Data Send for YM2149 soundchip #2
	// DF15: Register Select for YM2149 soundchip #2
	// referenced src/mame/bandai/sv8000.cpp, src/mame/atari/atarist.cpp
	map(0xdf10, 0xdf10).w(m_ymsnd_0, FUNC(ay8910_device::data_w));
	map(0xdf11, 0xdf11).w(m_ymsnd_0, FUNC(ay8910_device::address_w));
	map(0xdf14, 0xdf14).w(m_ymsnd_1, FUNC(ay8910_device::data_w));
	map(0xdf15, 0xdf15).w(m_ymsnd_1, FUNC(ay8910_device::address_w));

	// controllers
	// DF00-DF03: Controller 1 through 4 inputs (aliased to DF04-DF07)
	// When Controller Select Pin output (P51, pin 4, J4-P5x connector) is 1: [C B C B Right Left Down Up]
	// When Controller Select Pin output (P51, pin 4, J4-P5x connector) is 0: [Start A Start A 0 0 Down Up]
	map(0xdf00, 0xdf00).r(m_md_ctrl_ports[0], FUNC(sms_control_port_device::in_r));
	map(0xdf01, 0xdf01).r(m_md_ctrl_ports[1], FUNC(sms_control_port_device::in_r));
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

// HACK: for now, uncomment the ROM_LOAD line for the corresponding program to run
// need to figure out a better way to handle roms

// display test
ROM_LOAD("textdemo.bin", 0x8000, 0x8000, CRC(4cf363dc) SHA1(bed707ec2ebb3e6cddfc6db58d78e436af05961a))

// sound test
// ROM_LOAD("soundtest.bin", 0x8000, 0x8000, CRC(7a828be3) SHA1(3b6487dbec7407e628b900877aae976f706a4d51))

// monitor rom test
// ROM_LOAD("monitor.bin", 0xE000, 0x2000, CRC(9575d641) SHA1(56ca218c0ed3d8fd631ee03690c0815b1441d0d4))

ROM_END

} // namespace

COMP(2023, retcom87, 0, 0, retcom87, retcom87_inputs, retcom87_state, init, "Lantertronics", "RetCom87", MACHINE_NOT_WORKING)
