// license:BSD-3-Clause
// copyright-holders:Joakim Larsson Edstrom
/*
 *
 * History of Didact
 *------------------
 * Didact Laromedelsproduktion was started in Linkoping in Sweden by Anders Andersson, Arne Kullbjer and
 * Lars Bjorklund. They constructed a series of microcomputers for educational purposes such as "Mikrodator 6802",
 * Esselte 100 and the Candela computer for the swedish schools to educate the students in assembly programming
 * and BASIC for electro mechanical applications such as stepper motors, simple process control, buttons
 * and LED:s. Didact designs were marketed by Esselte Studium to the swedish schools.
 *
 * The Esselte 1000 was an educational package based on Apple II plus software and litterature
 * but the relation to Didact is at this point unknown so it is probably a pure Esselte software production.
 *
 * Misc links about the boards supported by this driver.
 *-----------------------------------------------------
 * http://elektronikforumet.com/forum/viewtopic.php?f=11&t=51424
 * http://kilroy71.fastmail.fm/gallery/Miscellaneous/20120729_019.jpg
 * http://elektronikforumet.com/forum/download/file.php?id=63988&mode=view
 * http://elektronikforumet.com/forum/viewtopic.php?f=2&t=79576&start=150#p1203915
 *
 *  TODO:
 *  Didact designs:    mp68a, md6802, Modulab, Esselte 100
 * -------------------------------------------------------
 *  - Add PCB layouts   OK     OK                OK
 *  - Dump ROM:s,       OK     OK                rev2
 *  - Keyboard          OK     OK                rev2
 *  - Display/CRT       OK     OK                OK
 *  - Clickable Artwork RQ     RQ
 *  - Sound             NA     NA
 *  - Cassette i/f                               OK
 *  - Expansion bus
 *  - Expansion overlay
 *  - Interrupts        OK                       OK
 *  - Serial                   XX                XX
 *   XX = needs debug
 ****************************************************************************/

#include "emu.h"
#include "cpu/m6800/m6800.h" // For mp68a, md6802
#include "machine/6821pia.h" // For all boards
#include "machine/74145.h"   // For the md6802
#include "machine/timer.h"
#include "video/dm9368.h"    // For the mp68a

// Features
#include "imagedev/cassette.h"
#include "bus/rs232/rs232.h"
#include "screen.h"

// Generated artwork includes
#include "mp68a.lh"
#include "md6802.lh"

//**************************************************************************
//  MACROS / CONSTANTS
//**************************************************************************

#define LOG_SETUP   (1U <<  1)
#define LOG_SCAN    (1U <<  2)
#define LOG_BANK    (1U <<  3)
#define LOG_SCREEN  (1U <<  4)
#define LOG_READ    (1U <<  5)
#define LOG_CS      (1U <<  6)
#define LOG_PLA     (1U <<  7)
#define LOG_PROM    (1U <<  8)

//#define VERBOSE (LOG_READ | LOG_GENERAL | LOG_SETUP | LOG_PLA | LOG_BANK)
//#define LOG_OUTPUT_FUNC printf
#include "logmacro.h"

#define LOGSETUP(...)   LOGMASKED(LOG_SETUP,   __VA_ARGS__)
#define LOGSCAN(...)    LOGMASKED(LOG_SCAN,    __VA_ARGS__)
#define LOGBANK(...)    LOGMASKED(LOG_BANK,    __VA_ARGS__)
#define LOGSCREEN(...)  LOGMASKED(LOG_SCREEN,  __VA_ARGS__)
#define LOGR(...)       LOGMASKED(LOG_READ,    __VA_ARGS__)
#define LOGCS(...)      LOGMASKED(LOG_CS,      __VA_ARGS__)
#define LOGPLA(...)     LOGMASKED(LOG_PLA,     __VA_ARGS__)
#define LOGPROM(...)    LOGMASKED(LOG_PROM,    __VA_ARGS__)

#ifdef _MSC_VER
#define FUNCNAME __func__
#else
#define FUNCNAME __PRETTY_FUNCTION__
#endif

#define PIA1_TAG "pia1"
#define PIA2_TAG "pia2"
#define PIA3_TAG "pia3"
#define PIA4_TAG "pia4"

/* Didact base class */
class didact_state : public driver_device
{
	public:
	didact_state(const machine_config &mconfig, device_type type, const char * tag)
		: driver_device(mconfig, type, tag)
		, m_io_lines(*this, "LINE%u", 0U)
		, m_lines{ 0, 0, 0, 0 }
		, m_led(0)
		, m_rs232(*this, "rs232")
		, m_leds(*this, "led%u", 0U)
	{ }

	TIMER_DEVICE_CALLBACK_MEMBER(scan_artwork);

protected:
	virtual void machine_start() override { m_leds.resolve(); }

	required_ioport_array<5> m_io_lines;
	uint8_t m_lines[4];
	uint8_t m_reset;
	uint8_t m_shift;
	uint8_t m_led;
	optional_device<rs232_port_device> m_rs232;
	output_finder<2> m_leds;
};


