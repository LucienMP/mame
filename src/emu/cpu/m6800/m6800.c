/*** m6800: Portable 6800 class  emulator *************************************

    m68xx.c

    References:

        6809 Simulator V09, By L.C. Benschop, Eidnhoven The Netherlands.

        m6809: Portable 6809 emulator, DS (6809 code in MAME, derived from
            the 6809 Simulator V09)

        6809 Microcomputer Programming & Interfacing with Experiments"
            by Andrew C. Staugaard, Jr.; Howard W. Sams & Co., Inc.

    System dependencies:    UINT16 must be 16 bit unsigned int
                            UINT8 must be 8 bit unsigned int
                            UINT32 must be more than 16 bits
                            arrays up to 65536 bytes must be supported
                            machine must be twos complement

History
991031  ZV
    Added NSC-8105 support

990319  HJB
    Fixed wrong LSB/MSB order for push/pull word.
    Subtract .extra_cycles at the beginning/end of the exectuion loops.

990316  HJB
    Renamed to 6800, since that's the basic CPU.
    Added different cycle count tables for M6800/2/8, M6801/3 and m68xx.

990314  HJB
    Also added the M6800 subtype.

990311  HJB
    Added _info functions. Now uses static m6808_Regs struct instead
    of single statics. Changed the 16 bit registers to use the generic
    PAIR union. Registers defined using macros. Split the core into
    four execution loops for M6802, M6803, M6808 and HD63701.
    TST, TSTA and TSTB opcodes reset carry flag.
TODO:
    Verify invalid opcodes for the different CPU types.
    Add proper credits to _info functions.
    Integrate m6808_Flags into the registers (multiple m6808 type CPUs?)

990301  HJB
    Modified the interrupt handling. No more pending interrupt checks.
    WAI opcode saves state, when an interrupt is taken (IRQ or OCI),
    the state is only saved if not already done by WAI.

*****************************************************************************/

/*

    Chip                RAM     NVRAM   ROM     SCI
    -----------------------------------------------
    MC6800              -       -       -       no
    MC6801              128     64      2K      yes
    MC68701             128     64      -       yes
    MC6803              128     64      -       yes
    MC6802              128     32      -       no
    MC6802NS            128     -       -       no
    MC6808              -       -       -       no

    HD6301              128     -       4K      yes
    HD63701             192     -       4K      yes
    HD6303              128     -       -       yes
    HD6801              128     64      2K      yes

    NSC8105
    MS2010-A

*/

#include "debugger.h"
#include "deprecat.h"
#include "m6800.h"

#define VERBOSE 0

#define LOG(x)	do { if (VERBOSE) logerror x; } while (0)

#if 0
/* CPU subtypes, needed for extra insn after TAP/CLI/SEI */
enum
{
	SUBTYPE_M6800,
	SUBTYPE_M6801,
	SUBTYPE_M6802,
	SUBTYPE_M6803,
	SUBTYPE_M6808,
	SUBTYPE_HD63701,
	SUBTYPE_NSC8105
};
#endif

/* 6800 Registers */
typedef struct
{
//  int     subtype;        /* CPU subtype */
	PAIR	ppc;			/* Previous program counter */
	PAIR	pc; 			/* Program counter */
	PAIR	s;				/* Stack pointer */
	PAIR	x;				/* Index register */
	PAIR	d;				/* Accumulators */
	UINT8	cc; 			/* Condition codes */
	UINT8	wai_state;		/* WAI opcode state ,(or sleep opcode state) */
	UINT8	nmi_state;		/* NMI line state */
	UINT8	irq_state[2];	/* IRQ line state [IRQ1,TIN] */
	UINT8	ic_eddge;		/* InputCapture eddge , b.0=fall,b.1=raise */

	cpu_irq_callback irq_callback;
	const device_config *device;
	int 	extra_cycles;	/* cycles used for interrupts */
	void	(* const * insn)(void);	/* instruction table */
	const UINT8 *cycles;			/* clock cycle of instruction table */
	/* internal registers */
	UINT8	port1_ddr;
	UINT8	port2_ddr;
	UINT8	port3_ddr;
	UINT8	port4_ddr;
	UINT8	port1_data;
	UINT8	port2_data;
	UINT8	port3_data;
	UINT8	port4_data;
	UINT8	tcsr;			/* Timer Control and Status Register */
	UINT8	pending_tcsr;	/* pending IRQ flag for clear IRQflag process */
	UINT8	irq2;			/* IRQ2 flags */
	UINT8	ram_ctrl;
	PAIR	counter;		/* free running counter */
	PAIR	output_compare;	/* output compare       */
	UINT16	input_capture;	/* input capture        */

	int		clock;
	UINT8	trcsr, rmcr, rdr, tdr, rsr, tsr;
	int		rxbits, txbits, txstate, trcsr_read, tx;

	PAIR	timer_over;
}   m6800_Regs;

/* 680x registers */
static m6800_Regs m68xx;

static emu_timer *m6800_rx_timer;
static emu_timer *m6800_tx_timer;

#define	pPPC	m68xx.ppc
#define pPC 	m68xx.pc
#define pS		m68xx.s
#define pX		m68xx.x
#define pD		m68xx.d

#define PC		m68xx.pc.w.l
#define PCD		m68xx.pc.d
#define S		m68xx.s.w.l
#define SD		m68xx.s.d
#define X		m68xx.x.w.l
#define D		m68xx.d.w.l
#define A		m68xx.d.b.h
#define B		m68xx.d.b.l
#define CC		m68xx.cc

#define CT		m68xx.counter.w.l
#define CTH		m68xx.counter.w.h
#define CTD		m68xx.counter.d
#define OC		m68xx.output_compare.w.l
#define OCH		m68xx.output_compare.w.h
#define OCD		m68xx.output_compare.d
#define TOH		m68xx.timer_over.w.l
#define TOD		m68xx.timer_over.d

static PAIR ea; 		/* effective address */
#define EAD ea.d
#define EA	ea.w.l

/* public globals */
static int m6800_ICount;

/* point of next timer event */
static UINT32 timer_next;

/* DS -- THESE ARE RE-DEFINED IN m68xx.h TO RAM, ROM or FUNCTIONS IN cpuintrf.c */
#define RM				M6800_RDMEM
#define WM				M6800_WRMEM
#define M_RDOP			M6800_RDOP
#define M_RDOP_ARG		M6800_RDOP_ARG

/* macros to access memory */
#define IMMBYTE(b)	b = M_RDOP_ARG(PCD); PC++
#define IMMWORD(w)	w.d = (M_RDOP_ARG(PCD)<<8) | M_RDOP_ARG((PCD+1)&0xffff); PC+=2

#define PUSHBYTE(b) WM(SD,b); --S
#define PUSHWORD(w) WM(SD,w.b.l); --S; WM(SD,w.b.h); --S
#define PULLBYTE(b) S++; b = RM(SD)
#define PULLWORD(w) S++; w.d = RM(SD)<<8; S++; w.d |= RM(SD)

#define MODIFIED_tcsr {	\
	m68xx.irq2 = (m68xx.tcsr&(m68xx.tcsr<<3))&(TCSR_ICF|TCSR_OCF|TCSR_TOF); \
}

#define SET_TIMER_EVENT {					\
	timer_next = (OCD - CTD < TOD - CTD) ? OCD : TOD;	\
}

/* cleanup high-word of counters */
#define CLEANUP_conters {						\
	OCH -= CTH;									\
	TOH -= CTH;									\
	CTH = 0;									\
	SET_TIMER_EVENT;							\
}

/* when change freerunningcounter or outputcapture */
#define MODIFIED_counters {						\
	OCH = (OC >= CT) ? CTH : CTH+1;				\
	SET_TIMER_EVENT;							\
}

// serial I/O

#define M6800_RMCR_SS_MASK		0x03 // Speed Select
#define M6800_RMCR_SS_4096		0x03 // E / 4096
#define M6800_RMCR_SS_1024		0x02 // E / 1024
#define M6800_RMCR_SS_128		0x01 // E / 128
#define M6800_RMCR_SS_16		0x00 // E / 16
#define M6800_RMCR_CC_MASK		0x0c // Clock Control/Format Select

#define M6800_TRCSR_RDRF		0x80 // Receive Data Register Full
#define M6800_TRCSR_ORFE		0x40 // Over Run Framing Error
#define M6800_TRCSR_TDRE		0x20 // Transmit Data Register Empty
#define M6800_TRCSR_RIE		0x10 // Receive Interrupt Enable
#define M6800_TRCSR_RE			0x08 // Receive Enable
#define M6800_TRCSR_TIE		0x04 // Transmit Interrupt Enable
#define M6800_TRCSR_TE			0x02 // Transmit Enable
#define M6800_TRCSR_WU			0x01 // Wake Up

#define M6800_PORT2_IO4		0x10
#define M6800_PORT2_IO3		0x08

static const int M6800_RMCR_SS[] = { 16, 128, 1024, 4096 };

#define M6800_SERIAL_START		0
#define M6800_SERIAL_STOP		9

enum
{
	M6800_TX_STATE_INIT = 0,
	M6800_TX_STATE_READY
};

/* take interrupt */
#define TAKE_ICI ENTER_INTERRUPT("M6800#%d take ICI\n",0xfff6)
#define TAKE_OCI ENTER_INTERRUPT("M6800#%d take OCI\n",0xfff4)
#define TAKE_TOI ENTER_INTERRUPT("M6800#%d take TOI\n",0xfff2)
#define TAKE_SCI ENTER_INTERRUPT("M6800#%d take SCI\n",0xfff0)
#define TAKE_TRAP ENTER_INTERRUPT("M6800#%d take TRAP\n",0xffee)

/* operate one instruction for */
#define ONE_MORE_INSN() {		\
	UINT8 ireg; 							\
	pPPC = pPC; 							\
	debugger_instruction_hook(Machine, PCD);						\
	ireg=M_RDOP(PCD);						\
	PC++;									\
	(*m68xx.insn[ireg])();					\
	INCREMENT_COUNTER(m68xx.cycles[ireg]);	\
}

/* check the IRQ lines for pending interrupts */
#define CHECK_IRQ_LINES() {										\
	if( !(CC & 0x10) )											\
	{															\
		if( m68xx.irq_state[M6800_IRQ_LINE] != CLEAR_LINE )		\
		{	/* standard IRQ */									\
			ENTER_INTERRUPT("M6800#%d take IRQ1\n",0xfff8);		\
			if( m68xx.irq_callback )							\
				(void)(*m68xx.irq_callback)(m68xx.device, M6800_IRQ_LINE);	\
		}														\
		else													\
			m6800_check_irq2();									\
	}															\
}

/* CC masks                       HI NZVC
                                7654 3210   */
#define CLR_HNZVC	CC&=0xd0
#define CLR_NZV 	CC&=0xf1
#define CLR_HNZC	CC&=0xd2
#define CLR_NZVC	CC&=0xf0
#define CLR_Z		CC&=0xfb
#define CLR_NZC 	CC&=0xf2
#define CLR_ZC		CC&=0xfa
#define CLR_C		CC&=0xfe

/* macros for CC -- CC bits affected should be reset before calling */
#define SET_Z(a)		if(!(a))SEZ
#define SET_Z8(a)		SET_Z((UINT8)(a))
#define SET_Z16(a)		SET_Z((UINT16)(a))
#define SET_N8(a)		CC|=(((a)&0x80)>>4)
#define SET_N16(a)		CC|=(((a)&0x8000)>>12)
#define SET_H(a,b,r)	CC|=((((a)^(b)^(r))&0x10)<<1)
#define SET_C8(a)		CC|=(((a)&0x100)>>8)
#define SET_C16(a)		CC|=(((a)&0x10000)>>16)
#define SET_V8(a,b,r)	CC|=((((a)^(b)^(r)^((r)>>1))&0x80)>>6)
#define SET_V16(a,b,r)	CC|=((((a)^(b)^(r)^((r)>>1))&0x8000)>>14)

static const UINT8 flags8i[256]=	 /* increment */
{
0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x0a,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08
};
static const UINT8 flags8d[256]= /* decrement */
{
0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08
};
#define SET_FLAGS8I(a)		{CC|=flags8i[(a)&0xff];}
#define SET_FLAGS8D(a)		{CC|=flags8d[(a)&0xff];}

/* combos */
#define SET_NZ8(a)			{SET_N8(a);SET_Z8(a);}
#define SET_NZ16(a)			{SET_N16(a);SET_Z16(a);}
#define SET_FLAGS8(a,b,r)	{SET_N8(r);SET_Z8(r);SET_V8(a,b,r);SET_C8(r);}
#define SET_FLAGS16(a,b,r)	{SET_N16(r);SET_Z16(r);SET_V16(a,b,r);SET_C16(r);}

/* for treating an UINT8 as a signed INT16 */
#define SIGNED(b) ((INT16)(b&0x80?b|0xff00:b))

/* Macros for addressing modes */
#define DIRECT IMMBYTE(EAD)
#define IMM8 EA=PC++
#define IMM16 {EA=PC;PC+=2;}
#define EXTENDED IMMWORD(ea)
#define INDEXED {EA=X+(UINT8)M_RDOP_ARG(PCD);PC++;}

/* macros to set status flags */
#if defined(SEC)
#undef SEC
#endif
#define SEC CC|=0x01
#define CLC CC&=0xfe
#define SEZ CC|=0x04
#define CLZ CC&=0xfb
#define SEN CC|=0x08
#define CLN CC&=0xf7
#define SEV CC|=0x02
#define CLV CC&=0xfd
#define SEH CC|=0x20
#define CLH CC&=0xdf
#define SEI CC|=0x10
#define CLI CC&=~0x10

/* mnemonicos for the Timer Control and Status Register bits */
#define TCSR_OLVL 0x01
#define TCSR_IEDG 0x02
#define TCSR_ETOI 0x04
#define TCSR_EOCI 0x08
#define TCSR_EICI 0x10
#define TCSR_TOF  0x20
#define TCSR_OCF  0x40
#define TCSR_ICF  0x80

#define INCREMENT_COUNTER(amount)	\
{									\
	m6800_ICount -= amount;			\
	CTD += amount;					\
	if( CTD >= timer_next)			\
		check_timer_event();		\
}