/*  _____________________________________________________________________________________________   ___________________________________________________________________________
 * |The Didact Mikrodator 6802 CPU board by Lars Bjorklund 1983                            (  ) |  |The Didact Mikrodator 6802 TB16 board by Lars Bjorklund 1983               |
 * |                                                                                     +----= |  |             +-|||||||-+                                         ______    |
 * |                                                                                     |    = |  | CA2 Tx      |terminal |                                        |  ()  |   |
 * |                                                                                     |    = |  | PA7 Rx      +---------+               +----------+  C1nF,<=R18k|      |   |
 * |     Photo of CPU board mainly covered by TB16 Keypad/Display board                  +--- = |  | CA1 DTR               +-----------+   |          |   CB2->CB1  |  E   |   |
 * |                                                                                            |  |               PA4-PA6 |           | 1 | BCD      |    +----+   |  X   |   |
 * |                                                                                            |  |               ------->| 74LS145   |   | digit 5  |    |LS  |   |  P   |   |
 * |                                                                                            |  |                       +-----------+   |----------|    | 122|   |  A   |   |
 * |                                                                                     +-----=|  |                                   |   |          |    |    |   |  N   |   |
 * |                                                                          +-------+  |     =|  |------ +--------+                  | 2 | BCD      |    |    |   |  S   |   |
 * |                                                                          |       |  |     =|  | RES*  | SHIFT  |  LED( )          |   | digit 4  |    |    |   |  I   |   |
 * |                                                                          |       |  |     =|  |       |  '*'   |    CA2           v   |----------|    +----+   |  O   |   |
 * |                                                                          | 6821  |  |     =|  |   PA3 |PA7 PA2 | PA1      PA0         |          |        +----|  N   |   |
 * |                                                                          | PIA   |  |     =|  |----|--+-----|--+--|-----+--|---+    3 |          |    PB0-|LS  |      |   |
 * |                                                                          |       |  |     =|  |    v  |     v  |  v     |  v   |      | BCD      |     PB7| 244|  C   |   |
 * |                                                                          |       |  |     =|  | ADR   | RUN    | SST    | CON  | 1    | digit 3  |    --->|    |  O   |   |
 * |                                                                          |       |  |     =|  |  0    |  4     |  8     |  C   |      |----------|        |    |  N   |   |
 * |                                                                          |       |  |     =|  |-------+--------+--------+------+      |          |<-------|    |  N   |   |
 * |                                                                          |       |  |     =|  |       |        |        |      |    4 |          |        +----|  E   |   |
 * |                                                                          |       |  |     =|  | STA   | BPS    | USERV  |      | 2    | BCD      |             |  C   |   |
 * |                                                                          |       |  |     =|  |  1    |  5     |  9     |  D   |      | digit 2  |             |  T   |   |
 * |                                                                          |       |  |     =|  |-------+--------+--------+------+      |----------|             |  O   |   |
 * |                                                                          |       |  |     =|  |       |        |        |      |      |          |             |  R   |   |
 * |                                                                          |       |  |     =|  | EXF   | EXB    | MOV    | LOAD | 3  5 | BCD      |             |      |   |
 * |                                                                          |       |  |     =|  |  2    |  6     |  A     |  E   |      | digit 1  |             |      |   |
 * |                                                                          +-------+  |     =|  |-------+--------+--------+------+      |----------|             |      |   |
 * |                                                                                     |     =|  |       |        |        |      |      |          |             |      |   |
 * |                                                                                     +-----=|  | CLR   |  SP    | USERJ  | FLAG | 4  6 | BCD      |             |      |   |
 * |                                                                                            |  |  3    |  7     |  B     |  F   |      | digit 0  |             |  ()  |   |
 * |                                                                                            |  |-------+--------+--------+------+      +----------+             +------+   |
 * |                                                                                            |  |                                                                           |
 * |                                                                                            |  |                                                                           |
 * |____________________________________________________________________________________________|  |___________________________________________________________________________|
 */

/* Mikrodator 6802 driver class */
class md6802_state : public didact_state
{
public:
	md6802_state(const machine_config &mconfig, device_type type, const char * tag)
		: didact_state(mconfig, type, tag)
		, m_maincpu(*this, "maincpu")
		, m_tb16_74145(*this, "tb16_74145")
		, m_pia1(*this, PIA1_TAG)
		, m_pia2(*this, PIA2_TAG)
		, m_7segs(*this, "digit%u", 0U)
		, m_segments(0)
	{ }

	void md6802(machine_config &config);

protected:
	DECLARE_READ8_MEMBER( pia2_kbA_r );
	DECLARE_WRITE8_MEMBER( pia2_kbA_w );
	DECLARE_READ8_MEMBER( pia2_kbB_r );
	DECLARE_WRITE8_MEMBER( pia2_kbB_w );
	DECLARE_WRITE_LINE_MEMBER( pia2_ca2_w);

	virtual void machine_reset() override;
	virtual void machine_start() override;