#define EAT_CYCLES													\
{																	\
	int cycles_to_eat;												\
																	\
	cycles_to_eat = timer_next - CTD;								\
	if( cycles_to_eat > m6800_ICount) cycles_to_eat = m6800_ICount;	\
	if (cycles_to_eat > 0)											\
	{																\
		INCREMENT_COUNTER(cycles_to_eat);							\
	}																\
}

/* macros for convenience */
#define DIRBYTE(b) {DIRECT;b=RM(EAD);}
#define DIRWORD(w) {DIRECT;w.d=RM16(EAD);}
#define EXTBYTE(b) {EXTENDED;b=RM(EAD);}
#define EXTWORD(w) {EXTENDED;w.d=RM16(EAD);}

#define IDXBYTE(b) {INDEXED;b=RM(EAD);}
#define IDXWORD(w) {INDEXED;w.d=RM16(EAD);}

/* Macros for branch instructions */
#define CHANGE_PC() change_pc(PCD)
#define BRANCH(f) {IMMBYTE(t);if(f){PC+=SIGNED(t);CHANGE_PC();}}
#define NXORV  ((CC&0x08)^((CC&0x02)<<2))

/* Note: we use 99 cycles here for invalid opcodes so that we don't */
/* hang in an infinite loop if we hit one */
static const UINT8 cycles_6800[] =
{
		/* 0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
	/*0*/ 99, 2,99,99,99,99, 2, 2, 4, 4, 2, 2, 2, 2, 2, 2,
	/*1*/  2, 2,99,99,99,99, 2, 2,99, 2,99, 2,99,99,99,99,
	/*2*/  4,99, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	/*3*/  4, 4, 4, 4, 4, 4, 4, 4,99, 5,99,10,99,99, 9,12,
	/*4*/  2,99,99, 2, 2,99, 2, 2, 2, 2, 2,99, 2, 2,99, 2,
	/*5*/  2,99,99, 2, 2,99, 2, 2, 2, 2, 2,99, 2, 2,99, 2,
	/*6*/  7,99,99, 7, 7,99, 7, 7, 7, 7, 7,99, 7, 7, 4, 7,
	/*7*/  6,99,99, 6, 6,99, 6, 6, 6, 6, 6,99, 6, 6, 3, 6,
	/*8*/  2, 2, 2,99, 2, 2, 2,99, 2, 2, 2, 2, 3, 8, 3,99,
	/*9*/  3, 3, 3,99, 3, 3, 3, 4, 3, 3, 3, 3, 4,99, 4, 5,
	/*A*/  5, 5, 5,99, 5, 5, 5, 6, 5, 5, 5, 5, 6, 8, 6, 7,
	/*B*/  4, 4, 4,99, 4, 4, 4, 5, 4, 4, 4, 4, 5, 9, 5, 6,
	/*C*/  2, 2, 2,99, 2, 2, 2,99, 2, 2, 2, 2,99,99, 3,99,
	/*D*/  3, 3, 3,99, 3, 3, 3, 4, 3, 3, 3, 3,99,99, 4, 5,
	/*E*/  5, 5, 5,99, 5, 5, 5, 6, 5, 5, 5, 5,99,99, 6, 7,
	/*F*/  4, 4, 4,99, 4, 4, 4, 5, 4, 4, 4, 4,99,99, 5, 6
};

static const UINT8 cycles_6803[] =
{
		/* 0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
	/*0*/ 99, 2,99,99, 3, 3, 2, 2, 3, 3, 2, 2, 2, 2, 2, 2,
	/*1*/  2, 2,99,99,99,99, 2, 2,99, 2,99, 2,99,99,99,99,
	/*2*/  3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
	/*3*/  3, 3, 4, 4, 3, 3, 3, 3, 5, 5, 3,10, 4,10, 9,12,
	/*4*/  2,99,99, 2, 2,99, 2, 2, 2, 2, 2,99, 2, 2,99, 2,
	/*5*/  2,99,99, 2, 2,99, 2, 2, 2, 2, 2,99, 2, 2,99, 2,
	/*6*/  6,99,99, 6, 6,99, 6, 6, 6, 6, 6,99, 6, 6, 3, 6,
	/*7*/  6,99,99, 6, 6,99, 6, 6, 6, 6, 6,99, 6, 6, 3, 6,
	/*8*/  2, 2, 2, 4, 2, 2, 2,99, 2, 2, 2, 2, 4, 6, 3,99,
	/*9*/  3, 3, 3, 5, 3, 3, 3, 3, 3, 3, 3, 3, 5, 5, 4, 4,
	/*A*/  4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 6, 6, 5, 5,
	/*B*/  4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 6, 6, 5, 5,
	/*C*/  2, 2, 2, 4, 2, 2, 2,99, 2, 2, 2, 2, 3,99, 3,99,
	/*D*/  3, 3, 3, 5, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4,
	/*E*/  4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5,
	/*F*/  4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5
};

static const UINT8 cycles_63701[] =
{
		/* 0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
	/*0*/ 99, 1,99,99, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	/*1*/  1, 1,99,99,99,99, 1, 1, 2, 2, 4, 1,99,99,99,99,
	/*2*/  3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
	/*3*/  1, 1, 3, 3, 1, 1, 4, 4, 4, 5, 1,10, 5, 7, 9,12,
	/*4*/  1,99,99, 1, 1,99, 1, 1, 1, 1, 1,99, 1, 1,99, 1,
	/*5*/  1,99,99, 1, 1,99, 1, 1, 1, 1, 1,99, 1, 1,99, 1,
	/*6*/  6, 7, 7, 6, 6, 7, 6, 6, 6, 6, 6, 5, 6, 4, 3, 5,
	/*7*/  6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 4, 6, 4, 3, 5,
	/*8*/  2, 2, 2, 3, 2, 2, 2,99, 2, 2, 2, 2, 3, 5, 3,99,
	/*9*/  3, 3, 3, 4, 3, 3, 3, 3, 3, 3, 3, 3, 4, 5, 4, 4,
	/*A*/  4, 4, 4, 5, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5,
	/*B*/  4, 4, 4, 5, 4, 4, 4, 4, 4, 4, 4, 4, 5, 6, 5, 5,
	/*C*/  2, 2, 2, 3, 2, 2, 2,99, 2, 2, 2, 2, 3,99, 3,99,
	/*D*/  3, 3, 3, 4, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4,
	/*E*/  4, 4, 4, 5, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5,
	/*F*/  4, 4, 4, 5, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5
};

static const UINT8 cycles_nsc8105[] =
{
		/* 0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
	/*0*/ 99,99, 2,99,99, 2,99, 2, 4, 2, 4, 2, 2, 2, 2, 2,
	/*1*/  2,99, 2,99,99, 2,99, 2,99,99, 2, 2,99,99,99,99,
	/*2*/  4, 4,99, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	/*3*/  4, 4, 4, 4, 4, 4, 4, 4,99,99, 5,10,99, 9,99,12,
	/*4*/  2,99,99, 2, 2, 2,99, 2, 2, 2, 2,99, 2,99, 2, 2,
	/*5*/  2,99,99, 2, 2, 2,99, 2, 2, 2, 2,99, 2,99, 2, 2,
	/*6*/  7,99,99, 7, 7, 7,99, 7, 7, 7, 7,99, 7, 4, 7, 7,
	/*7*/  6,99,99, 6, 6, 6,99, 6, 6, 6, 6,99, 6, 3, 6, 6,
	/*8*/  2, 2, 2,99, 2, 2, 2,99, 2, 2, 2, 2, 3, 3, 8,99,
	/*9*/  3, 3, 3,99, 3, 3, 3, 4, 3, 3, 3, 3, 4, 4,99, 5,
	/*A*/  5, 5, 5,99, 5, 5, 5, 6, 5, 5, 5, 5, 6, 6, 8, 7,
	/*B*/  4, 4, 4,99, 4, 4, 4, 5, 4, 4, 4, 4, 5, 5, 9, 6,
	/*C*/  2, 2, 2,99, 2, 2, 2,99, 2, 2, 2, 2,99, 3,99,99,
	/*D*/  3, 3, 3,99, 3, 3, 3, 4, 3, 3, 3, 3,99, 4,99, 5,
	/*E*/  5, 5, 5,99, 5, 5, 5, 6, 5, 5, 5, 5, 5, 6,99, 7,
	/*F*/  4, 4, 4,99, 4, 4, 4, 5, 4, 4, 4, 4, 4, 5,99, 6
};

INLINE UINT32 RM16( UINT32 Addr )
{
	UINT32 result = RM(Addr) << 8;
	return result | RM((Addr+1)&0xffff);
}

INLINE void WM16( UINT32 Addr, PAIR *p )
{
	WM( Addr, p->b.h );
	WM( (Addr+1)&0xffff, p->b.l );
}

/* IRQ enter */
static void ENTER_INTERRUPT(const char *message,UINT16 irq_vector)
{
	LOG((message, cpunum_get_active()));
	if( m68xx.wai_state & (M6800_WAI|M6800_SLP) )
	{
		if( m68xx.wai_state & M6800_WAI )
			m68xx.extra_cycles += 4;
		m68xx.wai_state &= ~(M6800_WAI|M6800_SLP);
	}
	else
	{
		PUSHWORD(pPC);
		PUSHWORD(pX);
		PUSHBYTE(A);
		PUSHBYTE(B);
		PUSHBYTE(CC);
		m68xx.extra_cycles += 12;
	}
	SEI;
	PCD = RM16( irq_vector );
	CHANGE_PC();
}

static void m6800_check_irq2(void)
{
	if ((m68xx.tcsr & (TCSR_EICI|TCSR_ICF)) == (TCSR_EICI|TCSR_ICF))
	{
		TAKE_ICI;
		if( m68xx.irq_callback )
			(void)(*m68xx.irq_callback)(m68xx.device, M6800_TIN_LINE);
	}
	else if ((m68xx.tcsr & (TCSR_EOCI|TCSR_OCF)) == (TCSR_EOCI|TCSR_OCF))
	{
		TAKE_OCI;
	}
	else if ((m68xx.tcsr & (TCSR_ETOI|TCSR_TOF)) == (TCSR_ETOI|TCSR_TOF))
	{
		TAKE_TOI;
	}
	else if (((m68xx.trcsr & (M6800_TRCSR_RIE|M6800_TRCSR_RDRF)) == (M6800_TRCSR_RIE|M6800_TRCSR_RDRF)) ||
			 ((m68xx.trcsr & (M6800_TRCSR_RIE|M6800_TRCSR_ORFE)) == (M6800_TRCSR_RIE|M6800_TRCSR_ORFE)) ||
			 ((m68xx.trcsr & (M6800_TRCSR_TIE|M6800_TRCSR_TDRE)) == (M6800_TRCSR_TIE|M6800_TRCSR_TDRE)))
	{
		TAKE_SCI;
	}
}

/* check OCI or TOI */
static void check_timer_event(void)
{
	/* OCI */
	if( CTD >= OCD)
	{
		OCH++;	// next IRQ point
		m68xx.tcsr |= TCSR_OCF;
		m68xx.pending_tcsr |= TCSR_OCF;
		MODIFIED_tcsr;
		if ( !(CC & 0x10) && (m68xx.tcsr & TCSR_EOCI))
			TAKE_OCI;
	}
	/* TOI */
	if( CTD >= TOD)
	{
		TOH++;	// next IRQ point
#if 0
		CLEANUP_conters;
#endif
		m68xx.tcsr |= TCSR_TOF;
		m68xx.pending_tcsr |= TCSR_TOF;
		MODIFIED_tcsr;
		if ( !(CC & 0x10) && (m68xx.tcsr & TCSR_ETOI))
			TAKE_TOI;
	}
	/* set next event */
	SET_TIMER_EVENT;
}

/* include the opcode prototypes and function pointer tables */
#include "6800tbl.c"

/* include the opcode functions */
#include "6800ops.c"

#if (HAS_M6801||HAS_M6803||HAS_HD63701)
static void m6800_tx(int value)
{
	m68xx.port2_data = (m68xx.port2_data & 0xef) | (value << 4);

	if(m68xx.port2_ddr == 0xff)
		io_write_byte_8be(M6803_PORT2,m68xx.port2_data);
	else
		io_write_byte_8be(M6803_PORT2,(m68xx.port2_data & m68xx.port2_ddr)
			| (io_read_byte_8be(M6803_PORT2) & (m68xx.port2_ddr ^ 0xff)));
}

static int m6800_rx(void)
{
	return (io_read_byte_8be(M6803_PORT2) & M6800_PORT2_IO3) >> 3;
}

static TIMER_CALLBACK(m6800_tx_tick)
{
    int cpunum = param;

	if (m68xx.trcsr & M6800_TRCSR_TE)
	{
		// force Port 2 bit 4 as output
		m68xx.port2_ddr |= M6800_PORT2_IO4;

		switch (m68xx.txstate)
		{
		case M6800_TX_STATE_INIT:
			m68xx.tx = 1;
			m68xx.txbits++;

			if (m68xx.txbits == 10)
			{
				m68xx.txstate = M6800_TX_STATE_READY;
				m68xx.txbits = M6800_SERIAL_START;
			}
			break;

		case M6800_TX_STATE_READY:
			switch (m68xx.txbits)
			{
			case M6800_SERIAL_START:
				if (m68xx.trcsr & M6800_TRCSR_TDRE)
				{
					// transmit buffer is empty, send consecutive '1's
					m68xx.tx = 1;
				}
				else
				{
					// transmit buffer is full, send data

					// load TDR to shift register
					m68xx.tsr = m68xx.tdr;

					// transmit buffer is empty, set TDRE flag
					m68xx.trcsr |= M6800_TRCSR_TDRE;

					// send start bit '0'
					m68xx.tx = 0;

					m68xx.txbits++;
				}
				break;

			case M6800_SERIAL_STOP:
				// send stop bit '1'
				m68xx.tx = 1;

			    cpu_push_context(machine->cpu[cpunum]);
				CHECK_IRQ_LINES();
				cpu_pop_context();

				m68xx.txbits = M6800_SERIAL_START;
				break;

			default:
				// send data bit '0' or '1'
				m68xx.tx = m68xx.tsr & 0x01;

				// shift transmit register
				m68xx.tsr >>= 1;

				m68xx.txbits++;
				break;
			}
			break;
		}
	}

	m6800_tx(m68xx.tx);
}

static TIMER_CALLBACK(m6800_rx_tick)
{
    int cpunum = param;

	if (m68xx.trcsr & M6800_TRCSR_RE)
	{
		if (m68xx.trcsr & M6800_TRCSR_WU)
		{
			// wait for 10 bits of '1'

			if (m6800_rx() == 1)
			{
				m68xx.rxbits++;

				if (m68xx.rxbits == 10)
				{
					m68xx.trcsr &= ~M6800_TRCSR_WU;
					m68xx.rxbits = M6800_SERIAL_START;
				}
			}
			else
			{
				m68xx.rxbits = M6800_SERIAL_START;
			}
		}
		else
		{
			// receive data

			switch (m68xx.rxbits)
			{
			case M6800_SERIAL_START:
				if (m6800_rx() == 0)
				{
					// start bit found
					m68xx.rxbits++;
				}
				break;

			case M6800_SERIAL_STOP:
				if (m6800_rx() == 1)
				{
					if (m68xx.trcsr & M6800_TRCSR_RDRF)
					{
						// overrun error

						m68xx.trcsr |= M6800_TRCSR_ORFE;

					    cpu_push_context(machine->cpu[cpunum]);
						CHECK_IRQ_LINES();
						cpu_pop_context();
					}
					else
					{
						if (!(m68xx.trcsr & M6800_TRCSR_ORFE))
						{
							// transfer data into receive register
							m68xx.rdr = m68xx.rsr;

							// set RDRF flag
							m68xx.trcsr |= M6800_TRCSR_RDRF;

							cpu_push_context(machine->cpu[cpunum]);
							CHECK_IRQ_LINES();
							cpu_pop_context();
						}
					}
				}
				else
				{
					// framing error

					if (!(m68xx.trcsr & M6800_TRCSR_ORFE))
					{
						// transfer unframed data into receive register
						m68xx.rdr = m68xx.rsr;
					}

					m68xx.trcsr |= M6800_TRCSR_ORFE;
					m68xx.trcsr &= ~M6800_TRCSR_RDRF;

					cpu_push_context(machine->cpu[cpunum]);
					CHECK_IRQ_LINES();
					cpu_pop_context();
				}

				m68xx.rxbits = M6800_SERIAL_START;
				break;

			default:
				// shift receive register
				m68xx.rsr >>= 1;

				// receive bit into register
				m68xx.rsr |= (m6800_rx() << 7);

				m68xx.rxbits++;
				break;
			}
		}
	}
}
#endif

/****************************************************************************
 * Reset registers to their initial values
 ****************************************************************************/
static void state_register(const char *type, int index)
{
	state_save_register_item(type, index, m68xx.ppc.w.l);
	state_save_register_item(type, index, m68xx.pc.w.l);
	state_save_register_item(type, index, m68xx.s.w.l);
	state_save_register_item(type, index, m68xx.x.w.l);
	state_save_register_item(type, index, m68xx.d.w.l);
	state_save_register_item(type, index, m68xx.cc);
	state_save_register_item(type, index, m68xx.wai_state);
	state_save_register_item(type, index, m68xx.nmi_state);
	state_save_register_item_array(type, index, m68xx.irq_state);
	state_save_register_item(type, index, m68xx.ic_eddge);

	state_save_register_item(type, index, m68xx.port1_ddr);
	state_save_register_item(type, index, m68xx.port2_ddr);
	state_save_register_item(type, index, m68xx.port3_ddr);
	state_save_register_item(type, index, m68xx.port4_ddr);
	state_save_register_item(type, index, m68xx.port1_data);
	state_save_register_item(type, index, m68xx.port2_data);
	state_save_register_item(type, index, m68xx.port3_data);
	state_save_register_item(type, index, m68xx.port4_data);
	state_save_register_item(type, index, m68xx.tcsr);
	state_save_register_item(type, index, m68xx.pending_tcsr);
	state_save_register_item(type, index, m68xx.irq2);
	state_save_register_item(type, index, m68xx.ram_ctrl);

	state_save_register_item(type, index, m68xx.counter.d);
	state_save_register_item(type, index, m68xx.output_compare.d);
	state_save_register_item(type, index, m68xx.input_capture);
	state_save_register_item(type, index, m68xx.timer_over.d);

	state_save_register_item(type, index, m68xx.clock);
	state_save_register_item(type, index, m68xx.trcsr);
	state_save_register_item(type, index, m68xx.rmcr);
	state_save_register_item(type, index, m68xx.rdr);
	state_save_register_item(type, index, m68xx.tdr);
	state_save_register_item(type, index, m68xx.rsr);
	state_save_register_item(type, index, m68xx.tsr);
	state_save_register_item(type, index, m68xx.rxbits);
	state_save_register_item(type, index, m68xx.txbits);
	state_save_register_item(type, index, m68xx.txstate);
	state_save_register_item(type, index, m68xx.trcsr_read);
	state_save_register_item(type, index, m68xx.tx);
}

static CPU_INIT( m6800 )
{
//  m68xx.subtype   = SUBTYPE_M6800;
	m68xx.insn = m6800_insn;
	m68xx.cycles = cycles_6800;
	m68xx.irq_callback = irqcallback;
	m68xx.device = device;
	state_register("m6800", index);
}

static CPU_RESET( m6800 )
{
	SEI;				/* IRQ disabled */
	PCD = RM16( 0xfffe );
	CHANGE_PC();

	m68xx.wai_state = 0;
	m68xx.nmi_state = 0;
	m68xx.irq_state[M6800_IRQ_LINE] = 0;
	m68xx.irq_state[M6800_TIN_LINE] = 0;
	m68xx.ic_eddge = 0;

	m68xx.port1_ddr = 0x00;
	m68xx.port2_ddr = 0x00;
	/* TODO: on reset port 2 should be read to determine the operating mode (bits 0-2) */
	m68xx.tcsr = 0x00;
	m68xx.pending_tcsr = 0x00;
	m68xx.irq2 = 0;
	CTD = 0x0000;
	OCD = 0xffff;
	TOD = 0xffff;
	m68xx.ram_ctrl |= 0x40;

	m68xx.trcsr = M6800_TRCSR_TDRE;
	m68xx.rmcr = 0;
	if (m6800_rx_timer) timer_enable(m6800_rx_timer, 0);
	if (m6800_tx_timer) timer_enable(m6800_tx_timer, 0);
	m68xx.txstate = M6800_TX_STATE_INIT;
	m68xx.txbits = m68xx.rxbits = 0;
	m68xx.trcsr_read = 0;
}

/****************************************************************************
 * Shut down CPU emulation
 ****************************************************************************/
static CPU_EXIT( m6800 )
{
	/* nothing to do */
}

/****************************************************************************
 * Get all registers in given buffer
 ****************************************************************************/
static CPU_GET_CONTEXT( m6800 )
{
	if( dst )
		*(m6800_Regs*)dst = m68xx;
}


/****************************************************************************
 * Set all registers to given values
 ****************************************************************************/
static CPU_SET_CONTEXT( m6800 )
{
	if( src )
		m68xx = *(m6800_Regs*)src;
	CHANGE_PC();
	CHECK_IRQ_LINES(); /* HJB 990417 */
}


static void set_irq_line(int irqline, int state)
{
	if (irqline == INPUT_LINE_NMI)
	{
		if (m68xx.nmi_state == state) return;
		LOG(("M6800#%d set_nmi_line %d \n", cpunum_get_active(), state));
		m68xx.nmi_state = state;
		if (state == CLEAR_LINE) return;

		/* NMI */
		ENTER_INTERRUPT("M6800#%d take NMI\n",0xfffc);
	}
	else
	{
		int eddge;

		if (m68xx.irq_state[irqline] == state) return;
		LOG(("M6800#%d set_irq_line %d,%d\n", cpunum_get_active(), irqline, state));
		m68xx.irq_state[irqline] = state;

		switch(irqline)
		{
		case M6800_IRQ_LINE:
			if (state == CLEAR_LINE) return;
			break;
		case M6800_TIN_LINE:
			eddge = (state == CLEAR_LINE ) ? 2 : 0;
			if( ((m68xx.tcsr&TCSR_IEDG) ^ (state==CLEAR_LINE ? TCSR_IEDG : 0))==0 )
				return;
			/* active edge in */
			m68xx.tcsr |= TCSR_ICF;
			m68xx.pending_tcsr |= TCSR_ICF;
			m68xx.input_capture = CT;
			MODIFIED_tcsr;
			if( !(CC & 0x10) )
				m6800_check_irq2();
			break;
		default:
			return;
		}
		CHECK_IRQ_LINES(); /* HJB 990417 */
	}
}

/****************************************************************************
 * Execute cycles CPU cycles. Return number of cycles really executed
 ****************************************************************************/
static CPU_EXECUTE( m6800 )
{
	UINT8 ireg;
	m6800_ICount = cycles;

	CLEANUP_conters;
	INCREMENT_COUNTER(m68xx.extra_cycles);
	m68xx.extra_cycles = 0;

	do
	{
		if( m68xx.wai_state & M6800_WAI )
		{
			EAT_CYCLES;
		}
		else
		{
			pPPC = pPC;
			debugger_instruction_hook(device->machine, PCD);
			ireg=M_RDOP(PCD);
			PC++;

			switch( ireg )
			{
				case 0x00: illegal(); break;
				case 0x01: nop(); break;
				case 0x02: illegal(); break;
				case 0x03: illegal(); break;
				case 0x04: illegal(); break;
				case 0x05: illegal(); break;
				case 0x06: tap(); break;
				case 0x07: tpa(); break;
				case 0x08: inx(); break;
				case 0x09: dex(); break;
				case 0x0A: CLV; break;
				case 0x0B: SEV; break;
				case 0x0C: CLC; break;
				case 0x0D: SEC; break;
				case 0x0E: cli(); break;
				case 0x0F: sei(); break;
				case 0x10: sba(); break;
				case 0x11: cba(); break;
				case 0x12: illegal(); break;
				case 0x13: illegal(); break;
				case 0x14: illegal(); break;
				case 0x15: illegal(); break;
				case 0x16: tab(); break;
				case 0x17: tba(); break;
				case 0x18: illegal(); break;
				case 0x19: daa(); break;
				case 0x1a: illegal(); break;
				case 0x1b: aba(); break;
				case 0x1c: illegal(); break;
				case 0x1d: illegal(); break;
				case 0x1e: illegal(); break;
				case 0x1f: illegal(); break;
				case 0x20: bra(); break;
				case 0x21: brn(); break;
				case 0x22: bhi(); break;
				case 0x23: bls(); break;
				case 0x24: bcc(); break;
				case 0x25: bcs(); break;
				case 0x26: bne(); break;
				case 0x27: beq(); break;
				case 0x28: bvc(); break;
				case 0x29: bvs(); break;
				case 0x2a: bpl(); break;
				case 0x2b: bmi(); break;
				case 0x2c: bge(); break;
				case 0x2d: blt(); break;
				case 0x2e: bgt(); break;
				case 0x2f: ble(); break;
				case 0x30: tsx(); break;
				case 0x31: ins(); break;
				case 0x32: pula(); break;
				case 0x33: pulb(); break;
				case 0x34: des(); break;
				case 0x35: txs(); break;
				case 0x36: psha(); break;
				case 0x37: pshb(); break;
				case 0x38: illegal(); break;
				case 0x39: rts(); break;
				case 0x3a: illegal(); break;
				case 0x3b: rti(); break;
				case 0x3c: illegal(); break;
				case 0x3d: illegal(); break;
				case 0x3e: wai(); break;
				case 0x3f: swi(); break;
				case 0x40: nega(); break;
				case 0x41: illegal(); break;
				case 0x42: illegal(); break;
				case 0x43: coma(); break;
				case 0x44: lsra(); break;
				case 0x45: illegal(); break;
				case 0x46: rora(); break;
				case 0x47: asra(); break;
				case 0x48: asla(); break;
				case 0x49: rola(); break;
				case 0x4a: deca(); break;
				case 0x4b: illegal(); break;
				case 0x4c: inca(); break;
				case 0x4d: tsta(); break;
				case 0x4e: illegal(); break;
				case 0x4f: clra(); break;
				case 0x50: negb(); break;
				case 0x51: illegal(); break;
				case 0x52: illegal(); break;
				case 0x53: comb(); break;
				case 0x54: lsrb(); break;
				case 0x55: illegal(); break;
				case 0x56: rorb(); break;
				case 0x57: asrb(); break;
				case 0x58: aslb(); break;
				case 0x59: rolb(); break;
				case 0x5a: decb(); break;
				case 0x5b: illegal(); break;
				case 0x5c: incb(); break;
				case 0x5d: tstb(); break;
				case 0x5e: illegal(); break;
				case 0x5f: clrb(); break;
				case 0x60: neg_ix(); break;
				case 0x61: illegal(); break;
				case 0x62: illegal(); break;
				case 0x63: com_ix(); break;
				case 0x64: lsr_ix(); break;
				case 0x65: illegal(); break;
				case 0x66: ror_ix(); break;
				case 0x67: asr_ix(); break;
				case 0x68: asl_ix(); break;
				case 0x69: rol_ix(); break;
				case 0x6a: dec_ix(); break;
				case 0x6b: illegal(); break;
				case 0x6c: inc_ix(); break;
				case 0x6d: tst_ix(); break;
				case 0x6e: jmp_ix(); break;
				case 0x6f: clr_ix(); break;
				case 0x70: neg_ex(); break;
				case 0x71: illegal(); break;
				case 0x72: illegal(); break;
				case 0x73: com_ex(); break;
				case 0x74: lsr_ex(); break;
				case 0x75: illegal(); break;
				case 0x76: ror_ex(); break;
				case 0x77: asr_ex(); break;
				case 0x78: asl_ex(); break;
				case 0x79: rol_ex(); break;
				case 0x7a: dec_ex(); break;
				case 0x7b: illegal(); break;
				case 0x7c: inc_ex(); break;
				case 0x7d: tst_ex(); break;
				case 0x7e: jmp_ex(); break;
				case 0x7f: clr_ex(); break;
				case 0x80: suba_im(); break;
				case 0x81: cmpa_im(); break;
				case 0x82: sbca_im(); break;
				case 0x83: illegal(); break;
				case 0x84: anda_im(); break;
				case 0x85: bita_im(); break;
				case 0x86: lda_im(); break;
				case 0x87: sta_im(); break;
				case 0x88: eora_im(); break;
				case 0x89: adca_im(); break;
				case 0x8a: ora_im(); break;
				case 0x8b: adda_im(); break;
				case 0x8c: cmpx_im(); break;
				case 0x8d: bsr(); break;
				case 0x8e: lds_im(); break;
				case 0x8f: sts_im(); /* orthogonality */ break;
				case 0x90: suba_di(); break;
				case 0x91: cmpa_di(); break;
				case 0x92: sbca_di(); break;
				case 0x93: illegal(); break;
				case 0x94: anda_di(); break;
				case 0x95: bita_di(); break;
				case 0x96: lda_di(); break;
				case 0x97: sta_di(); break;
				case 0x98: eora_di(); break;
				case 0x99: adca_di(); break;
				case 0x9a: ora_di(); break;
				case 0x9b: adda_di(); break;
				case 0x9c: cmpx_di(); break;
				case 0x9d: jsr_di(); break;
				case 0x9e: lds_di(); break;
				case 0x9f: sts_di(); break;
				case 0xa0: suba_ix(); break;
				case 0xa1: cmpa_ix(); break;
				case 0xa2: sbca_ix(); break;
				case 0xa3: illegal(); break;
				case 0xa4: anda_ix(); break;
				case 0xa5: bita_ix(); break;
				case 0xa6: lda_ix(); break;
				case 0xa7: sta_ix(); break;
				case 0xa8: eora_ix(); break;
				case 0xa9: adca_ix(); break;
				case 0xaa: ora_ix(); break;
				case 0xab: adda_ix(); break;
				case 0xac: cmpx_ix(); break;
				case 0xad: jsr_ix(); break;
				case 0xae: lds_ix(); break;
				case 0xaf: sts_ix(); break;
				case 0xb0: suba_ex(); break;
				case 0xb1: cmpa_ex(); break;
				case 0xb2: sbca_ex(); break;
				case 0xb3: illegal(); break;
				case 0xb4: anda_ex(); break;
				case 0xb5: bita_ex(); break;
				case 0xb6: lda_ex(); break;
				case 0xb7: sta_ex(); break;
				case 0xb8: eora_ex(); break;
				case 0xb9: adca_ex(); break;
				case 0xba: ora_ex(); break;
				case 0xbb: adda_ex(); break;
				case 0xbc: cmpx_ex(); break;
				case 0xbd: jsr_ex(); break;
				case 0xbe: lds_ex(); break;
				case 0xbf: sts_ex(); break;
				case 0xc0: subb_im(); break;
				case 0xc1: cmpb_im(); break;
				case 0xc2: sbcb_im(); break;
				case 0xc3: illegal(); break;
				case 0xc4: andb_im(); break;
				case 0xc5: bitb_im(); break;
				case 0xc6: ldb_im(); break;
				case 0xc7: stb_im(); break;
				case 0xc8: eorb_im(); break;
				case 0xc9: adcb_im(); break;
				case 0xca: orb_im(); break;
				case 0xcb: addb_im(); break;
				case 0xcc: illegal(); break;
				case 0xcd: illegal(); break;
				case 0xce: ldx_im(); break;
				case 0xcf: stx_im(); break;
				case 0xd0: subb_di(); break;
				case 0xd1: cmpb_di(); break;
				case 0xd2: sbcb_di(); break;
				case 0xd3: illegal(); break;
				case 0xd4: andb_di(); break;
				case 0xd5: bitb_di(); break;
				case 0xd6: ldb_di(); break;
				case 0xd7: stb_di(); break;
				case 0xd8: eorb_di(); break;
				case 0xd9: adcb_di(); break;
				case 0xda: orb_di(); break;
				case 0xdb: addb_di(); break;
				case 0xdc: illegal(); break;
				case 0xdd: illegal(); break;
				case 0xde: ldx_di(); break;
				case 0xdf: stx_di(); break;
				case 0xe0: subb_ix(); break;
				case 0xe1: cmpb_ix(); break;
				case 0xe2: sbcb_ix(); break;
				case 0xe3: illegal(); break;
				case 0xe4: andb_ix(); break;
				case 0xe5: bitb_ix(); break;
				case 0xe6: ldb_ix(); break;
				case 0xe7: stb_ix(); break;
				case 0xe8: eorb_ix(); break;
				case 0xe9: adcb_ix(); break;
				case 0xea: orb_ix(); break;
				case 0xeb: addb_ix(); break;
				case 0xec: illegal(); break;
				case 0xed: illegal(); break;
				case 0xee: ldx_ix(); break;
				case 0xef: stx_ix(); break;
				case 0xf0: subb_ex(); break;
				case 0xf1: cmpb_ex(); break;
				case 0xf2: sbcb_ex(); break;
				case 0xf3: illegal(); break;
				case 0xf4: andb_ex(); break;
				case 0xf5: bitb_ex(); break;
				case 0xf6: ldb_ex(); break;
				case 0xf7: stb_ex(); break;
				case 0xf8: eorb_ex(); break;
				case 0xf9: adcb_ex(); break;
				case 0xfa: orb_ex(); break;
				case 0xfb: addb_ex(); break;
				case 0xfc: addx_ex(); break;
				case 0xfd: illegal(); break;
				case 0xfe: ldx_ex(); break;
				case 0xff: stx_ex(); break;
			}
			INCREMENT_COUNTER(cycles_6800[ireg]);
		}
	} while( m6800_ICount>0 );

	INCREMENT_COUNTER(m68xx.extra_cycles);
	m68xx.extra_cycles = 0;

	return cycles - m6800_ICount;
}