	void md6802_map(address_map &map);

private:
	required_device<m6802_cpu_device> m_maincpu;
	required_device<ttl74145_device> m_tb16_74145;
	required_device<pia6821_device> m_pia1;
	required_device<pia6821_device> m_pia2;
	output_finder<6> m_7segs;
	uint8_t m_segments;
};

/* Keyboard */
READ8_MEMBER( md6802_state::pia2_kbA_r )
{
	uint8_t ls145;
	uint8_t pa = 0xff;

	// Read out the selected column
	ls145 = m_tb16_74145->read() & 0x0f;

	// read out the artwork, line04 is handled by the timer
	for (unsigned i = 0U; 4U > i; ++i)
	{
		m_lines[i] = m_io_lines[i]->read();

		// Mask out those rows that has a button pressed
		pa &= ~(((~m_lines[i] & ls145) != 0) ? (1 << i) : 0);
	}

	if (m_shift)
	{
		pa &= 0x7f;   // Clear shift bit if button being pressed (PA7) to ground (internal pullup)
		LOG("SHIFT is pressed\n");
	}

	// Serial IN - needs debug/verification
	pa &= (m_rs232->rxd_r() != 0 ? 0xff : 0x7f);

	return pa;
}

/* Pull the cathodes low enabling the correct digit and lit the segments held by port B */
WRITE8_MEMBER( md6802_state::pia2_kbA_w )
{
//  LOG("--->%s(%02x)\n", FUNCNAME, data);

	uint8_t const digit_nbr((data >> 4) & 0x07);
	m_tb16_74145->write(digit_nbr);
	if (digit_nbr < 6)
		m_7segs[digit_nbr] = m_segments;
}

/* PIA 2 Port B is all outputs to drive the display so it is very unlikely that this function is called */
READ8_MEMBER( md6802_state::pia2_kbB_r )
{
	LOG("Warning, trying to read from Port B designated to drive the display, please check why\n");
	logerror("Warning, trying to read from Port B designated to drive the display, please check why\n");
	return 0;
}

/* Port B is fully used ouputting the segment pattern to the display */
WRITE8_MEMBER( md6802_state::pia2_kbB_w )
{
//  LOG("--->%s(%02x)\n", FUNCNAME, data);

	/* Store the segment pattern but do not lit up the digit here, done by pulling the correct cathode low on Port A */
	m_segments = bitswap<8>(data, 0, 4, 5, 3, 2, 1, 7, 6);
}

WRITE_LINE_MEMBER( md6802_state::pia2_ca2_w )
{
	LOG("--->%s(%02x) LED is connected through resisitor to +5v so logical 0 will lit it\n", FUNCNAME, state);
	m_leds[m_led] = state ? 0 :1;

	// Serial Out - needs debug/verification
	m_rs232->write_txd(state);

	m_shift = !state;
}

void md6802_state::machine_start()
{
	LOG("--->%s()\n", FUNCNAME);

	didact_state::machine_start();
	m_7segs.resolve();

	save_item(NAME(m_reset));
	save_item(NAME(m_shift));
	save_item(NAME(m_led));
}

void md6802_state::machine_reset()
{
	LOG("--->%s()\n", FUNCNAME);
	m_led = 1;
	m_maincpu->reset();
}

// This address map is traced from schema
void md6802_state::md6802_map(address_map &map)
{
	map(0x0000, 0x07ff).ram().mirror(0x1800);
	map(0xa000, 0xa003).rw(m_pia1, FUNC(pia6821_device::read), FUNC(pia6821_device::write)).mirror(0x1ffc);
	map(0xc000, 0xc003).rw(m_pia2, FUNC(pia6821_device::read), FUNC(pia6821_device::write)).mirror(0x1ffc);
	map(0xe000, 0xe7ff).rom().mirror(0x1800).region("maincpu", 0xe000);
}

/*
 *  ___________________________________________________________________________________________________________           _____________________________________________________
 * | The Didact Mp68A CPU board, by Anders Andersson 1979                                                      |         |The Didact Mp68A keypad/display  PB6   +oooo+        |
 * |                  +------+ +-------+     +--+                                                              |         |  by Anders Andersson 1979  +-------+  |cass|        |
 * |                  | 7402 | | 74490 |     |  |      +-------+               +--+                            |         |                    +--+    | 9368  |  +----+    +--+|
 * |       +-------+  +------+ +-------+     |  |      |       |               |  |                            |         |+-------+    2x5082-|B |    +-------+            |  ||
 * |       |       |    2112   2112          |  |      | EXP   |               |  |                            |         || 74132 |       7433|CD| 145  PA0-PA3            |E ||
 * |       | ROM   |    +--+   +--+          +--+      | ANS   |               |P |                            |         |+-------+           |DI| +--+               132  |X ||
 * |       | 7641  |    |  |   |  |                    | ION   |               |I |                            |         |+------+------+     | S| |  |               +--+ |P ||
 * |       |       |    |A |   |B |       +-----+      | BUSES |               |A |                            |         ||      |SHIFT |     | P| |  | PA4-PA6       |  | |A ||
 * |       | 512x8 |    |  |   |  |       |     |      | (2 x) |               |  |                            |         || RES  |(led) |     +--+ |  |               |  | |N ||
 * |       |       |    +--+   +--+       |     |      | FOR   |               |A |                            |         ||      |  *   |          +--+               |  | |S ||
 * |       +-------+    RAMS 4x256x4      |     |      |       |               |  |                            |         |+------+------+------+------+               +--+ |I ||
 * |     ROMS 2x512x8   2112   2112       |     |      | KEY   |               |E |                            |         ||      |      |      |      |                    |O ||
 * |       +-------+    +--+   +--+       |CPU  |      | BOARD | +------+      |X |                            |         || ADR  | RUN  | SST  | REG  |                    |N ||
 * |       |       |    |  |   |  |       |6800 |      |       | |      |      |P |                            |         ||  0   |  4   |  8   |  C   |                    |  ||
 * |       | ROM   |    |A |   |B |       |     |      | AND   | |      |      |A |                            |         |+------+------+------+------+                    |C ||
 * |       | 7641  |    |  |   |  |       |     |      |       | |      |      |N |                            |         ||      |      |      |      |                    |O ||
 * |       |       |    +--+   +--+       |     |      | I/O   | | 6820 |      |S |                            |         || STA  | STO  | BPR  | BPS  |                    |N ||
 * |       | 512x8 |    512 bytes RAM     |     |      | BOARDS| | PIA  |      |I |                            |         ||  1   |  5   |  9   |  D   |                    |N ||
 * |       +-------+                      |     |      |       | |  #1  |      |O |                         +-----+      |+------+------+------+------+           +------+ |E ||
 * |     1024 bytes ROM                   |     |      |       | |      |      |N |                         |     |      ||      |      |      |      |           |      | |C ||
 * |                                      +-----+      |       | |      |      |  |                  PIA A  |    |       || EXF  | EXB  | MOV  | PRM  |           |      | |T ||
 * |        7402  7412                                 |       | |      |      |B |                EXPANSION|    |       ||  2   |  6   |  A   |  E   |           |      | |O ||
 * |        +--+  +--+                                 |       | |      |      |U |                CONNECTOR|    |       |+------+------+------+------+           | 6820 | |R ||
 * |        |  |  |  |                                 |       | |      |      |S |                         |   _|       ||      |      |      |      |           | PIA  | |  ||
 * |        |  |  |  |                                 |       | |      |      |  |                     J4  |  |         || CLR  | REL  | REC  | PLA  |           |  #2  | |  ||
 * |        |  |  |  |                                 |       | +------+      |  |                         |  |_        ||  3   |  7   |  B   |  F   |           |      | |  ||
 * |        +--+  +--+         +--------+              |       |               |  |                         |    |       |+------+------+------+------+           |      | |  ||
 * |                  +-+      | 96LS02 |              |       |               |  |                         |    |       | +-------+ +-------+  +------+          |      | |  ||
 * |       R * * * R  |T|      +--------+              |       |               |  |                         |    |       | | 74148 | | 74148 |  | 7400 |          |      | |  ||
 * |       O  X    A  |R|                              |       |               |  |                         |    |       | +-------+ +-------+  +------+          |      | +--+|
 * |       M * * * M  |M|  Oscillator circuits         +-------+               +--+                         |     |      |                PB3    PB0-PB2          |      |     |
 * |                  |_|                               J1   J2                 J3                          +-----+      |       +---------+                      +------+  J1 |
 * |____________________________________________________________________________________________________________|        |______ |  _|||_  |___________________________________|
 *
 */
/* Didact mp68a driver class */

// Just a statement that the real mp68a hardware was designed with 6820 and not 6821
// They are functional equivalents BUT has different electrical characteristics.
#define pia6820_device pia6821_device
#define PIA6820 PIA6821
class mp68a_state : public didact_state
{
	public:
	mp68a_state(const machine_config &mconfig, device_type type, const char * tag)
		: didact_state(mconfig, type, tag)
		, m_maincpu(*this, "maincpu")
		, m_digits(*this, "digit%u", 0U)
		, m_7segs(*this, "digit%u", 0U)
		, m_pia1(*this, PIA1_TAG)
		, m_pia2(*this, PIA2_TAG)
	{ }

	required_device<m6800_cpu_device> m_maincpu;

	// The display segment driver device (there is actually just one, needs rewrite to be correct)
	required_device_array<dm9368_device, 6> m_digits;
	output_finder<6> m_7segs;

	DECLARE_READ8_MEMBER( pia2_kbA_r );
	DECLARE_WRITE8_MEMBER( pia2_kbA_w );
	DECLARE_READ8_MEMBER( pia2_kbB_r );
	DECLARE_WRITE8_MEMBER( pia2_kbB_w );
	DECLARE_READ_LINE_MEMBER( pia2_cb1_r );
	template <unsigned N> DECLARE_WRITE8_MEMBER( digit_w ) { m_7segs[N] = data; }

	virtual void machine_reset() override;
	virtual void machine_start() override;
	void mp68a(machine_config &config);
	void mp68a_map(address_map &map);
protected:
	required_device<pia6820_device> m_pia1;
	required_device<pia6820_device> m_pia2;
};