/****************************************************************************
 * M6801 almost (fully?) equal to the M6803
 ****************************************************************************/
#if (HAS_M6801)
static CPU_INIT( m6801 )
{
//  m68xx.subtype = SUBTYPE_M6801;
	m68xx.insn = m6803_insn;
	m68xx.cycles = cycles_6803;
	m68xx.irq_callback = irqcallback;
	m68xx.device = device;

	m68xx.clock = clock;
	m6800_rx_timer = timer_alloc(m6800_rx_tick, NULL);
	m6800_tx_timer = timer_alloc(m6800_tx_tick, NULL);

	state_register("m6801", index);
}
#endif

/****************************************************************************
 * M6802 almost (fully?) equal to the M6800
 ****************************************************************************/
#if (HAS_M6802)
static CPU_INIT( m6802 )
{
//  m68xx.subtype   = SUBTYPE_M6802;
	m68xx.insn = m6800_insn;
	m68xx.cycles = cycles_6800;
	m68xx.irq_callback = irqcallback;
	m68xx.device = device;
	state_register("m6802", index);
}
#endif

/****************************************************************************
 * M6803 almost (fully?) equal to the M6801
 ****************************************************************************/
#if (HAS_M6803)
static CPU_INIT( m6803 )
{
//  m68xx.subtype = SUBTYPE_M6803;
	m68xx.insn = m6803_insn;
	m68xx.cycles = cycles_6803;
	m68xx.irq_callback = irqcallback;
	m68xx.device = device;

	m68xx.clock = clock;
	m6800_rx_timer = timer_alloc(m6800_rx_tick, NULL);
	m6800_tx_timer = timer_alloc(m6800_tx_tick, NULL);

	state_register("m6803", index);
}
#endif

/****************************************************************************
 * Execute cycles CPU cycles. Return number of cycles really executed
 ****************************************************************************/
#if (HAS_M6803||HAS_M6801)
static CPU_EXECUTE( m6803 )
{
	UINT8 ireg;
	m6800_ICount = cycles;

	CLEANUP_conters;
	INCREMENT_COUNTER(m68xx.extra_cycles);
	m68xx.extra_cycles = 0;

	do
	{
		if( m68xx.wai_state & M6800_WAI )
		{
			EAT_CYCLES;
		}
		else
		{
			pPPC = pPC;
			debugger_instruction_hook(device->machine, PCD);
			ireg=M_RDOP(PCD);
			PC++;

			switch( ireg )
			{
				case 0x00: illegal(); break;
				case 0x01: nop(); break;
				case 0x02: illegal(); break;
				case 0x03: illegal(); break;
				case 0x04: lsrd(); /* 6803 only */; break;
				case 0x05: asld(); /* 6803 only */; break;
				case 0x06: tap(); break;
				case 0x07: tpa(); break;
				case 0x08: inx(); break;
				case 0x09: dex(); break;
				case 0x0A: CLV; break;
				case 0x0B: SEV; break;
				case 0x0C: CLC; break;
				case 0x0D: SEC; break;
				case 0x0E: cli(); break;
				case 0x0F: sei(); break;
				case 0x10: sba(); break;
				case 0x11: cba(); break;
				case 0x12: illegal(); break;
				case 0x13: illegal(); break;
				case 0x14: illegal(); break;
				case 0x15: illegal(); break;
				case 0x16: tab(); break;
				case 0x17: tba(); break;
				case 0x18: illegal(); break;
				case 0x19: daa(); break;
				case 0x1a: illegal(); break;
				case 0x1b: aba(); break;
				case 0x1c: illegal(); break;
				case 0x1d: illegal(); break;
				case 0x1e: illegal(); break;
				case 0x1f: illegal(); break;
				case 0x20: bra(); break;
				case 0x21: brn(); break;
				case 0x22: bhi(); break;
				case 0x23: bls(); break;
				case 0x24: bcc(); break;
				case 0x25: bcs(); break;
				case 0x26: bne(); break;
				case 0x27: beq(); break;
				case 0x28: bvc(); break;
				case 0x29: bvs(); break;
				case 0x2a: bpl(); break;
				case 0x2b: bmi(); break;
				case 0x2c: bge(); break;
				case 0x2d: blt(); break;
				case 0x2e: bgt(); break;
				case 0x2f: ble(); break;
				case 0x30: tsx(); break;
				case 0x31: ins(); break;
				case 0x32: pula(); break;
				case 0x33: pulb(); break;
				case 0x34: des(); break;
				case 0x35: txs(); break;
				case 0x36: psha(); break;
				case 0x37: pshb(); break;
				case 0x38: pulx(); /* 6803 only */ break;
				case 0x39: rts(); break;
				case 0x3a: abx(); /* 6803 only */ break;
				case 0x3b: rti(); break;
				case 0x3c: pshx(); /* 6803 only */ break;
				case 0x3d: mul(); /* 6803 only */ break;
				case 0x3e: wai(); break;
				case 0x3f: swi(); break;
				case 0x40: nega(); break;
				case 0x41: illegal(); break;
				case 0x42: illegal(); break;
				case 0x43: coma(); break;
				case 0x44: lsra(); break;
				case 0x45: illegal(); break;
				case 0x46: rora(); break;
				case 0x47: asra(); break;
				case 0x48: asla(); break;
				case 0x49: rola(); break;
				case 0x4a: deca(); break;
				case 0x4b: illegal(); break;
				case 0x4c: inca(); break;
				case 0x4d: tsta(); break;
				case 0x4e: illegal(); break;
				case 0x4f: clra(); break;
				case 0x50: negb(); break;
				case 0x51: illegal(); break;
				case 0x52: illegal(); break;
				case 0x53: comb(); break;
				case 0x54: lsrb(); break;
				case 0x55: illegal(); break;
				case 0x56: rorb(); break;
				case 0x57: asrb(); break;
				case 0x58: aslb(); break;
				case 0x59: rolb(); break;
				case 0x5a: decb(); break;
				case 0x5b: illegal(); break;
				case 0x5c: incb(); break;
				case 0x5d: tstb(); break;
				case 0x5e: illegal(); break;
				case 0x5f: clrb(); break;
				case 0x60: neg_ix(); break;
				case 0x61: illegal(); break;
				case 0x62: illegal(); break;
				case 0x63: com_ix(); break;
				case 0x64: lsr_ix(); break;
				case 0x65: illegal(); break;
				case 0x66: ror_ix(); break;
				case 0x67: asr_ix(); break;
				case 0x68: asl_ix(); break;
				case 0x69: rol_ix(); break;
				case 0x6a: dec_ix(); break;
				case 0x6b: illegal(); break;
				case 0x6c: inc_ix(); break;
				case 0x6d: tst_ix(); break;
				case 0x6e: jmp_ix(); break;
				case 0x6f: clr_ix(); break;
				case 0x70: neg_ex(); break;
				case 0x71: illegal(); break;
				case 0x72: illegal(); break;
				case 0x73: com_ex(); break;
				case 0x74: lsr_ex(); break;
				case 0x75: illegal(); break;
				case 0x76: ror_ex(); break;
				case 0x77: asr_ex(); break;
				case 0x78: asl_ex(); break;
				case 0x79: rol_ex(); break;
				case 0x7a: dec_ex(); break;
				case 0x7b: illegal(); break;
				case 0x7c: inc_ex(); break;
				case 0x7d: tst_ex(); break;
				case 0x7e: jmp_ex(); break;
				case 0x7f: clr_ex(); break;
				case 0x80: suba_im(); break;
				case 0x81: cmpa_im(); break;
				case 0x82: sbca_im(); break;
				case 0x83: subd_im(); /* 6803 only */ break;
				case 0x84: anda_im(); break;
				case 0x85: bita_im(); break;
				case 0x86: lda_im(); break;
				case 0x87: sta_im(); break;
				case 0x88: eora_im(); break;
				case 0x89: adca_im(); break;
				case 0x8a: ora_im(); break;
				case 0x8b: adda_im(); break;
				case 0x8c: cpx_im(); /* 6803 difference */ break;
				case 0x8d: bsr(); break;
				case 0x8e: lds_im(); break;
				case 0x8f: sts_im(); /* orthogonality */ break;
				case 0x90: suba_di(); break;
				case 0x91: cmpa_di(); break;
				case 0x92: sbca_di(); break;
				case 0x93: subd_di(); /* 6803 only */ break;
				case 0x94: anda_di(); break;
				case 0x95: bita_di(); break;
				case 0x96: lda_di(); break;
				case 0x97: sta_di(); break;
				case 0x98: eora_di(); break;
				case 0x99: adca_di(); break;
				case 0x9a: ora_di(); break;
				case 0x9b: adda_di(); break;
				case 0x9c: cpx_di(); /* 6803 difference */ break;
				case 0x9d: jsr_di(); break;
				case 0x9e: lds_di(); break;
				case 0x9f: sts_di(); break;
				case 0xa0: suba_ix(); break;
				case 0xa1: cmpa_ix(); break;
				case 0xa2: sbca_ix(); break;
				case 0xa3: subd_ix(); /* 6803 only */ break;
				case 0xa4: anda_ix(); break;
				case 0xa5: bita_ix(); break;
				case 0xa6: lda_ix(); break;
				case 0xa7: sta_ix(); break;
				case 0xa8: eora_ix(); break;
				case 0xa9: adca_ix(); break;
				case 0xaa: ora_ix(); break;
				case 0xab: adda_ix(); break;
				case 0xac: cpx_ix(); /* 6803 difference */ break;
				case 0xad: jsr_ix(); break;
				case 0xae: lds_ix(); break;
				case 0xaf: sts_ix(); break;
				case 0xb0: suba_ex(); break;
				case 0xb1: cmpa_ex(); break;
				case 0xb2: sbca_ex(); break;
				case 0xb3: subd_ex(); /* 6803 only */ break;
				case 0xb4: anda_ex(); break;
				case 0xb5: bita_ex(); break;
				case 0xb6: lda_ex(); break;
				case 0xb7: sta_ex(); break;
				case 0xb8: eora_ex(); break;
				case 0xb9: adca_ex(); break;
				case 0xba: ora_ex(); break;
				case 0xbb: adda_ex(); break;
				case 0xbc: cpx_ex(); /* 6803 difference */ break;
				case 0xbd: jsr_ex(); break;
				case 0xbe: lds_ex(); break;
				case 0xbf: sts_ex(); break;
				case 0xc0: subb_im(); break;
				case 0xc1: cmpb_im(); break;
				case 0xc2: sbcb_im(); break;
				case 0xc3: addd_im(); /* 6803 only */ break;
				case 0xc4: andb_im(); break;
				case 0xc5: bitb_im(); break;
				case 0xc6: ldb_im(); break;
				case 0xc7: stb_im(); break;
				case 0xc8: eorb_im(); break;
				case 0xc9: adcb_im(); break;
				case 0xca: orb_im(); break;
				case 0xcb: addb_im(); break;
				case 0xcc: ldd_im(); /* 6803 only */ break;
				case 0xcd: std_im(); /* 6803 only -- orthogonality */ break;
				case 0xce: ldx_im(); break;
				case 0xcf: stx_im(); break;
				case 0xd0: subb_di(); break;
				case 0xd1: cmpb_di(); break;
				case 0xd2: sbcb_di(); break;
				case 0xd3: addd_di(); /* 6803 only */ break;
				case 0xd4: andb_di(); break;
				case 0xd5: bitb_di(); break;
				case 0xd6: ldb_di(); break;
				case 0xd7: stb_di(); break;
				case 0xd8: eorb_di(); break;
				case 0xd9: adcb_di(); break;
				case 0xda: orb_di(); break;
				case 0xdb: addb_di(); break;
				case 0xdc: ldd_di(); /* 6803 only */ break;
				case 0xdd: std_di(); /* 6803 only */ break;
				case 0xde: ldx_di(); break;
				case 0xdf: stx_di(); break;
				case 0xe0: subb_ix(); break;
				case 0xe1: cmpb_ix(); break;
				case 0xe2: sbcb_ix(); break;
				case 0xe3: addd_ix(); /* 6803 only */ break;
				case 0xe4: andb_ix(); break;
				case 0xe5: bitb_ix(); break;
				case 0xe6: ldb_ix(); break;
				case 0xe7: stb_ix(); break;
				case 0xe8: eorb_ix(); break;
				case 0xe9: adcb_ix(); break;
				case 0xea: orb_ix(); break;
				case 0xeb: addb_ix(); break;
				case 0xec: ldd_ix(); /* 6803 only */ break;
				case 0xed: std_ix(); /* 6803 only */ break;
				case 0xee: ldx_ix(); break;
				case 0xef: stx_ix(); break;
				case 0xf0: subb_ex(); break;
				case 0xf1: cmpb_ex(); break;
				case 0xf2: sbcb_ex(); break;
				case 0xf3: addd_ex(); /* 6803 only */ break;
				case 0xf4: andb_ex(); break;
				case 0xf5: bitb_ex(); break;
				case 0xf6: ldb_ex(); break;
				case 0xf7: stb_ex(); break;
				case 0xf8: eorb_ex(); break;
				case 0xf9: adcb_ex(); break;
				case 0xfa: orb_ex(); break;
				case 0xfb: addb_ex(); break;
				case 0xfc: ldd_ex(); /* 6803 only */ break;
				case 0xfd: std_ex(); /* 6803 only */ break;
				case 0xfe: ldx_ex(); break;
				case 0xff: stx_ex(); break;
			}
			INCREMENT_COUNTER(cycles_6803[ireg]);
		}
	} while( m6800_ICount>0 );

	INCREMENT_COUNTER(m68xx.extra_cycles);
	m68xx.extra_cycles = 0;

	return cycles - m6800_ICount;
}
#endif

#if (HAS_M6803)

static READ8_HANDLER( m6803_internal_registers_r );
static WRITE8_HANDLER( m6803_internal_registers_w );

static ADDRESS_MAP_START(m6803_mem, ADDRESS_SPACE_PROGRAM, 8)
	AM_RANGE(0x0000, 0x001f) AM_READWRITE(m6803_internal_registers_r, m6803_internal_registers_w)
	AM_RANGE(0x0020, 0x007f) AM_NOP        /* unused */
	AM_RANGE(0x0080, 0x00ff) AM_RAM        /* 6803 internal RAM */
ADDRESS_MAP_END

#endif

/****************************************************************************
 * M6808 almost (fully?) equal to the M6800
 ****************************************************************************/
#if (HAS_M6808)
static CPU_INIT( m6808 )
{
//  m68xx.subtype = SUBTYPE_M6808;
	m68xx.insn = m6800_insn;
	m68xx.cycles = cycles_6800;
	m68xx.irq_callback = irqcallback;
	m68xx.device = device;
	state_register("m6808", index);
}
#endif

/****************************************************************************
 * HD63701 similiar to the M6800
 ****************************************************************************/
#if (HAS_HD63701)

static CPU_INIT( hd63701 )
{
//  m68xx.subtype = SUBTYPE_HD63701;
	m68xx.insn = hd63701_insn;
	m68xx.cycles = cycles_63701;
	m68xx.irq_callback = irqcallback;
	m68xx.device = device;

	m68xx.clock = clock;
	m6800_rx_timer = timer_alloc(m6800_rx_tick, NULL);
	m6800_tx_timer = timer_alloc(m6800_tx_tick, NULL);

	state_register("hd63701", index);
}
/****************************************************************************
 * Execute cycles CPU cycles. Return number of cycles really executed
 ****************************************************************************/
static CPU_EXECUTE( hd63701 )
{
	UINT8 ireg;
	m6800_ICount = cycles;

	CLEANUP_conters;
	INCREMENT_COUNTER(m68xx.extra_cycles);
	m68xx.extra_cycles = 0;

	do
	{
		if( m68xx.wai_state & (HD63701_WAI|HD63701_SLP) )
		{
			EAT_CYCLES;
		}
		else
		{
			pPPC = pPC;
			debugger_instruction_hook(device->machine, PCD);
			ireg=M_RDOP(PCD);
			PC++;

			switch( ireg )
			{
				case 0x00: trap(); break;
				case 0x01: nop(); break;
				case 0x02: trap(); break;
				case 0x03: trap(); break;
				case 0x04: lsrd(); /* 6803 only */; break;
				case 0x05: asld(); /* 6803 only */; break;
				case 0x06: tap(); break;
				case 0x07: tpa(); break;
				case 0x08: inx(); break;
				case 0x09: dex(); break;
				case 0x0A: CLV; break;
				case 0x0B: SEV; break;
				case 0x0C: CLC; break;
				case 0x0D: SEC; break;
				case 0x0E: cli(); break;
				case 0x0F: sei(); break;
				case 0x10: sba(); break;
				case 0x11: cba(); break;
				case 0x12: undoc1(); break;
				case 0x13: undoc2(); break;
				case 0x14: trap(); break;
				case 0x15: trap(); break;
				case 0x16: tab(); break;
				case 0x17: tba(); break;
				case 0x18: xgdx(); /* HD63701YO only */; break;
				case 0x19: daa(); break;
				case 0x1a: slp(); break;
				case 0x1b: aba(); break;
				case 0x1c: trap(); break;
				case 0x1d: trap(); break;
				case 0x1e: trap(); break;
				case 0x1f: trap(); break;
				case 0x20: bra(); break;
				case 0x21: brn(); break;
				case 0x22: bhi(); break;
				case 0x23: bls(); break;
				case 0x24: bcc(); break;
				case 0x25: bcs(); break;
				case 0x26: bne(); break;
				case 0x27: beq(); break;
				case 0x28: bvc(); break;
				case 0x29: bvs(); break;
				case 0x2a: bpl(); break;
				case 0x2b: bmi(); break;
				case 0x2c: bge(); break;
				case 0x2d: blt(); break;
				case 0x2e: bgt(); break;
				case 0x2f: ble(); break;
				case 0x30: tsx(); break;
				case 0x31: ins(); break;
				case 0x32: pula(); break;
				case 0x33: pulb(); break;
				case 0x34: des(); break;
				case 0x35: txs(); break;
				case 0x36: psha(); break;
				case 0x37: pshb(); break;
				case 0x38: pulx(); /* 6803 only */ break;
				case 0x39: rts(); break;
				case 0x3a: abx(); /* 6803 only */ break;
				case 0x3b: rti(); break;
				case 0x3c: pshx(); /* 6803 only */ break;
				case 0x3d: mul(); /* 6803 only */ break;
				case 0x3e: wai(); break;
				case 0x3f: swi(); break;
				case 0x40: nega(); break;
				case 0x41: trap(); break;
				case 0x42: trap(); break;
				case 0x43: coma(); break;
				case 0x44: lsra(); break;
				case 0x45: trap(); break;
				case 0x46: rora(); break;
				case 0x47: asra(); break;
				case 0x48: asla(); break;
				case 0x49: rola(); break;
				case 0x4a: deca(); break;
				case 0x4b: trap(); break;
				case 0x4c: inca(); break;
				case 0x4d: tsta(); break;
				case 0x4e: trap(); break;
				case 0x4f: clra(); break;
				case 0x50: negb(); break;
				case 0x51: trap(); break;
				case 0x52: trap(); break;
				case 0x53: comb(); break;
				case 0x54: lsrb(); break;
				case 0x55: trap(); break;
				case 0x56: rorb(); break;
				case 0x57: asrb(); break;
				case 0x58: aslb(); break;
				case 0x59: rolb(); break;
				case 0x5a: decb(); break;
				case 0x5b: trap(); break;
				case 0x5c: incb(); break;
				case 0x5d: tstb(); break;
				case 0x5e: trap(); break;
				case 0x5f: clrb(); break;
				case 0x60: neg_ix(); break;
				case 0x61: aim_ix(); /* HD63701YO only */; break;
				case 0x62: oim_ix(); /* HD63701YO only */; break;
				case 0x63: com_ix(); break;
				case 0x64: lsr_ix(); break;
				case 0x65: eim_ix(); /* HD63701YO only */; break;
				case 0x66: ror_ix(); break;
				case 0x67: asr_ix(); break;
				case 0x68: asl_ix(); break;
				case 0x69: rol_ix(); break;
				case 0x6a: dec_ix(); break;
				case 0x6b: tim_ix(); /* HD63701YO only */; break;
				case 0x6c: inc_ix(); break;
				case 0x6d: tst_ix(); break;
				case 0x6e: jmp_ix(); break;
				case 0x6f: clr_ix(); break;
				case 0x70: neg_ex(); break;
				case 0x71: aim_di(); /* HD63701YO only */; break;
				case 0x72: oim_di(); /* HD63701YO only */; break;
				case 0x73: com_ex(); break;
				case 0x74: lsr_ex(); break;
				case 0x75: eim_di(); /* HD63701YO only */; break;
				case 0x76: ror_ex(); break;
				case 0x77: asr_ex(); break;
				case 0x78: asl_ex(); break;
				case 0x79: rol_ex(); break;
				case 0x7a: dec_ex(); break;
				case 0x7b: tim_di(); /* HD63701YO only */; break;
				case 0x7c: inc_ex(); break;
				case 0x7d: tst_ex(); break;
				case 0x7e: jmp_ex(); break;
				case 0x7f: clr_ex(); break;
				case 0x80: suba_im(); break;
				case 0x81: cmpa_im(); break;
				case 0x82: sbca_im(); break;
				case 0x83: subd_im(); /* 6803 only */ break;
				case 0x84: anda_im(); break;
				case 0x85: bita_im(); break;
				case 0x86: lda_im(); break;
				case 0x87: sta_im(); break;
				case 0x88: eora_im(); break;
				case 0x89: adca_im(); break;
				case 0x8a: ora_im(); break;
				case 0x8b: adda_im(); break;
				case 0x8c: cpx_im(); /* 6803 difference */ break;
				case 0x8d: bsr(); break;
				case 0x8e: lds_im(); break;
				case 0x8f: sts_im(); /* orthogonality */ break;
				case 0x90: suba_di(); break;
				case 0x91: cmpa_di(); break;
				case 0x92: sbca_di(); break;
				case 0x93: subd_di(); /* 6803 only */ break;
				case 0x94: anda_di(); break;
				case 0x95: bita_di(); break;
				case 0x96: lda_di(); break;
				case 0x97: sta_di(); break;
				case 0x98: eora_di(); break;
				case 0x99: adca_di(); break;
				case 0x9a: ora_di(); break;
				case 0x9b: adda_di(); break;
				case 0x9c: cpx_di(); /* 6803 difference */ break;
				case 0x9d: jsr_di(); break;
				case 0x9e: lds_di(); break;
				case 0x9f: sts_di(); break;
				case 0xa0: suba_ix(); break;
				case 0xa1: cmpa_ix(); break;
				case 0xa2: sbca_ix(); break;
				case 0xa3: subd_ix(); /* 6803 only */ break;
				case 0xa4: anda_ix(); break;
				case 0xa5: bita_ix(); break;
				case 0xa6: lda_ix(); break;
				case 0xa7: sta_ix(); break;
				case 0xa8: eora_ix(); break;
				case 0xa9: adca_ix(); break;
				case 0xaa: ora_ix(); break;
				case 0xab: adda_ix(); break;
				case 0xac: cpx_ix(); /* 6803 difference */ break;
				case 0xad: jsr_ix(); break;
				case 0xae: lds_ix(); break;
				case 0xaf: sts_ix(); break;
				case 0xb0: suba_ex(); break;
				case 0xb1: cmpa_ex(); break;
				case 0xb2: sbca_ex(); break;
				case 0xb3: subd_ex(); /* 6803 only */ break;
				case 0xb4: anda_ex(); break;
				case 0xb5: bita_ex(); break;
				case 0xb6: lda_ex(); break;
				case 0xb7: sta_ex(); break;
				case 0xb8: eora_ex(); break;
				case 0xb9: adca_ex(); break;
				case 0xba: ora_ex(); break;
				case 0xbb: adda_ex(); break;
				case 0xbc: cpx_ex(); /* 6803 difference */ break;
				case 0xbd: jsr_ex(); break;
				case 0xbe: lds_ex(); break;
				case 0xbf: sts_ex(); break;
				case 0xc0: subb_im(); break;
				case 0xc1: cmpb_im(); break;
				case 0xc2: sbcb_im(); break;
				case 0xc3: addd_im(); /* 6803 only */ break;
				case 0xc4: andb_im(); break;
				case 0xc5: bitb_im(); break;
				case 0xc6: ldb_im(); break;
				case 0xc7: stb_im(); break;
				case 0xc8: eorb_im(); break;
				case 0xc9: adcb_im(); break;
				case 0xca: orb_im(); break;
				case 0xcb: addb_im(); break;
				case 0xcc: ldd_im(); /* 6803 only */ break;
				case 0xcd: std_im(); /* 6803 only -- orthogonality */ break;
				case 0xce: ldx_im(); break;
				case 0xcf: stx_im(); break;
				case 0xd0: subb_di(); break;
				case 0xd1: cmpb_di(); break;
				case 0xd2: sbcb_di(); break;
				case 0xd3: addd_di(); /* 6803 only */ break;
				case 0xd4: andb_di(); break;
				case 0xd5: bitb_di(); break;
				case 0xd6: ldb_di(); break;
				case 0xd7: stb_di(); break;
				case 0xd8: eorb_di(); break;
				case 0xd9: adcb_di(); break;
				case 0xda: orb_di(); break;
				case 0xdb: addb_di(); break;
				case 0xdc: ldd_di(); /* 6803 only */ break;
				case 0xdd: std_di(); /* 6803 only */ break;
				case 0xde: ldx_di(); break;
				case 0xdf: stx_di(); break;
				case 0xe0: subb_ix(); break;
				case 0xe1: cmpb_ix(); break;
				case 0xe2: sbcb_ix(); break;
				case 0xe3: addd_ix(); /* 6803 only */ break;
				case 0xe4: andb_ix(); break;
				case 0xe5: bitb_ix(); break;
				case 0xe6: ldb_ix(); break;
				case 0xe7: stb_ix(); break;
				case 0xe8: eorb_ix(); break;
				case 0xe9: adcb_ix(); break;
				case 0xea: orb_ix(); break;
				case 0xeb: addb_ix(); break;
				case 0xec: ldd_ix(); /* 6803 only */ break;
				case 0xed: std_ix(); /* 6803 only */ break;
				case 0xee: ldx_ix(); break;
				case 0xef: stx_ix(); break;
				case 0xf0: subb_ex(); break;
				case 0xf1: cmpb_ex(); break;
				case 0xf2: sbcb_ex(); break;
				case 0xf3: addd_ex(); /* 6803 only */ break;
				case 0xf4: andb_ex(); break;
				case 0xf5: bitb_ex(); break;
				case 0xf6: ldb_ex(); break;
				case 0xf7: stb_ex(); break;
				case 0xf8: eorb_ex(); break;
				case 0xf9: adcb_ex(); break;
				case 0xfa: orb_ex(); break;
				case 0xfb: addb_ex(); break;
				case 0xfc: ldd_ex(); /* 6803 only */ break;
				case 0xfd: std_ex(); /* 6803 only */ break;
				case 0xfe: ldx_ex(); break;
				case 0xff: stx_ex(); break;
			}
			INCREMENT_COUNTER(cycles_63701[ireg]);
		}
	} while( m6800_ICount>0 );

	INCREMENT_COUNTER(m68xx.extra_cycles);
	m68xx.extra_cycles = 0;

	return cycles - m6800_ICount;
}