READ8_MEMBER( mp68a_state::pia2_kbA_r )
{
	LOG("--->%s\n", FUNCNAME);

	return 0;
}

WRITE8_MEMBER( mp68a_state::pia2_kbA_w )
{
	/* Display memory is at $702 to $708 in AAAADD format (A=address digit, D=Data digit)
	   but we are using data read from the port. */
	uint8_t const digit_nbr = (data >> 4) & 0x07;

	/* There is actually only one 9368 and a 74145 to drive the cathode of the right digit low */
	/* This can be emulated by pretending there are one 9368 per digit, at least for now      */
	switch (digit_nbr)
	{
	case 0:
	case 1:
	case 2:
	case 3:
	case 4:
	case 5:
		m_digits[digit_nbr]->a_w(data & 0x0f);
		break;
	case 7: // used as an 'unselect' by the ROM between digit accesses.
		break;
	default:
		logerror("Invalid digit index %d\n", digit_nbr);
	}
}

READ8_MEMBER( mp68a_state::pia2_kbB_r )
{
	uint8_t a012, line, pb;

	LOG("--->%s %02x %02x %02x %02x %02x => ", FUNCNAME, m_lines[0], m_lines[1], m_lines[2], m_lines[3], m_shift);

	a012 = 0;
	if ((line = (m_lines[0] | m_lines[1])) != 0)
	{
		a012 = 8;
		while (a012 > 0 && !(line & (1 << --a012)));
		a012 += 8;
	}
	if ( a012 == 0 && (line = ((m_lines[2]) | m_lines[3])) != 0)
	{
		a012 = 8;
		while (a012 > 0 && !(line & (1 << --a012)));
	}

	pb  = a012;       // A0-A2 -> PB0-PB3

	if (m_shift)
	{
		pb |= 0x80;   // Set shift bit (PB7)
		m_shift = 0;  // Reset flip flop
		m_leds[m_led] = m_shift ? 1 : 0;
		LOG("SHIFT is released\n");
	}

	LOG("%02x\n", pb);

	return pb;
}

WRITE8_MEMBER( mp68a_state::pia2_kbB_w )
{
	LOG("--->%s(%02x)\n", FUNCNAME, data);
}

READ_LINE_MEMBER( mp68a_state::pia2_cb1_r )
{
	for (unsigned i = 0U; 4U > i; ++i)
		m_lines[i] = m_io_lines[i]->read();

#if VERBOSE
	if (m_lines[0] | m_lines[1] | m_lines[2] | m_lines[3])
		LOG("%s()-->%02x %02x %02x %02x\n", FUNCNAME, m_lines[0], m_lines[1], m_lines[2], m_lines[3]);
#endif

	return (m_lines[0] | m_lines[1] | m_lines[2] | m_lines[3]) ? 0 : 1;
}

void mp68a_state::machine_reset()
{
	LOG("--->%s()\n", FUNCNAME);
	m_maincpu->reset();
}

void mp68a_state::machine_start()
{
	LOG("--->%s()\n", FUNCNAME);

	didact_state::machine_start();
	m_7segs.resolve();

	/* register for state saving */
	save_item(NAME(m_shift));
	save_item(NAME(m_led));
	save_item(NAME(m_reset));
}

// This address map is traced from pcb
void mp68a_state::mp68a_map(address_map &map)
{
	map(0x0000, 0x00ff).ram().mirror(0xf000);
	map(0x0500, 0x0503).rw(m_pia1, FUNC(pia6820_device::read), FUNC(pia6820_device::write)).mirror(0xf0fc);
	map(0x0600, 0x0603).rw(m_pia2, FUNC(pia6820_device::read), FUNC(pia6820_device::write)).mirror(0xf0fc);
	map(0x0700, 0x07ff).ram().mirror(0xf000);
	map(0x0800, 0x0bff).rom().mirror(0xf400).region("maincpu", 0x0800);
}