/*
    if change_pc() direccted these areas ,Call hd63701_trap_pc().
    'mode' is selected by the sense of p2.0,p2.1,and p2.3 at reset timming.
    mode 0,1,2,4,6 : $0000-$001f
    mode 5         : $0000-$001f,$0200-$efff
    mode 7         : $0000-$001f,$0100-$efff
*/
void hd63701_trap_pc(void)
{
	TAKE_TRAP;
}

static READ8_HANDLER( m6803_internal_registers_r );
static WRITE8_HANDLER( m6803_internal_registers_w );

READ8_HANDLER( hd63701_internal_registers_r )
{
	return m6803_internal_registers_r(machine, offset);
}

WRITE8_HANDLER( hd63701_internal_registers_w )
{
	m6803_internal_registers_w(machine, offset,data);
}
#endif

/****************************************************************************
 * NSC-8105 similiar to the M6800, but the opcodes are scrambled and there
 * is at least one new opcode ($fc)
 ****************************************************************************/
#if (HAS_NSC8105)
static CPU_INIT( nsc8105 )
{
//  m68xx.subtype = SUBTYPE_NSC8105;
	m68xx.insn = nsc8105_insn;
	m68xx.cycles = cycles_nsc8105;
	state_register("nsc8105", index);
}
/****************************************************************************
 * Execute cycles CPU cycles. Return number of cycles really executed
 ****************************************************************************/
static CPU_EXECUTE( nsc8105 )
{
	UINT8 ireg;
	m6800_ICount = cycles;

	CLEANUP_conters;
	INCREMENT_COUNTER(m68xx.extra_cycles);
	m68xx.extra_cycles = 0;

	do
	{
		if( m68xx.wai_state & NSC8105_WAI )
		{
			EAT_CYCLES;
		}
		else
		{
			pPPC = pPC;
			debugger_instruction_hook(device->machine, PCD);
			ireg=M_RDOP(PCD);
			PC++;

			switch( ireg )
			{
				case 0x00: illegal(); break;
				case 0x01: illegal(); break;
				case 0x02: nop(); break;
				case 0x03: illegal(); break;
				case 0x04: illegal(); break;
				case 0x05: tap(); break;
				case 0x06: illegal(); break;
				case 0x07: tpa(); break;
				case 0x08: inx(); break;
				case 0x09: CLV; break;
				case 0x0a: dex(); break;
				case 0x0b: SEV; break;
				case 0x0c: CLC; break;
				case 0x0d: cli(); break;
				case 0x0e: SEC; break;
				case 0x0f: sei(); break;
				case 0x10: sba(); break;
				case 0x11: illegal(); break;
				case 0x12: cba(); break;
				case 0x13: illegal(); break;
				case 0x14: illegal(); break;
				case 0x15: tab(); break;
				case 0x16: illegal(); break;
				case 0x17: tba(); break;
				case 0x18: illegal(); break;
				case 0x19: illegal(); break;
				case 0x1a: daa(); break;
				case 0x1b: aba(); break;
				case 0x1c: illegal(); break;
				case 0x1d: illegal(); break;
				case 0x1e: illegal(); break;
				case 0x1f: illegal(); break;
				case 0x20: bra(); break;
				case 0x21: bhi(); break;
				case 0x22: brn(); break;
				case 0x23: bls(); break;
				case 0x24: bcc(); break;
				case 0x25: bne(); break;
				case 0x26: bcs(); break;
				case 0x27: beq(); break;
				case 0x28: bvc(); break;
				case 0x29: bpl(); break;
				case 0x2a: bvs(); break;
				case 0x2b: bmi(); break;
				case 0x2c: bge(); break;
				case 0x2d: bgt(); break;
				case 0x2e: blt(); break;
				case 0x2f: ble(); break;
				case 0x30: tsx(); break;
				case 0x31: pula(); break;
				case 0x32: ins(); break;
				case 0x33: pulb(); break;
				case 0x34: des(); break;
				case 0x35: psha(); break;
				case 0x36: txs(); break;
				case 0x37: pshb(); break;
				case 0x38: illegal(); break;
				case 0x39: illegal(); break;
				case 0x3a: rts(); break;
				case 0x3b: rti(); break;
				case 0x3c: illegal(); break;
				case 0x3d: wai(); break;
				case 0x3e: illegal(); break;
				case 0x3f: swi(); break;
				case 0x40: suba_im(); break;
				case 0x41: sbca_im(); break;
				case 0x42: cmpa_im(); break;
				case 0x43: illegal(); break;
				case 0x44: anda_im(); break;
				case 0x45: lda_im(); break;
				case 0x46: bita_im(); break;
				case 0x47: sta_im(); break;
				case 0x48: eora_im(); break;
				case 0x49: ora_im(); break;
				case 0x4a: adca_im(); break;
				case 0x4b: adda_im(); break;
				case 0x4c: cmpx_im(); break;
				case 0x4d: lds_im(); break;
				case 0x4e: bsr(); break;
				case 0x4f: sts_im(); /* orthogonality */ break;
				case 0x50: suba_di(); break;
				case 0x51: sbca_di(); break;
				case 0x52: cmpa_di(); break;
				case 0x53: illegal(); break;
				case 0x54: anda_di(); break;
				case 0x55: lda_di(); break;
				case 0x56: bita_di(); break;
				case 0x57: sta_di(); break;
				case 0x58: eora_di(); break;
				case 0x59: ora_di(); break;
				case 0x5a: adca_di(); break;
				case 0x5b: adda_di(); break;
				case 0x5c: cmpx_di(); break;
				case 0x5d: lds_di(); break;
				case 0x5e: jsr_di(); break;
				case 0x5f: sts_di(); break;
				case 0x60: suba_ix(); break;
				case 0x61: sbca_ix(); break;
				case 0x62: cmpa_ix(); break;
				case 0x63: illegal(); break;
				case 0x64: anda_ix(); break;
				case 0x65: lda_ix(); break;
				case 0x66: bita_ix(); break;
				case 0x67: sta_ix(); break;
				case 0x68: eora_ix(); break;
				case 0x69: ora_ix(); break;
				case 0x6a: adca_ix(); break;
				case 0x6b: adda_ix(); break;
				case 0x6c: cmpx_ix(); break;
				case 0x6d: lds_ix(); break;
				case 0x6e: jsr_ix(); break;
				case 0x6f: sts_ix(); break;
				case 0x70: suba_ex(); break;
				case 0x71: sbca_ex(); break;
				case 0x72: cmpa_ex(); break;
				case 0x73: illegal(); break;
				case 0x74: anda_ex(); break;
				case 0x75: lda_ex(); break;
				case 0x76: bita_ex(); break;
				case 0x77: sta_ex(); break;
				case 0x78: eora_ex(); break;
				case 0x79: ora_ex(); break;
				case 0x7a: adca_ex(); break;
				case 0x7b: adda_ex(); break;
				case 0x7c: cmpx_ex(); break;
				case 0x7d: lds_ex(); break;
				case 0x7e: jsr_ex(); break;
				case 0x7f: sts_ex(); break;
				case 0x80: nega(); break;
				case 0x81: illegal(); break;
				case 0x82: illegal(); break;
				case 0x83: coma(); break;
				case 0x84: lsra(); break;
				case 0x85: rora(); break;
				case 0x86: illegal(); break;
				case 0x87: asra(); break;
				case 0x88: asla(); break;
				case 0x89: deca(); break;
				case 0x8a: rola(); break;
				case 0x8b: illegal(); break;
				case 0x8c: inca(); break;
				case 0x8d: illegal(); break;
				case 0x8e: tsta(); break;
				case 0x8f: clra(); break;
				case 0x90: negb(); break;
				case 0x91: illegal(); break;
				case 0x92: illegal(); break;
				case 0x93: comb(); break;
				case 0x94: lsrb(); break;
				case 0x95: rorb(); break;
				case 0x96: illegal(); break;
				case 0x97: asrb(); break;
				case 0x98: aslb(); break;
				case 0x99: decb(); break;
				case 0x9a: rolb(); break;
				case 0x9b: illegal(); break;
				case 0x9c: incb(); break;
				case 0x9d: illegal(); break;
				case 0x9e: tstb(); break;
				case 0x9f: clrb(); break;
				case 0xa0: neg_ix(); break;
				case 0xa1: illegal(); break;
				case 0xa2: illegal(); break;
				case 0xa3: com_ix(); break;
				case 0xa4: lsr_ix(); break;
				case 0xa5: ror_ix(); break;
				case 0xa6: illegal(); break;
				case 0xa7: asr_ix(); break;
				case 0xa8: asl_ix(); break;
				case 0xa9: dec_ix(); break;
				case 0xaa: rol_ix(); break;
				case 0xab: illegal(); break;
				case 0xac: inc_ix(); break;
				case 0xad: jmp_ix(); break;
				case 0xae: tst_ix(); break;
				case 0xaf: clr_ix(); break;
				case 0xb0: neg_ex(); break;
				case 0xb1: illegal(); break;
				case 0xb2: illegal(); break;
				case 0xb3: com_ex(); break;
				case 0xb4: lsr_ex(); break;
				case 0xb5: ror_ex(); break;
				case 0xb6: illegal(); break;
				case 0xb7: asr_ex(); break;
				case 0xb8: asl_ex(); break;
				case 0xb9: dec_ex(); break;
				case 0xba: rol_ex(); break;
				case 0xbb: illegal(); break;
				case 0xbc: inc_ex(); break;
				case 0xbd: jmp_ex(); break;
				case 0xbe: tst_ex(); break;
				case 0xbf: clr_ex(); break;
				case 0xc0: subb_im(); break;
				case 0xc1: sbcb_im(); break;
				case 0xc2: cmpb_im(); break;
				case 0xc3: illegal(); break;
				case 0xc4: andb_im(); break;
				case 0xc5: ldb_im(); break;
				case 0xc6: bitb_im(); break;
				case 0xc7: stb_im(); break;
				case 0xc8: eorb_im(); break;
				case 0xc9: orb_im(); break;
				case 0xca: adcb_im(); break;
				case 0xcb: addb_im(); break;
				case 0xcc: illegal(); break;
				case 0xcd: ldx_im(); break;
				case 0xce: illegal(); break;
				case 0xcf: stx_im(); break;
				case 0xd0: subb_di(); break;
				case 0xd1: sbcb_di(); break;
				case 0xd2: cmpb_di(); break;
				case 0xd3: illegal(); break;
				case 0xd4: andb_di(); break;
				case 0xd5: ldb_di(); break;
				case 0xd6: bitb_di(); break;
				case 0xd7: stb_di(); break;
				case 0xd8: eorb_di(); break;
				case 0xd9: orb_di(); break;
				case 0xda: adcb_di(); break;
				case 0xdb: addb_di(); break;
				case 0xdc: illegal(); break;
				case 0xdd: ldx_di(); break;
				case 0xde: illegal(); break;
				case 0xdf: stx_di(); break;
				case 0xe0: subb_ix(); break;
				case 0xe1: sbcb_ix(); break;
				case 0xe2: cmpb_ix(); break;
				case 0xe3: illegal(); break;
				case 0xe4: andb_ix(); break;
				case 0xe5: ldb_ix(); break;
				case 0xe6: bitb_ix(); break;
				case 0xe7: stb_ix(); break;
				case 0xe8: eorb_ix(); break;
				case 0xe9: orb_ix(); break;
				case 0xea: adcb_ix(); break;
				case 0xeb: addb_ix(); break;
				case 0xec: adcx_im(); break; /* NSC8105 only */
				case 0xed: ldx_ix(); break;
				case 0xee: illegal(); break;
				case 0xef: stx_ix(); break;
				case 0xf0: subb_ex(); break;
				case 0xf1: sbcb_ex(); break;
				case 0xf2: cmpb_ex(); break;
				case 0xf3: illegal(); break;
				case 0xf4: andb_ex(); break;
				case 0xf5: ldb_ex(); break;
				case 0xf6: bitb_ex(); break;
				case 0xf7: stb_ex(); break;
				case 0xf8: eorb_ex(); break;
				case 0xf9: orb_ex(); break;
				case 0xfa: adcb_ex(); break;
				case 0xfb: addb_ex(); break;
				case 0xfc: addx_ex(); break;
				case 0xfd: ldx_ex(); break;
				case 0xfe: illegal(); break;
				case 0xff: stx_ex(); break;
			}
			INCREMENT_COUNTER(cycles_nsc8105[ireg]);
		}
	} while( m6800_ICount>0 );

	INCREMENT_COUNTER(m68xx.extra_cycles);
	m68xx.extra_cycles = 0;

	return cycles - m6800_ICount;
}
#endif