static INPUT_PORTS_START( md6802 )
	PORT_START("LINE0") /* KEY ROW 0 */
	PORT_BIT(0x01, 0x01, IPT_KEYBOARD) PORT_NAME("0") PORT_CODE(KEYCODE_0)  PORT_CHAR('0')
	PORT_BIT(0x02, 0x02, IPT_KEYBOARD) PORT_NAME("1") PORT_CODE(KEYCODE_1)  PORT_CHAR('1')
	PORT_BIT(0x04, 0x04, IPT_KEYBOARD) PORT_NAME("2") PORT_CODE(KEYCODE_2)  PORT_CHAR('2')
	PORT_BIT(0x08, 0x08, IPT_KEYBOARD) PORT_NAME("3") PORT_CODE(KEYCODE_3)  PORT_CHAR('3')
	PORT_BIT(0xf0, 0x00, IPT_UNUSED )

	PORT_START("LINE1") /* KEY ROW 1 */
	PORT_BIT(0x01, 0x01, IPT_KEYBOARD) PORT_NAME("4") PORT_CODE(KEYCODE_4)  PORT_CHAR('4')
	PORT_BIT(0x02, 0x02, IPT_KEYBOARD) PORT_NAME("5") PORT_CODE(KEYCODE_5)  PORT_CHAR('5')
	PORT_BIT(0x04, 0x04, IPT_KEYBOARD) PORT_NAME("6") PORT_CODE(KEYCODE_6)  PORT_CHAR('6')
	PORT_BIT(0x08, 0x08, IPT_KEYBOARD) PORT_NAME("7") PORT_CODE(KEYCODE_7)  PORT_CHAR('7')
	PORT_BIT(0xf0, 0x00, IPT_UNUSED )

	PORT_START("LINE2") /* KEY ROW 2 */
	PORT_BIT(0x01, 0x01, IPT_KEYBOARD) PORT_NAME("8") PORT_CODE(KEYCODE_8)  PORT_CHAR('8')
	PORT_BIT(0x02, 0x02, IPT_KEYBOARD) PORT_NAME("9") PORT_CODE(KEYCODE_9)  PORT_CHAR('9')
	PORT_BIT(0x04, 0x04, IPT_KEYBOARD) PORT_NAME("A") PORT_CODE(KEYCODE_A)  PORT_CHAR('A')
	PORT_BIT(0x08, 0x08, IPT_KEYBOARD) PORT_NAME("B") PORT_CODE(KEYCODE_B)  PORT_CHAR('B')
	PORT_BIT(0xf0, 0x00, IPT_UNUSED )

	PORT_START("LINE3") /* KEY ROW 3 */
	PORT_BIT(0x01, 0x01, IPT_KEYBOARD) PORT_NAME("C") PORT_CODE(KEYCODE_C)  PORT_CHAR('C')
	PORT_BIT(0x02, 0x02, IPT_KEYBOARD) PORT_NAME("D") PORT_CODE(KEYCODE_D)  PORT_CHAR('D')
	PORT_BIT(0x04, 0x04, IPT_KEYBOARD) PORT_NAME("E") PORT_CODE(KEYCODE_E)  PORT_CHAR('E')
	PORT_BIT(0x08, 0x08, IPT_KEYBOARD) PORT_NAME("F") PORT_CODE(KEYCODE_F)  PORT_CHAR('F')
	PORT_BIT(0xf0, 0x00, IPT_UNUSED )

	PORT_START("LINE4") /* Special KEY ROW for reset and Shift/'*' keys */
	PORT_BIT(0x08, 0x00, IPT_KEYBOARD) PORT_NAME("*") PORT_CODE(KEYCODE_LSHIFT) PORT_CODE(KEYCODE_RSHIFT) PORT_CHAR('*')
	PORT_BIT(0x04, 0x00, IPT_KEYBOARD) PORT_NAME("Reset") PORT_CODE(KEYCODE_F12)
	PORT_BIT(0xf3, 0x00, IPT_UNUSED )
INPUT_PORTS_END

static INPUT_PORTS_START( mp68a )
	PORT_START("LINE0") /* KEY ROW 0 */
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("C") PORT_CODE(KEYCODE_C)    PORT_CHAR('C')
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("D") PORT_CODE(KEYCODE_D)    PORT_CHAR('D')
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("E") PORT_CODE(KEYCODE_E)    PORT_CHAR('E')
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("F") PORT_CODE(KEYCODE_F)    PORT_CHAR('F')
	PORT_BIT(0x0f, IP_ACTIVE_HIGH, IPT_UNUSED )

	PORT_START("LINE1") /* KEY ROW 1 */
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("8") PORT_CODE(KEYCODE_8)    PORT_CHAR('8')
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("9") PORT_CODE(KEYCODE_9)    PORT_CHAR('9')
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("A") PORT_CODE(KEYCODE_A)    PORT_CHAR('A')
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("B") PORT_CODE(KEYCODE_B)    PORT_CHAR('B')
	PORT_BIT(0xf0, IP_ACTIVE_HIGH, IPT_UNUSED )

	PORT_START("LINE2") /* KEY ROW 2 */
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("4") PORT_CODE(KEYCODE_4)    PORT_CHAR('4')
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("5") PORT_CODE(KEYCODE_5)    PORT_CHAR('5')
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("6") PORT_CODE(KEYCODE_6)    PORT_CHAR('6')
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("7") PORT_CODE(KEYCODE_7)    PORT_CHAR('7')
	PORT_BIT(0x0f, IP_ACTIVE_HIGH, IPT_UNUSED )

	PORT_START("LINE3") /* KEY ROW 3 */
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("0") PORT_CODE(KEYCODE_0)    PORT_CHAR('0')
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("1") PORT_CODE(KEYCODE_1)    PORT_CHAR('1')
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("2") PORT_CODE(KEYCODE_2)    PORT_CHAR('2')
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("3") PORT_CODE(KEYCODE_3)    PORT_CHAR('3')
	PORT_BIT(0xf0, IP_ACTIVE_HIGH, IPT_UNUSED )

	PORT_START("LINE4") /* Special KEY ROW for reset and Shift/'*' keys */
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("*") PORT_CODE(KEYCODE_LSHIFT) PORT_CODE(KEYCODE_RSHIFT) PORT_CHAR('*')
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_NAME("Reset") PORT_CODE(KEYCODE_F12)
	PORT_BIT(0xf3, IP_ACTIVE_HIGH, IPT_UNUSED )
INPUT_PORTS_END

// TODO: Fix shift led for mp68a correctly, workaround doesn't work anymore! Shift works though...
TIMER_DEVICE_CALLBACK_MEMBER(didact_state::scan_artwork)
{
	//  LOG("--->%s()\n", FUNCNAME);

	// Poll the artwork Reset key
	if (m_io_lines[4]->read() & 0x04)
	{
		LOG("RESET is pressed, resetting the CPU\n");
		m_shift = 0;
		m_leds[m_led] = m_shift ? 1 : 0; // For mp68a only
		if (m_reset == 0)
		{
			machine_reset();
		}
		m_reset = 1; // Inhibit multiple resets
	}
	else if (m_io_lines[4]->read() & 0x08)
	{
		// Poll the artwork SHIFT/* key
		LOG("%s", !m_shift ? "SHIFT is set\n" : "");
		m_shift = 1;
		m_leds[m_led] = m_shift ? 1 : 0; // For mp68a only
	}
	else
	{
		if (m_reset == 1)
		{
			m_reset = 0; // Enable reset again
		}
	}
}

MACHINE_CONFIG_START(md6802_state::md6802)
	MCFG_DEVICE_ADD("maincpu", M6802, XTAL(4'000'000))
	MCFG_DEVICE_PROGRAM_MAP(md6802_map)
	config.set_default_layout(layout_md6802);

	/* Devices */
	TTL74145(config, m_tb16_74145, 0);
	/* PIA #1 0xA000-0xA003 - used differently by laborations and loaded software */
	PIA6821(config, m_pia1, 0);

	/* PIA #2 Keyboard & Display 0xC000-0xC003 */
	PIA6821(config, m_pia2, 0);
	/* --PIA init----------------------- */
	/* 0xE007 0xC002 (DDR B)     = 0xFF - Port B all outputs and set to 0 (zero) */
	/* 0xE00B 0xC000 (DDR A)     = 0x70 - Port A three outputs and set to 0 (zero) */
	/* 0xE00F 0xC001 (Control A) = 0x3C - */
	/* 0xE013 0xC003 (Control B) = 0x3C - */
	/* --execution-wait for key loop-- */
	/* 0xE026 0xC000             = (Reading Port A)  */
	/* 0xE033 0xC000             = (Reading Port A)  */
	/* 0xE068 0xC000 (Port A)    = 0x60 */
	/* 0xE08A 0xC002 (Port B)    = 0xEE - updating display */
	/* 0xE090 0xC000 (Port A)    = 0x00 - looping in 0x10,0x20,0x30,0x40,0x50 */
	m_pia2->writepa_handler().set(FUNC(md6802_state::pia2_kbA_w));
	m_pia2->readpa_handler().set(FUNC(md6802_state::pia2_kbA_r));
	m_pia2->writepb_handler().set(FUNC(md6802_state::pia2_kbB_w));
	m_pia2->readpb_handler().set(FUNC(md6802_state::pia2_kbB_r));
	m_pia2->ca2_handler().set(FUNC(md6802_state::pia2_ca2_w));

	MCFG_TIMER_DRIVER_ADD_PERIODIC("artwork_timer", md6802_state, scan_artwork, attotime::from_hz(10))

	MCFG_DEVICE_ADD("rs232", RS232_PORT, default_rs232_devices, nullptr)
MACHINE_CONFIG_END