#if (HAS_M6803||HAS_HD63701)

static READ8_HANDLER( m6803_internal_registers_r )
{
	switch (offset)
	{
		case 0x00:
			return m68xx.port1_ddr;
		case 0x01:
			return m68xx.port2_ddr;
		case 0x02:
			return (io_read_byte_8be(M6803_PORT1) & (m68xx.port1_ddr ^ 0xff))
					| (m68xx.port1_data & m68xx.port1_ddr);
		case 0x03:
			return (io_read_byte_8be(M6803_PORT2) & (m68xx.port2_ddr ^ 0xff))
					| (m68xx.port2_data & m68xx.port2_ddr);
		case 0x04:
			return m68xx.port3_ddr;
		case 0x05:
			return m68xx.port4_ddr;
		case 0x06:
			return (io_read_byte_8be(M6803_PORT3) & (m68xx.port3_ddr ^ 0xff))
					| (m68xx.port3_data & m68xx.port3_ddr);
		case 0x07:
			return (io_read_byte_8be(M6803_PORT4) & (m68xx.port4_ddr ^ 0xff))
					| (m68xx.port4_data & m68xx.port4_ddr);
		case 0x08:
			m68xx.pending_tcsr = 0;
			return m68xx.tcsr;
		case 0x09:
			if(!(m68xx.pending_tcsr&TCSR_TOF))
			{
				m68xx.tcsr &= ~TCSR_TOF;
				MODIFIED_tcsr;
			}
			return m68xx.counter.b.h;
		case 0x0a:
			return m68xx.counter.b.l;
		case 0x0b:
			if(!(m68xx.pending_tcsr&TCSR_OCF))
			{
				m68xx.tcsr &= ~TCSR_OCF;
				MODIFIED_tcsr;
			}
			return m68xx.output_compare.b.h;
		case 0x0c:
			if(!(m68xx.pending_tcsr&TCSR_OCF))
			{
				m68xx.tcsr &= ~TCSR_OCF;
				MODIFIED_tcsr;
			}
			return m68xx.output_compare.b.l;
		case 0x0d:
			if(!(m68xx.pending_tcsr&TCSR_ICF))
			{
				m68xx.tcsr &= ~TCSR_ICF;
				MODIFIED_tcsr;
			}
			return (m68xx.input_capture >> 0) & 0xff;
		case 0x0e:
			return (m68xx.input_capture >> 8) & 0xff;
		case 0x0f:
			logerror("CPU #%d PC %04x: warning - read from unsupported register %02x\n",cpunum_get_active(),cpu_get_pc(machine->activecpu),offset);
			return 0;
		case 0x10:
			return m68xx.rmcr;
		case 0x11:
			m68xx.trcsr_read = 1;
			return m68xx.trcsr;
		case 0x12:
			if (m68xx.trcsr_read)
			{
				m68xx.trcsr_read = 0;
				m68xx.trcsr = m68xx.trcsr & 0x3f;
			}
			return m68xx.rdr;
		case 0x13:
			return m68xx.tdr;
		case 0x14:
			logerror("CPU #%d PC %04x: read RAM control register\n",cpunum_get_active(),cpu_get_pc(machine->activecpu));
			return m68xx.ram_ctrl;
		case 0x15:
		case 0x16:
		case 0x17:
		case 0x18:
		case 0x19:
		case 0x1a:
		case 0x1b:
		case 0x1c:
		case 0x1d:
		case 0x1e:
		case 0x1f:
		default:
			logerror("CPU #%d PC %04x: warning - read from reserved internal register %02x\n",cpunum_get_active(),cpu_get_pc(machine->activecpu),offset);
			return 0;
	}
}

static WRITE8_HANDLER( m6803_internal_registers_w )
{
	static int latch09;

	switch (offset)
	{
		case 0x00:
			if (m68xx.port1_ddr != data)
			{
				m68xx.port1_ddr = data;
				if(m68xx.port1_ddr == 0xff)
					io_write_byte_8be(M6803_PORT1,m68xx.port1_data);
				else
					io_write_byte_8be(M6803_PORT1,(m68xx.port1_data & m68xx.port1_ddr)
						| (io_read_byte_8be(M6803_PORT1) & (m68xx.port1_ddr ^ 0xff)));
			}
			break;
		case 0x01:
			if (m68xx.port2_ddr != data)
			{
				m68xx.port2_ddr = data;
				if(m68xx.port2_ddr == 0xff)
					io_write_byte_8be(M6803_PORT2,m68xx.port2_data);
				else
					io_write_byte_8be(M6803_PORT2,(m68xx.port2_data & m68xx.port2_ddr)
						| (io_read_byte_8be(M6803_PORT2) & (m68xx.port2_ddr ^ 0xff)));

				if (m68xx.port2_ddr & 2)
					logerror("CPU #%d PC %04x: warning - port 2 bit 1 set as output (OLVL) - not supported\n",cpunum_get_active(),cpu_get_pc(machine->activecpu));
			}
			break;
		case 0x02:
			m68xx.port1_data = data;
			if(m68xx.port1_ddr == 0xff)
				io_write_byte_8be(M6803_PORT1,m68xx.port1_data);
			else
				io_write_byte_8be(M6803_PORT1,(m68xx.port1_data & m68xx.port1_ddr)
					| (io_read_byte_8be(M6803_PORT1) & (m68xx.port1_ddr ^ 0xff)));
			break;
		case 0x03:
			if (m68xx.trcsr & M6800_TRCSR_TE)
			{
				m68xx.port2_data = (data & 0xef) | (m68xx.tx << 4);
			}
			else
			{
				m68xx.port2_data = data;
			}
			if(m68xx.port2_ddr == 0xff)
				io_write_byte_8be(M6803_PORT2,m68xx.port2_data);
			else
				io_write_byte_8be(M6803_PORT2,(m68xx.port2_data & m68xx.port2_ddr)
					| (io_read_byte_8be(M6803_PORT2) & (m68xx.port2_ddr ^ 0xff)));
			break;
		case 0x04:
			if (m68xx.port3_ddr != data)
			{
				m68xx.port3_ddr = data;
				if(m68xx.port3_ddr == 0xff)
					io_write_byte_8be(M6803_PORT3,m68xx.port3_data);
				else
					io_write_byte_8be(M6803_PORT3,(m68xx.port3_data & m68xx.port3_ddr)
						| (io_read_byte_8be(M6803_PORT3) & (m68xx.port3_ddr ^ 0xff)));
			}
			break;
		case 0x05:
			if (m68xx.port4_ddr != data)
			{
				m68xx.port4_ddr = data;
				if(m68xx.port4_ddr == 0xff)
					io_write_byte_8be(M6803_PORT4,m68xx.port4_data);
				else
					io_write_byte_8be(M6803_PORT4,(m68xx.port4_data & m68xx.port4_ddr)
						| (io_read_byte_8be(M6803_PORT4) & (m68xx.port4_ddr ^ 0xff)));
			}
			break;
		case 0x06:
			m68xx.port3_data = data;
			if(m68xx.port3_ddr == 0xff)
				io_write_byte_8be(M6803_PORT3,m68xx.port3_data);
			else
				io_write_byte_8be(M6803_PORT3,(m68xx.port3_data & m68xx.port3_ddr)
					| (io_read_byte_8be(M6803_PORT3) & (m68xx.port3_ddr ^ 0xff)));
			break;
		case 0x07:
			m68xx.port4_data = data;
			if(m68xx.port4_ddr == 0xff)
				io_write_byte_8be(M6803_PORT4,m68xx.port4_data);
			else
				io_write_byte_8be(M6803_PORT4,(m68xx.port4_data & m68xx.port4_ddr)
					| (io_read_byte_8be(M6803_PORT4) & (m68xx.port4_ddr ^ 0xff)));
			break;
		case 0x08:
			m68xx.tcsr = data;
			m68xx.pending_tcsr &= m68xx.tcsr;
			MODIFIED_tcsr;
			if( !(CC & 0x10) )
				m6800_check_irq2();
			break;
		case 0x09:
			latch09 = data & 0xff;	/* 6301 only */
			CT  = 0xfff8;
			TOH = CTH;
			MODIFIED_counters;
			break;
		case 0x0a:	/* 6301 only */
			CT = (latch09 << 8) | (data & 0xff);
			TOH = CTH;
			MODIFIED_counters;
			break;
		case 0x0b:
			if( m68xx.output_compare.b.h != data)
			{
				m68xx.output_compare.b.h = data;
				MODIFIED_counters;
			}
			break;
		case 0x0c:
			if( m68xx.output_compare.b.l != data)
			{
				m68xx.output_compare.b.l = data;
				MODIFIED_counters;
			}
			break;
		case 0x0d:
		case 0x0e:
		case 0x12:
			logerror("CPU #%d PC %04x: warning - write %02x to read only internal register %02x\n",cpunum_get_active(),cpu_get_pc(machine->activecpu),data,offset);
			break;
		case 0x0f:
			logerror("CPU #%d PC %04x: warning - write %02x to unsupported internal register %02x\n",cpunum_get_active(),cpu_get_pc(machine->activecpu),data,offset);
			break;
		case 0x10:
			m68xx.rmcr = data & 0x0f;

			switch ((m68xx.rmcr & M6800_RMCR_CC_MASK) >> 2)
			{
			case 0:
			case 3: // not implemented
				timer_enable(m6800_rx_timer, 0);
				timer_enable(m6800_tx_timer, 0);
				break;

			case 1:
			case 2:
				{
					int divisor = M6800_RMCR_SS[m68xx.rmcr & M6800_RMCR_SS_MASK];

					timer_adjust_periodic(m6800_rx_timer, attotime_zero, cpunum_get_active(), ATTOTIME_IN_HZ(m68xx.clock / divisor));
					timer_adjust_periodic(m6800_tx_timer, attotime_zero, cpunum_get_active(), ATTOTIME_IN_HZ(m68xx.clock / divisor));
				}
				break;
			}
			break;
		case 0x11:
			if ((data & M6800_TRCSR_TE) && !(m68xx.trcsr & M6800_TRCSR_TE))
			{
				m68xx.txstate = M6800_TX_STATE_INIT;
			}
			m68xx.trcsr = (m68xx.trcsr & 0xe0) | (data & 0x1f);
			break;
		case 0x13:
			if (m68xx.trcsr_read)
			{
				m68xx.trcsr_read = 0;
				m68xx.trcsr &= ~M6800_TRCSR_TDRE;
			}
			m68xx.tdr = data;
			break;
		case 0x14:
			logerror("CPU #%d PC %04x: write %02x to RAM control register\n",cpunum_get_active(),cpu_get_pc(machine->activecpu),data);
			m68xx.ram_ctrl = data;
			break;
		case 0x15:
		case 0x16:
		case 0x17:
		case 0x18:
		case 0x19:
		case 0x1a:
		case 0x1b:
		case 0x1c:
		case 0x1d:
		case 0x1e:
		case 0x1f:
		default:
			logerror("CPU #%d PC %04x: warning - write %02x to reserved internal register %02x\n",cpunum_get_active(),cpu_get_pc(machine->activecpu),data,offset);
			break;
	}
}
#endif


/**************************************************************************
 * Generic set_info
 **************************************************************************/

static CPU_SET_INFO( m6800 )
{
	switch (state)
	{
		/* --- the following bits of info are set as 64-bit signed integers --- */
		case CPUINFO_INT_INPUT_STATE + M6800_IRQ_LINE:	set_irq_line(M6800_IRQ_LINE, info->i);	break;
		case CPUINFO_INT_INPUT_STATE + M6800_TIN_LINE:	set_irq_line(M6800_TIN_LINE, info->i);	break;
		case CPUINFO_INT_INPUT_STATE + INPUT_LINE_NMI:	set_irq_line(INPUT_LINE_NMI, info->i);	break;

		case CPUINFO_INT_PC:							PC = info->i; CHANGE_PC();				break;
		case CPUINFO_INT_REGISTER + M6800_PC:			m68xx.pc.w.l = info->i;					break;
		case CPUINFO_INT_SP:							S = info->i;							break;
		case CPUINFO_INT_REGISTER + M6800_S:			m68xx.s.w.l = info->i;					break;
		case CPUINFO_INT_REGISTER + M6800_CC:			m68xx.cc = info->i;						break;
		case CPUINFO_INT_REGISTER + M6800_A:			m68xx.d.b.h = info->i;					break;
		case CPUINFO_INT_REGISTER + M6800_B:			m68xx.d.b.l = info->i;					break;
		case CPUINFO_INT_REGISTER + M6800_X:			m68xx.x.w.l = info->i;					break;
	}
}



/**************************************************************************
 * Generic get_info
 **************************************************************************/

CPU_GET_INFO( m6800 )
{
	switch (state)
	{
		/* --- the following bits of info are returned as 64-bit signed integers --- */
		case CPUINFO_INT_CONTEXT_SIZE:					info->i = sizeof(m68xx);				break;
		case CPUINFO_INT_INPUT_LINES:					info->i = 2;							break;
		case CPUINFO_INT_DEFAULT_IRQ_VECTOR:			info->i = 0;							break;
		case CPUINFO_INT_ENDIANNESS:					info->i = CPU_IS_BE;					break;
		case CPUINFO_INT_CLOCK_MULTIPLIER:				info->i = 1;							break;
		case CPUINFO_INT_CLOCK_DIVIDER:					info->i = 1;							break;
		case CPUINFO_INT_MIN_INSTRUCTION_BYTES:			info->i = 1;							break;
		case CPUINFO_INT_MAX_INSTRUCTION_BYTES:			info->i = 4;							break;
		case CPUINFO_INT_MIN_CYCLES:					info->i = 1;							break;
		case CPUINFO_INT_MAX_CYCLES:					info->i = 12;							break;

		case CPUINFO_INT_DATABUS_WIDTH + ADDRESS_SPACE_PROGRAM:	info->i = 8;					break;
		case CPUINFO_INT_ADDRBUS_WIDTH + ADDRESS_SPACE_PROGRAM: info->i = 16;					break;
		case CPUINFO_INT_ADDRBUS_SHIFT + ADDRESS_SPACE_PROGRAM: info->i = 0;					break;
		case CPUINFO_INT_DATABUS_WIDTH + ADDRESS_SPACE_DATA:	info->i = 0;					break;
		case CPUINFO_INT_ADDRBUS_WIDTH + ADDRESS_SPACE_DATA: 	info->i = 0;					break;
		case CPUINFO_INT_ADDRBUS_SHIFT + ADDRESS_SPACE_DATA: 	info->i = 0;					break;
		case CPUINFO_INT_DATABUS_WIDTH + ADDRESS_SPACE_IO:		info->i = 9;					break;
		case CPUINFO_INT_ADDRBUS_WIDTH + ADDRESS_SPACE_IO: 		info->i = 0;					break;
		case CPUINFO_INT_ADDRBUS_SHIFT + ADDRESS_SPACE_IO: 		info->i = 0;					break;

		case CPUINFO_INT_INPUT_STATE + M6800_IRQ_LINE:	info->i = m68xx.irq_state[M6800_IRQ_LINE]; break;
		case CPUINFO_INT_INPUT_STATE + M6800_TIN_LINE:	info->i = m68xx.irq_state[M6800_TIN_LINE]; break;
		case CPUINFO_INT_INPUT_STATE + INPUT_LINE_NMI:	info->i = m68xx.nmi_state;				break;

		case CPUINFO_INT_PREVIOUSPC:					info->i = m68xx.ppc.w.l;				break;

		case CPUINFO_INT_PC:							info->i = PC;							break;
		case CPUINFO_INT_REGISTER + M6800_PC:			info->i = m68xx.pc.w.l;					break;
		case CPUINFO_INT_SP:							info->i = S;							break;
		case CPUINFO_INT_REGISTER + M6800_S:			info->i = m68xx.s.w.l;					break;
		case CPUINFO_INT_REGISTER + M6800_CC:			info->i = m68xx.cc;						break;
		case CPUINFO_INT_REGISTER + M6800_A:			info->i = m68xx.d.b.h;					break;
		case CPUINFO_INT_REGISTER + M6800_B:			info->i = m68xx.d.b.l;					break;
		case CPUINFO_INT_REGISTER + M6800_X:			info->i = m68xx.x.w.l;					break;
		case CPUINFO_INT_REGISTER + M6800_WAI_STATE:	info->i = m68xx.wai_state;				break;

		/* --- the following bits of info are returned as pointers to data or functions --- */
		case CPUINFO_PTR_SET_INFO:						info->setinfo = CPU_SET_INFO_NAME(m6800);			break;
		case CPUINFO_PTR_GET_CONTEXT:					info->getcontext = CPU_GET_CONTEXT_NAME(m6800);	break;
		case CPUINFO_PTR_SET_CONTEXT:					info->setcontext = CPU_SET_CONTEXT_NAME(m6800);	break;
		case CPUINFO_PTR_INIT:							info->init = CPU_INIT_NAME(m6800);				break;
		case CPUINFO_PTR_RESET:							info->reset = CPU_RESET_NAME(m6800);				break;
		case CPUINFO_PTR_EXIT:							info->exit = CPU_EXIT_NAME(m6800);				break;
		case CPUINFO_PTR_EXECUTE:						info->execute = CPU_EXECUTE_NAME(m6800);			break;
		case CPUINFO_PTR_BURN:							info->burn = NULL;						break;
		case CPUINFO_PTR_DISASSEMBLE:					info->disassemble = CPU_DISASSEMBLE_NAME(m6800);			break;
		case CPUINFO_PTR_INSTRUCTION_COUNTER:			info->icount = &m6800_ICount;			break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case CPUINFO_STR_NAME:							strcpy(info->s, "M6800");				break;
		case CPUINFO_STR_CORE_FAMILY:					strcpy(info->s, "Motorola 6800");		break;
		case CPUINFO_STR_CORE_VERSION:					strcpy(info->s, "1.1");					break;
		case CPUINFO_STR_CORE_FILE:						strcpy(info->s, __FILE__);				break;
		case CPUINFO_STR_CORE_CREDITS:					strcpy(info->s, "The MAME team.");		break;

		case CPUINFO_STR_FLAGS:
			sprintf(info->s, "%c%c%c%c%c%c%c%c",
				m68xx.cc & 0x80 ? '?':'.',
				m68xx.cc & 0x40 ? '?':'.',
				m68xx.cc & 0x20 ? 'H':'.',
				m68xx.cc & 0x10 ? 'I':'.',
				m68xx.cc & 0x08 ? 'N':'.',
				m68xx.cc & 0x04 ? 'Z':'.',
				m68xx.cc & 0x02 ? 'V':'.',
				m68xx.cc & 0x01 ? 'C':'.');
			break;

		case CPUINFO_STR_REGISTER + M6800_A:			sprintf(info->s, "A:%02X", m68xx.d.b.h); break;
		case CPUINFO_STR_REGISTER + M6800_B:			sprintf(info->s, "B:%02X", m68xx.d.b.l); break;
		case CPUINFO_STR_REGISTER + M6800_PC:			sprintf(info->s, "PC:%04X", m68xx.pc.w.l); break;
		case CPUINFO_STR_REGISTER + M6800_S:			sprintf(info->s, "S:%04X", m68xx.s.w.l); break;
		case CPUINFO_STR_REGISTER + M6800_X:			sprintf(info->s, "X:%04X", m68xx.x.w.l); break;
		case CPUINFO_STR_REGISTER + M6800_CC:			sprintf(info->s, "CC:%02X", m68xx.cc); break;
		case CPUINFO_STR_REGISTER + M6800_WAI_STATE:	sprintf(info->s, "WAI:%X", m68xx.wai_state); break;
	}
}


#if (HAS_M6801)
/**************************************************************************
 * CPU-specific set_info
 **************************************************************************/

CPU_GET_INFO( m6801 )
{
	switch (state)
	{
		/* --- the following bits of info are returned as 64-bit signed integers --- */
		case CPUINFO_INT_CLOCK_DIVIDER:							info->i = 4;					break;
		case CPUINFO_INT_DATABUS_WIDTH + ADDRESS_SPACE_IO:		info->i = 8;					break;
		case CPUINFO_INT_ADDRBUS_WIDTH + ADDRESS_SPACE_IO: 		info->i = 9;					break;

		/* --- the following bits of info are returned as pointers to data or functions --- */
		case CPUINFO_PTR_INIT:							info->init = CPU_INIT_NAME(m6801);				break;
		case CPUINFO_PTR_EXECUTE:						info->execute = CPU_EXECUTE_NAME(m6803);			break;
		case CPUINFO_PTR_DISASSEMBLE:					info->disassemble = CPU_DISASSEMBLE_NAME(m6801);			break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case CPUINFO_STR_NAME:							strcpy(info->s, "M6801");				break;

		default:										CPU_GET_INFO_CALL(m6800);				break;
	}
}
#endif


#if (HAS_M6802)
/**************************************************************************
 * CPU-specific set_info
 **************************************************************************/

CPU_GET_INFO( m6802 )
{
	switch (state)
	{
		/* --- the following bits of info are returned as 64-bit signed integers --- */
		case CPUINFO_INT_CLOCK_DIVIDER:					info->i = 4;							break;

		/* --- the following bits of info are returned as pointers to data or functions --- */
		case CPUINFO_PTR_INIT:							info->init = CPU_INIT_NAME(m6802);				break;
		case CPUINFO_PTR_DISASSEMBLE:					info->disassemble = CPU_DISASSEMBLE_NAME(m6802);			break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case CPUINFO_STR_NAME:							strcpy(info->s, "M6802");				break;

		default:										CPU_GET_INFO_CALL(m6800);				break;
	}
}
#endif


#if (HAS_M6803)
/**************************************************************************
 * CPU-specific set_info
 **************************************************************************/

CPU_GET_INFO( m6803 )
{
	switch (state)
	{
		/* --- the following bits of info are returned as 64-bit signed integers --- */
		case CPUINFO_INT_CLOCK_DIVIDER:							info->i = 4;					break;
		case CPUINFO_INT_DATABUS_WIDTH + ADDRESS_SPACE_IO:		info->i = 8;					break;
		case CPUINFO_INT_ADDRBUS_WIDTH + ADDRESS_SPACE_IO: 		info->i = 9;					break;

		/* --- the following bits of info are returned as pointers to data or functions --- */
		case CPUINFO_PTR_INIT:							info->init = CPU_INIT_NAME(m6803);				break;
		case CPUINFO_PTR_EXECUTE:						info->execute = CPU_EXECUTE_NAME(m6803);			break;
		case CPUINFO_PTR_DISASSEMBLE:					info->disassemble = CPU_DISASSEMBLE_NAME(m6803);			break;

		case CPUINFO_PTR_INTERNAL_MEMORY_MAP + ADDRESS_SPACE_PROGRAM: info->internal_map8 = ADDRESS_MAP_NAME(m6803_mem); break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case CPUINFO_STR_NAME:							strcpy(info->s, "M6803");				break;

		default:										CPU_GET_INFO_CALL(m6800);				break;
	}
}
#endif


#if (HAS_M6808)
/**************************************************************************
 * CPU-specific set_info
 **************************************************************************/

CPU_GET_INFO( m6808 )
{
	switch (state)
	{
		/* --- the following bits of info are returned as 64-bit signed integers --- */
		case CPUINFO_INT_CLOCK_DIVIDER:					info->i = 4;							break;

		/* --- the following bits of info are returned as pointers to data or functions --- */
		case CPUINFO_PTR_INIT:							info->init = CPU_INIT_NAME(m6808);				break;
		case CPUINFO_PTR_DISASSEMBLE:					info->disassemble = CPU_DISASSEMBLE_NAME(m6808);			break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case CPUINFO_STR_NAME:							strcpy(info->s, "M6808");				break;

		default:										CPU_GET_INFO_CALL(m6800);				break;
	}
}
#endif


#if (HAS_HD63701)
/**************************************************************************
 * CPU-specific set_info
 **************************************************************************/

CPU_GET_INFO( hd63701 )
{
	switch (state)
	{
		/* --- the following bits of info are returned as 64-bit signed integers --- */
		case CPUINFO_INT_CLOCK_DIVIDER:							info->i = 4;					break;
		case CPUINFO_INT_DATABUS_WIDTH + ADDRESS_SPACE_IO:		info->i = 8;					break;
		case CPUINFO_INT_ADDRBUS_WIDTH + ADDRESS_SPACE_IO: 		info->i = 9;					break;

		/* --- the following bits of info are returned as pointers to data or functions --- */
		case CPUINFO_PTR_INIT:							info->init = CPU_INIT_NAME(hd63701);				break;
		case CPUINFO_PTR_EXECUTE:						info->execute = CPU_EXECUTE_NAME(hd63701);		break;
		case CPUINFO_PTR_DISASSEMBLE:					info->disassemble = CPU_DISASSEMBLE_NAME(hd63701);		break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case CPUINFO_STR_NAME:							strcpy(info->s, "HD63701");				break;

		default:										CPU_GET_INFO_CALL(m6800);				break;
	}
}
#endif


#if (HAS_NSC8105)
/**************************************************************************
 * CPU-specific set_info
 **************************************************************************/

CPU_GET_INFO( nsc8105 )
{
	switch (state)
	{
		/* --- the following bits of info are returned as 64-bit signed integers --- */
		case CPUINFO_INT_CLOCK_DIVIDER:					info->i = 4;							break;

		/* --- the following bits of info are returned as pointers to data or functions --- */
		case CPUINFO_PTR_INIT:							info->init = CPU_INIT_NAME(nsc8105);				break;
		case CPUINFO_PTR_EXECUTE:						info->execute = CPU_EXECUTE_NAME(nsc8105);		break;
		case CPUINFO_PTR_DISASSEMBLE:					info->disassemble = CPU_DISASSEMBLE_NAME(nsc8105);		break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case CPUINFO_STR_NAME:							strcpy(info->s, "NSC8105");				break;

		default:										CPU_GET_INFO_CALL(m6800);				break;
	}
}
#endif