MACHINE_CONFIG_START(mp68a_state::mp68a)
	// Clock source is based on a N9602N Dual Retriggerable Resettable Monostable Multivibrator oscillator at aprox 505KHz.
	// Trimpot seems broken/stuck at 5K Ohm thu. ROM code 1Ms delay loops suggest 1MHz+
	MCFG_DEVICE_ADD("maincpu", M6800, 505000)
	MCFG_DEVICE_PROGRAM_MAP(mp68a_map)
	config.set_default_layout(layout_mp68a);

	/* Devices */
	/* PIA #1 0x500-0x503 - used differently by laborations and loaded software */
	PIA6820(config, m_pia1, 0);

	/* PIA #2 Keyboard & Display 0x600-0x603 */
	PIA6820(config, m_pia2, 0);
	/* --PIA inits----------------------- */
	/* 0x0BAF 0x601 (Control A) = 0x30 - CA2 is low and enable DDRA */
	/* 0x0BB1 0x603 (Control B) = 0x30 - CB2 is low and enable DDRB */
	/* 0x0BB5 0x600 (DDR A)     = 0xFF - Port A all outputs and set to 0 (zero) */
	/* 0x0BB9 0x602 (DDR B)     = 0x50 - Port B two outputs and set to 0 (zero) */
	/* 0x0BBD 0x601 (Control A) = 0x34 - CA2 is low and lock DDRA */
	/* 0x0BBF 0x603 (Control B) = 0x34 - CB2 is low and lock DDRB */
	/* 0x0BC3 0x602 (Port B)    = 0x40 - Turn on display via RBI* on  */
	/* --execution-wait for key loop-- */
	/* 0x086B Update display sequnc, see below                            */
	/* 0x0826 CB1 read          = 0x603 (Control B)  - is a key pressed? */
	m_pia2->writepa_handler().set(FUNC(mp68a_state::pia2_kbA_w));
	m_pia2->readpa_handler().set(FUNC(mp68a_state::pia2_kbA_r));
	m_pia2->writepb_handler().set(FUNC(mp68a_state::pia2_kbB_w));
	m_pia2->readpb_handler().set(FUNC(mp68a_state::pia2_kbB_r));
	m_pia2->readcb1_handler().set(FUNC(mp68a_state::pia2_cb1_r));
	m_pia2->irqa_handler().set_inputline("maincpu", M6800_IRQ_LINE); /* Not used by ROM. Combined trace to CPU IRQ with IRQB */
	m_pia2->irqb_handler().set_inputline("maincpu", M6800_IRQ_LINE); /* Not used by ROM. Combined trace to CPU IRQ with IRQA */

	/* Display - sequence outputting all '0':s at start */
	/* 0x086B 0x600 (Port A)    = 0x00 */
	/* 0x086B 0x600 (Port A)    = 0x70 */
	/* 0x086B 0x600 (Port A)    = 0x10 */
	/* 0x086B 0x600 (Port A)    = 0x70 */
	/* 0x086B 0x600 (Port A)    = 0x20 */
	/* 0x086B 0x600 (Port A)    = 0x70 */
	/* 0x086B 0x600 (Port A)    = 0x30 */
	/* 0x086B 0x600 (Port A)    = 0x70 */
	/* 0x086B 0x600 (Port A)    = 0x40 */
	/* 0x086B 0x600 (Port A)    = 0x70 */
	/* 0x086B 0x600 (Port A)    = 0x50 */
	/* 0x086B 0x600 (Port A)    = 0x70 */
	MCFG_DEVICE_ADD("digit0", DM9368, 0)
	MCFG_DM9368_UPDATE_CALLBACK(WRITE8(*this, mp68a_state, digit_w<0>))
	MCFG_DEVICE_ADD("digit1", DM9368, 0)
	MCFG_DM9368_UPDATE_CALLBACK(WRITE8(*this, mp68a_state, digit_w<1>))
	MCFG_DEVICE_ADD("digit2", DM9368, 0)
	MCFG_DM9368_UPDATE_CALLBACK(WRITE8(*this, mp68a_state, digit_w<2>))
	MCFG_DEVICE_ADD("digit3", DM9368, 0)
	MCFG_DM9368_UPDATE_CALLBACK(WRITE8(*this, mp68a_state, digit_w<3>))
	MCFG_DEVICE_ADD("digit4", DM9368, 0)
	MCFG_DM9368_UPDATE_CALLBACK(WRITE8(*this, mp68a_state, digit_w<4>))
	MCFG_DEVICE_ADD("digit5", DM9368, 0)
	MCFG_DM9368_UPDATE_CALLBACK(WRITE8(*this, mp68a_state, digit_w<5>))

	MCFG_TIMER_DRIVER_ADD_PERIODIC("artwork_timer", mp68a_state, scan_artwork, attotime::from_hz(10))
MACHINE_CONFIG_END

// TODO split ROM image into proper ROM set
ROM_START( md6802 ) // ROM image from http://elektronikforumet.com/forum/viewtopic.php?f=2&t=79576&start=135#p1203640
	ROM_REGION(0x10000, "maincpu", 0)
	ROM_LOAD( "didact.bin", 0xe000, 0x0800, CRC(50430b1d) SHA1(8e2172a9ae95b04f20aa14177df2463a286c8465) )
ROM_END

ROM_START( mp68a ) // ROM image from http://elektronikforumet.com/forum/viewtopic.php?f=2&t=79576&start=135#p1203640
	ROM_REGION(0x10000, "maincpu", 0)
	ROM_LOAD( "didacta.bin", 0x0800, 0x0200, CRC(aa05e1ce) SHA1(9ce8223efd274045b43ceca3529e037e16e99fdf) )
	ROM_LOAD( "didactb.bin", 0x0a00, 0x0200, CRC(592898dc) SHA1(2962f4817712cae97f3ab37b088fc73e66535ff8) )
ROM_END

//    YEAR  NAME    PARENT  COMPAT  MACHINE  INPUT   CLASS         INIT        COMPANY      FULLNAME           FLAGS
COMP( 1979, mp68a,  0,      0,      mp68a,   mp68a,  mp68a_state,  empty_init, "Didact AB", "mp68a",           MACHINE_NO_SOUND_HW )
COMP( 1983, md6802, 0,      0,      md6802,  md6802, md6802_state, empty_init, "Didact AB", "Mikrodator 6802", MACHINE_NO_SOUND_HW )
