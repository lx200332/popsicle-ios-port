/*
 * The primary touch screen of the Xiaomi 17 Pro Max (popsicle): a Synaptics TCM
 * touch chip on the SPI bus of the serial engine qupv3_4 (se@1a80000, SE0), as a
 * source of touches for /dev/redwood-touch (sys/redwood_input.h). Only with
 * boot-arg syna=1 (the GT9615 second panel keeps touchscreen=1, per the port plan §5.5).
 *
 * Three sources, every block annotated in place:
 *  - the board machinery (TLMM, RPMh, GCC, GENI SPI in FIFO mode, the bit-bang
 *    fallback, the polled thread) is a port of bsd/dev/redwood_goodix.c; each block
 *    carries its source lines (redwood_goodix.c L45-69, L71-114, ...).
 *  - the touch protocol is the LineageOS sm8350 Synaptics TCM driver
 *    (drivers/input/touchscreen/synaptics_tcm/core.c, core.h, touch.c, spi.c,
 *    testing.c); each block carries its source lines (core.c:1167, core.h:66, ...).
 *  - the board values are from the popsicle device tree and the port plan
 *    (syna-tcm-移植计划.md §1/§4), every value annotated with its source.
 *
 * popsicle primary-screen board table:
 *  serial engine  0x1a80000, size 0x4000 (qupv3_4 wrap4 SE0)
 *                  DTB /soc/qcom,qupv3_4_geni_se@1ac0000/spi@1a80000: reg = <0x1a80000 0x4000>
 *  TLMM           0x0f100000 (TRUE base: pin table = 0x100000 + 0x1000*n, see DT_FINDINGS)  DT /soc/pinctrl@f000000
 *  GPIOs          MISO 48, MOSI 49, CLK 50, CS 51 (function qup4_se0_l0..l3),
 *                 INT 158 (active low), RESET 66 (active low), AVDD 117 (active high)
 *                 DTB pinctrl groups mi_ts_spi_miso/mosi/clk/cs_active (gpio48..51),
 *                 mi_ts_int_active, mi_ts_reset_active; the fixed regulator
 *                 /fragment@41/__overlay__/touch_avdd_vreg: gpio = <0x75 0> = GPIO 117.
 *                 The mux number for "qup4_se0" is 1: the same first non-GPIO slot
 *                 pattern as redwood/popsicle_goodix.c. VERIFY ON HARDWARE; the
 *                 bit-bang fallback needs no mux (function 0 = GPIO).
 *  supplies       iovdd = RPMh VRM "L4I_E0" 1800 mV (rpmh-regulator-l4i-e0,
 *                 regulator-pmh0110-i-l4), avdd = the fixed regulator
 *                 touch_avdd_vreg behind GPIO 117 (its voltage is not in the DTB;
 *                 Synaptics analog supply, typically 3.3 V).
 *  clocks         GCC 0x100000: the qupv3_4 branch enables share one CBCR at 0x52018
 *                 -- M_AHB bit 22 / halt 0x23548, S_AHB bit 23 / halt 0x2354c,
 *                 core bit 24 / halt 0x23550, SE0 bit 26 / halt 0xa9004; the SE0 RCG
 *                 cmd_rcgr sits at 0xa900c (CFG +4, DFSR +0x14, PERF_CFG/M/N
 *                 +0x1c/+0x5c/+0x9c per level). From the plan §1 (gcc-kaanapali.c
 *                 gcc_qupv3_wrap4_*). Note: the wrap3 pattern in popsicle_goodix.c
 *                 had cmd_rcgr at halt + 0x10; here the plan gives halt + 8 -- the
 *                 clocks_on() code reads the DFSR first and falls back to the
 *                 crystal, so a wrong address shows up as "no usable preset clock
 *                 rate" instead of a hang.
 *  panel          1200 x 2608 (DTB synaptics,max-x/max-y 0x4b0/0xa30), rx 18, tx 39
 *                 (DTB synaptics,rx-num/tx-num), fod window 597,2406 246x246
 *                 (DTB synaptics,fod-lx/ly/x-size/y-size; unused here, plan §6.9)
 *  SPI            up to 15 MHz (DTB synaptics,ubl-max-freq = 0xe4e1c0); first boot
 *                 5 MHz, conservative (plan §6.3)
 *  GIC SPI        158 (the DTB wires the touch interrupt as gpio 158 / GIC SPI 158;
 *                 this driver polls the line instead, like the Goodix one, plan §6.4)
 *  [TBD §6.10: the DTB's qts (Qualcomm Touchscreen) VM mode and the vm-gpio-list are
 *  hypervisor concerns; the bare-metal XNU boot has no VM to worry about]
 *
 * Power-up order (the plan's §4 sequence): hold reset (66) low, then iovdd (L4I_E0),
 * then avdd (GPIO 117), wait 50 ms (DTB synaptics,power-delay-ms = 0x32), release
 * reset, wait 200 ms (DTB synaptics,reset-delay-ms = 0xc8; the plan's §6.1 default
 * was 100 ms), then the clocks, then the SPI engine at 5 MHz, then the TCM probe.
 *
 * The first-boot flow (the plan's §3): IDENTIFY -> in APPLICATION, GET_APPLICATION_INFO;
 * in BOOTLOADER, RUN_APPLICATION_FIRMWARE + 100 ms + IDENTIFY again -> application
 * firmware with APP_STATUS_OK -> SET_TOUCH_REPORT_CONFIG with 12-bit x and y
 * (touch.c:875-891) -> ENABLE_REPORT -> poll REPORT_TOUCH.
 *
 * Not implemented, by the plan (§3.6/§3.7): reflash, zeroflash, recovery, testing,
 * the download protocols and host-download. A chip whose application status is
 * BAD_APP_CONFIG is reported and left alone (plan §6.6).
 *
 * The interrupt line is polled 125 times a second (boot-arg touch_poll_us=N), the
 * same thread shape as the Goodix driver; a failed probe terminates the thread
 * quietly, no screen is touched (plan §5.5). The plan §6 items that could not be
 * settled from the DTB are marked // [TBD §6.x] with the default chosen.
 *
 * Wiring (the plan §5.2-5.4): redwood_input.c:219 holds `redwood_goodix_init();`;
 * popsicle_syna_init() is called right after it.
 *
 * Register layouts: Linux drivers/spi/spi-geni-qcom.c, drivers/soc/qcom/{qcom-geni-se,
 * rpmh-rsc, cmd-db}.c, drivers/clk/qcom/gcc-kaanapali.c, drivers/pinctrl/qcom/
 * pinctrl-sm8850.c, drivers/input/touchscreen/synaptics_tcm/, and the phone's own
 * device tree (kaanapali-xiaomi-popsicle.dtb).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern void redwood_tl(const char *format, ...) __printflike(1, 2);

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

static vm_offset_t tlmm, gcc, se, rsc, cmd_db;

/* ---- registers, delays -------------------------------------------------------------------------
 * redwood_goodix.c L45-69 */

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- pins (TLMM) -------------------------------------------------------------------------------
 * redwood_goodix.c L71-114, with the popsicle values */

#define TLMM_PHYS               0x0f100000UL    /* TRUE base: pinctrl-canoe.ko pin table = 0x100000 + 0x1000*n (ios-pandora DT_FINDINGS.md:101); DT /soc/pinctrl@f000000 reg=<0x0f000000 0x202000> is the pre-offset base. 0x0f0xxxxx hangs the bus. */
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define PULL_NONE               0
#define PULL_UP                 3

/* DTB pinctrl@f000000: mi_ts_spi_miso/mosi/clk/cs_active (gpio48..51, "qup4_se0_l0..l3"),
 * mi_ts_reset_active (gpio66, "gpio"), mi_ts_int_active (gpio158, "gpio"), and the fixed
 * regulator touch_avdd_vreg (gpio 0x75 = 117). (redwood: 52..55/105/81/107) */
#define PIN_MISO                48
#define PIN_MOSI                49
#define PIN_CLK                 50
#define PIN_CS                  51
#define PIN_INT                 158     /* low = the chip has a report (DTB synaptics,irq-on-state = 0).
					 * [TBD §6.4: the interrupt polarity/edge is never used -- the driver
					 * polls the GPIO level, like the Goodix one] */
#define PIN_RESET               66      /* low = in reset */
#define PIN_AVDD                117     /* high = analog supply on */
#define PIN_FUNCTION_SPI        1       /* "qup4_se0": slot 1 of PINGROUP(48..51) in pinctrl-sm8850.c -- VERIFY */

static void
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
	wr(GPIO_CFG(pin), pull | function << 2 | (ma / 2 - 1) << 6 | (output ? 1u << 9 : 0));
}

static void
gpio_set(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), high ? 2 : 0);
}

static bool
gpio_get(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
gpio_output(unsigned pin, bool high, unsigned pull, unsigned ma)
{
	gpio_set(pin, high);            /* the level first, so that the pin never drives the wrong one */
	gpio_configure(pin, 0, pull, ma, true);
}

/* ---- the I/O supply (RPMh) -----------------------------------------------------------------------
 * redwood_goodix.c L116-261, with the popsicle resource */

#define CMD_DB_PHYS             0x81c60000UL    /* DTB aop_cmd_db_region@81c60000 (redwood: 0x80860000) */
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x18920000UL    /* DTB rsc@18900000, third reg = drv-2 (redwood: 0x18220000) */
#define RSC_SIZE                0x10000UL
#define TCS(reg)                (rsc + 0xd00 + (reg))           /* DTB drv@2: qcom,tcs-offset = 13 slots x 0x100 */
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18            /* 0 while busy */
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100

#define VRM_VOLTAGE             0               /* a regulator's registers, from its address: millivolts */
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

/* The RPMh address of a resource by its name in the command database the boot loader left; 0 = none. */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		printf("syna: no command database (magic %02x %02x %02x %02x)\n", db->magic[0], db->magic[1], db->magic[2],
		    db->magic[3]);
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
rpmh_wait(uint32_t reg, uint32_t mask, uint32_t value, const char *what)
{
	for (int i = 0; i < 2000; i++) {
		if ((rd(TCS(reg)) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	printf("syna: RPMh: timeout waiting for %s (register %#x = %#x)\n", what, reg, rd(TCS(reg)));
	return false;
}

/* Send up to three register writes to RPMh and wait until they are done. */
static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	bool done;

	/* the slot set must be idle, and switched off the way the hardware wants it: trigger, then enable */
	for (int i = 0; rd(TCS(TCS_STATUS)) == 0; i++) {
		if (i > 2000) {
			printf("syna: RPMh: the command slots stay busy\n");
			return false;
		}
		sleep_us(100);
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(TCS_CMD_MSGID(i)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(TCS_CMD_ADDR(i)), address[i]);
		wr(TCS(TCS_CMD_DATA(i)), data[i]);
	}
	wr(TCS(TCS_WAIT_FOR_COMPLETION), (1u << count) - 1);
	wr(TCS(TCS_CMD_ENABLE), (1u << count) - 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);        /* the status bit follows the enable; the interrupt itself stays masked */
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	done = rpmh_wait(TCS_IRQ_STATUS, 1, 1, "the request");
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	return done;
}

/* The chip's I/O supply: 1.8 V from the PMH0110's LDO4 on the I die ("L4I_E0", the DTB's
 * rpmh-regulator-l4i-e0 resource; regulator-pmh0110-i-l4). XBL has almost certainly left
 * it on, so a failure here is reported but not fatal. (redwood: 1.8 V "ldoc2") */
static bool
io_supply_on(void)
{
	uint32_t base = rpmh_address("L4I_E0");
	uint32_t address[3] = { base + VRM_VOLTAGE, base + VRM_ENABLE, base + VRM_MODE };
	uint32_t data[3] = { 1800, 1, VRM_MODE_LDO_HPM };

	/* addresses of regulators ("VRM") have 4 in bits 19:16; anything else is not what we are looking for */
	if (base == 0 || (base >> 16 & 0xf) != 4) {
		printf("syna: RPMh: no regulator \"L4I_E0\" (address %#x); assuming XBL left the I/O supply on\n", base);
		return false;
	}
	printf("syna: RPMh: L4I_E0 is at %#x; slot status %#x, control %#x, irq enable %#x status %#x\n", base,
	    rd(TCS(TCS_STATUS)), rd(TCS(TCS_CONTROL)), rd(TCS(TCS_IRQ_ENABLE)), rd(TCS(TCS_IRQ_STATUS)));
	if (!rpmh_write(address, data, 3)) {
		printf("syna: RPMh: could not write L4I_E0; continuing (the LDO may already be on)\n");
		return false;
	}
	printf("syna: I/O supply on (1.8 V)\n");
	return true;
}

/* ---- clocks (GCC) --------------------------------------------------------------------------------
 * redwood_goodix.c L263-350, with the qupv3_4 values */

#define GCC_PHYS                0x00100000UL    /* DTB /soc/clock-controller@100000 */
#define GCC_SIZE                0x60000UL
/*
 * The qupv3_4 branch enables share one CBCR (the plan §1, gcc-kaanapali.c structs
 * gcc_qupv3_wrap4_*): enable_reg 0x52018, enable_mask BIT(n). Every halt register is
 * BRANCH_HALT_VOTED: bit 31 (CBCR_CLK_OFF) reads 0 while the clock runs.
 * (redwood: GCC_VOTE 0x52008 with one bit per branch, SE5 RCG at 0x18600.)
 */
#define GCC_CBCR                (gcc + 0x52018)
#define VOTE_QUP4_M_AHB         (1u << 22)      /* halt 0x23548 */
#define VOTE_QUP4_S_AHB         (1u << 23)      /* halt 0x2354c */
#define VOTE_QUP4_CORE          (1u << 24)      /* halt 0x23550 */
#define VOTE_QUP4_S0            (1u << 26)      /* halt 0xa9004 */
#define GCC_QUP4_M_AHB_HALT     (gcc + 0x23548)
#define GCC_QUP4_S_AHB_HALT     (gcc + 0x2354c)
#define GCC_QUP4_CORE_HALT      (gcc + 0x23550)
#define GCC_QUP4_S0_HALT        (gcc + 0xa9004)
#define GCC_QUP4_S0_CMD         (gcc + 0xa900c)         /* gcc_qupv3_wrap4_s0_clk_src: cmd_rcgr = 0xa900c; bit 0: take the new configuration */
#define GCC_QUP4_S0_CFG         (gcc + 0xa9010)         /* [4:0] 2*divider-1, [10:8] source, [13:12] mode */
#define GCC_QUP4_S0_DFSR        (gcc + 0xa9020)         /* cmd_rcgr + 0x14; bit 0: the engine chooses among preset rates itself */
#define GCC_QUP4_S0_PERF_CFG(l) (gcc + 0xa9028 + 4 * (l))       /* cmd_rcgr + 0x1c + 4*l */
#define GCC_QUP4_S0_PERF_M(l)   (gcc + 0xa9068 + 4 * (l))       /* cmd_rcgr + 0x5c + 4*l */
#define GCC_QUP4_S0_PERF_N(l)   (gcc + 0xa90a8 + 4 * (l))       /* cmd_rcgr + 0x9c + 4*l */
#define CLOCK_OFF               (1u << 31)
#define XO_HZ                   19200000

static uint32_t se_source_hz, se_clock_select;

static uint32_t
rcg_rate(uint32_t cfg, uint32_t m, uint32_t not_n_minus_m)
{
	/* popsicle parent map 0: { TCXO 0, GPLL0_OUT_MAIN 1 = 600 MHz, GPLL0_OUT_EVEN 6 = 300 MHz }
	 * (gcc-kaanapali.c gcc_parent_map_0; GPLL0 is a TAYCAN EKO-T fixed PLL). redwood: 19.2/600/300. */
	static const uint32_t parent[8] = { XO_HZ, 600000000, 0, 0, 0, 0, 300000000, 0 };
	uint64_t hz = parent[cfg >> 8 & 7];
	uint32_t divider = cfg & 0x1f, mode = cfg >> 12 & 3;

	if (divider) {
		hz = hz * 2 / (divider + 1);
	}
	if (mode) {
		uint32_t n = (~not_n_minus_m & 0xffff) + (m & 0xffff);

		if (n) {
			hz = hz * (m & 0xffff) / n;
		}
	}
	return (uint32_t)hz;
}

static bool
clocks_on(void)
{
	/* the plan §6.11: all four branches are voted at once [TBD §6.11: whether the
	 * shared-CBCR votes interact is not observable before boot] */
	wr(GCC_CBCR, rd(GCC_CBCR) | VOTE_QUP4_M_AHB | VOTE_QUP4_S_AHB | VOTE_QUP4_CORE);
	if (rd(GCC_QUP4_S0_DFSR) & 1) {
		/* preset rates: the lowest that is at least 4 MHz (wrap4_s0 is in gcc_dfs_clocks) */
		for (uint32_t level = 0; level < 8; level++) {
			uint32_t hz = rcg_rate(rd(GCC_QUP4_S0_PERF_CFG(level)), rd(GCC_QUP4_S0_PERF_M(level)),
			    rd(GCC_QUP4_S0_PERF_N(level)));

			if (hz >= 4000000 && (se_source_hz == 0 || hz < se_source_hz)) {
				se_source_hz = hz;
				se_clock_select = level;
			}
		}
		if (se_source_hz == 0) {
			printf("syna: no usable preset clock rate\n");
			return false;
		}
	} else {
		/* the crystal, undivided: nothing else has to be running */
		wr(GCC_QUP4_S0_CFG, (rd(GCC_QUP4_S0_CFG) & ~0x371fu) | 1);
		wr(GCC_QUP4_S0_CMD, rd(GCC_QUP4_S0_CMD) | 1);
		for (int i = 0; i < 500 && (rd(GCC_QUP4_S0_CMD) & 1); i++) {
			sleep_us(10);
		}
		if (rd(GCC_QUP4_S0_CMD) & 1) {
			printf("syna: the serial engine's clock does not take its configuration (%#x)\n", rd(GCC_QUP4_S0_CMD));
			return false;
		}
		se_source_hz = XO_HZ;
		se_clock_select = 0;
	}
	wr(GCC_CBCR, rd(GCC_CBCR) | VOTE_QUP4_S0);
	for (int i = 0; i < 1000; i++) {
		if (!((rd(GCC_QUP4_M_AHB_HALT) | rd(GCC_QUP4_S_AHB_HALT) | rd(GCC_QUP4_CORE_HALT) | rd(GCC_QUP4_S0_HALT)) &
		    CLOCK_OFF)) {
			return true;
		}
		sleep_us(10);
	}
	printf("syna: clocks stay off: %#x %#x %#x %#x\n", rd(GCC_QUP4_M_AHB_HALT), rd(GCC_QUP4_S_AHB_HALT),
	    rd(GCC_QUP4_CORE_HALT), rd(GCC_QUP4_S0_HALT));
	return false;
}

/* ---- SPI through the serial engine (GENI) ----------------------------------------------------------
 * redwood_goodix.c L352-556; the GENI register layout is the same IP block, only SE_PHYS changes */

#define SE_PHYS                 0x01a80000UL    /* DTB spi@1a80000 (redwood: 0x00a94000) */
#define SE_SIZE                 0x4000UL
#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)    /* bit 0: a command is running */
#define GENI_SER_M_CLK_CFG      (se + 0x048)    /* bit 0 enable, [15:4] divider */
#define GENI_IF_DISABLE_RO      (se + 0x064)    /* bit 0: no FIFO interface */
#define GENI_FW_REVISION_RO     (se + 0x068)    /* [15:8] protocol: 1 = SPI */
#define GENI_CLK_SEL            (se + 0x07c)
#define SPI_CPHA                (se + 0x224)
#define SPI_LOOPBACK            (se + 0x22c)
#define SPI_CPOL                (se + 0x230)
#define SPI_DEMUX_OUTPUT_INV    (se + 0x24c)
#define SPI_DEMUX_SEL           (se + 0x250)
#define GENI_BYTE_GRAN          (se + 0x254)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define SPI_TRANS_CFG           (se + 0x25c)
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_TX_PACKING_CFG1    (se + 0x264)
#define SPI_WORD_LEN            (se + 0x268)
#define SPI_TX_TRANS_LEN        (se + 0x26c)
#define SPI_RX_TRANS_LEN        (se + 0x270)
#define SPI_DELAY_COUNTERS      (se + 0x278)
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_RX_PACKING_CFG1    (se + 0x288)
#define GENI_M_CMD0             (se + 0x600)    /* [31:27] operation */
#define GENI_M_CMD_CTRL         (se + 0x604)    /* bit 2 cancel, bit 1 abort */
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_IRQ_CLEAR        (se + 0x648)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_TX_FIFO_STATUS     (se + 0x800)    /* [27:0] words */
#define GENI_RX_FIFO_STATUS     (se + 0x804)    /* bit 31 last, [30:28] bytes of the last word, [24:0] words */
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_DMA_TX_IRQ_CLR       (se + 0xc44)
#define SE_DMA_RX_IRQ_CLR       (se + 0xd44)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)    /* [21:16] FIFO depth in words */
#define SE_DMA_GENERAL_CFG      (se + 0xe30)

#define M_CMD_DONE              (1u << 0)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define M_ERRORS                ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))
#define SPI_FULL_DUPLEX         (3u << 27)

static uint32_t fifo_depth;

/* 0 = ready; otherwise why the engine cannot be used */
static const char *
geni_spi_init(uint32_t hz)
{
	uint32_t divider;

	if ((rd(GENI_FW_REVISION_RO) >> 8 & 0xff) != 1) {
		return "the serial engine has no SPI firmware";
	}
	if (rd(GENI_IF_DISABLE_RO) & 1) {
		return "the serial engine's FIFO interface is disabled";
	}
	fifo_depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;
	if (fifo_depth < 4) {
		return "the serial engine reports no FIFO";
	}
	wr(SE_GSI_EVENT_EN, 0);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
	wr(SE_DMA_GENERAL_CFG, rd(SE_DMA_GENERAL_CFG) | 0xf);
	wr(GENI_OUTPUT_CTRL, 0x7f);
	wr(GENI_FORCE_DEFAULT, 1);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1u);
	wr(GENI_RX_WATERMARK, fifo_depth - 3);
	wr(GENI_RX_RFR_WATERMARK, fifo_depth - 2);
	wr(GENI_TX_WATERMARK, 0);
	/* the status bits we poll follow these enables; the interrupt itself stays masked at the GIC */
	wr(SE_IRQ_EN, rd(SE_IRQ_EN) | 0xf);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | 0x7fc0007f);

	/* mode 0, chip select 0, 8 bits, most significant first, four bytes in a FIFO word */
	wr(SPI_LOOPBACK, 0);
	wr(SPI_DEMUX_SEL, 0);
	wr(SPI_CPHA, 0);
	wr(SPI_CPOL, 0);
	wr(SPI_DEMUX_OUTPUT_INV, 0);
	wr(SPI_TRANS_CFG, rd(SPI_TRANS_CFG) & ~2u);
	wr(SPI_DELAY_COUNTERS, 0);
	wr(GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_TX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_RX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_BYTE_GRAN, 0);
	wr(SPI_WORD_LEN, 8 - 4);

	divider = (se_source_hz + hz - 1) / hz;
	wr(GENI_CLK_SEL, se_clock_select & 7);
	wr(GENI_SER_M_CLK_CFG, (divider ? divider : 1) << 4 | 1);
	return NULL;
}

static void
geni_drain(uint8_t *rx, uint32_t length, uint32_t *received)
{
	uint32_t status = rd(GENI_RX_FIFO_STATUS), words = status & 0x01ffffff, bytes = words * 4;

	if (status & 0x80000000u) {
		uint32_t valid = status >> 28 & 7;

		if (valid && valid < 4) {
			bytes -= 4 - valid;
		}
	}
	for (uint32_t i = 0; i < words; i++) {
		uint32_t word = rd(GENI_RX_FIFO);

		for (int j = 0; j < 4 && bytes; j++, bytes--) {
			if (*received < length) {
				if (rx) {
					rx[*received] = (uint8_t)(word >> (8 * j));
				}
				(*received)++;
			}
		}
	}
}

static void
geni_recover(void)
{
	wr(GENI_M_CMD_CTRL, 1u << 2);
	for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL); i++) {
		sleep_us(100);
	}
	if (!(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL)) {
		wr(GENI_M_CMD_CTRL, 1u << 1);
		for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_ABORT); i++) {
			sleep_us(100);
		}
	}
	for (int i = 0; i < 64 && (rd(GENI_RX_FIFO_STATUS) & 0x01ffffff); i++) {
		(void)rd(GENI_RX_FIFO);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
}

/* One transfer with the chip selected throughout: `length` bytes out (zeros without tx) and as many in. */
static bool
geni_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	static unsigned complaints;
	uint32_t sent = 0, received = 0, status = 0;
	uint64_t deadline;

	for (int i = 0; rd(GENI_STATUS) & 1; i++) {
		if (i > 2000) {
			geni_recover();
			break;
		}
		sleep_us(100);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(SPI_TX_TRANS_LEN, length);
	wr(SPI_RX_TRANS_LEN, length);
	wr(GENI_M_CMD0, SPI_FULL_DUPLEX);
	clock_interval_to_deadline(200, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		while (sent < length && (rd(GENI_TX_FIFO_STATUS) & 0x0fffffff) + 1 < fifo_depth) {
			uint32_t word = 0;

			for (int j = 0; j < 4 && sent < length; j++, sent++) {
				word |= (uint32_t)(tx ? tx[sent] : 0) << (8 * j);
			}
			wr(GENI_TX_FIFO, word);
		}
		status = rd(GENI_M_IRQ_STATUS);
		geni_drain(rx, length, &received);
		if (status & M_ERRORS) {
			break;
		}
		if (status & M_CMD_DONE) {
			geni_drain(rx, length, &received);
			wr(GENI_M_IRQ_CLEAR, status);
			if (received == length) {
				return true;
			}
			break;
		}
	}
	if (complaints++ < 8) {
		printf("syna: SPI transfer of %u bytes failed: status %#x, sent %u, received %u\n", length, status, sent,
		    received);
	}
	geni_recover();
	return false;
}

/* ---- SPI by hand, on the same pins -----------------------------------------------------------------
 * redwood_goodix.c L558-592 */

static unsigned bitbang_in = PIN_MISO, bitbang_out = PIN_MOSI;

static void
bitbang_init(void)
{
	gpio_output(PIN_CS, true, PULL_NONE, 6);
	gpio_output(PIN_CLK, false, PULL_NONE, 6);
	gpio_output(bitbang_out, false, PULL_NONE, 6);
	gpio_configure(bitbang_in, 0, PULL_NONE, 2, false);
}

static bool
bitbang_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	gpio_set(PIN_CS, false);
	for (uint32_t i = 0; i < length; i++) {
		uint8_t out = tx ? tx[i] : 0, in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			gpio_set(bitbang_out, out >> bit & 1);
			gpio_set(PIN_CLK, true);
			in = (uint8_t)(in << 1 | gpio_get(bitbang_in));
			gpio_set(PIN_CLK, false);
		}
		if (rx) {
			rx[i] = in;
		}
	}
	gpio_set(PIN_CS, true);
	return true;
}

static bool (*spi_transfer)(const uint8_t *tx, uint8_t *rx, uint32_t length);

/* ---- the Synaptics TCM chip -------------------------------------------------------------------------
 * The protocol is the LineageOS sm8350 Synaptics TCM driver; every block carries its
 * source lines. All the reflash / zeroflash / recovery / testing / host-download paths
 * are left out (the plan §3.6/§3.7). */

/* core.h:66-68: a message from the chip is [0]=marker 0xa5, [1]=code, [2..3]=payload
 * length little-endian, then the payload, then one 0x5a padding byte. */
#define TCM_MESSAGE_HEADER_SIZE 4
#define TCM_MESSAGE_MARKER      0xa5
#define TCM_MESSAGE_PADDING     0x5a

/* core.h:245-256 status_code */
#define TCM_STATUS_IDLE           0x00
#define TCM_STATUS_OK             0x01
#define TCM_STATUS_BUSY           0x02
#define TCM_STATUS_CONTINUED_READ 0x03
#define TCM_STATUS_ERROR          0x0f
#define TCM_STATUS_INVALID        0xff

/* core.h:162-168 boot_mode */
#define TCM_MODE_APPLICATION      0x01
#define TCM_MODE_BOOTLOADER       0x0b
#define TCM_MODE_TDDI_BOOTLOADER  0x0c

/* core.h:170-184 boot_status / app_status */
#define TCM_BOOT_STATUS_OK        0x00
#define TCM_APP_STATUS_OK         0x00
#define TCM_APP_STATUS_BOOTING    0x01
#define TCM_APP_STATUS_UPDATING   0x02
#define TCM_APP_STATUS_BAD_APP_CONFIG 0xff

/* core.h:209-243 command (the subset used here) */
#define TCM_CMD_IDENTIFY          0x02
#define TCM_CMD_RESET             0x04
#define TCM_CMD_ENABLE_REPORT     0x05
#define TCM_CMD_GET_BOOT_INFO     0x10
#define TCM_CMD_RUN_APPLICATION_FIRMWARE 0x14
#define TCM_CMD_GET_APPLICATION_INFO 0x20
#define TCM_CMD_SET_TOUCH_REPORT_CONFIG 0x26

/* core.h:258-266 report_type (the subset used here) */
#define TCM_REPORT_IDENTIFY       0x10
#define TCM_REPORT_TOUCH          0x11

/* core.c:47-91 the timings the Linux core uses */
#define TCM_MIN_READ_LENGTH       9
#define TCM_RESPONSE_TIMEOUT_MS   3000
#define TCM_APP_STATUS_POLL_MS    100
#define TCM_APP_STATUS_POLL_TIMEOUT_MS 1000
#define TCM_MODE_SWITCH_DELAY_MS  100
#define TCM_READ_RETRY_US_MIN     5000
#define TCM_READ_RETRY_US_MAX     10000

/* touch.c:43-48 touch_status */
#define TOUCH_LIFT                0
#define TOUCH_FINGER              1
#define TOUCH_GLOVED_FINGER       2
#define TOUCH_NOP                 (-1)

/* touch.c:50-80 touch_report_code (the subset the report config uses) */
#define TOUCH_END                 0
#define TOUCH_FOREACH_ACTIVE_OBJECT 1
#define TOUCH_FOREACH_END         3
#define TOUCH_OBJECT_N_INDEX      6
#define TOUCH_OBJECT_N_CLASSIFICATION 7
#define TOUCH_OBJECT_N_X_POSITION 8
#define TOUCH_OBJECT_N_Y_POSITION 9

/* the DTB timings of the chip's reset (popsicle_overlay synaptics_tcm@0)
 * [TBD §6.1: the plan defaulted to 20 ms / 100 ms; the DTB says 10 ms / 200 ms] */
#define POWER_DELAY_MS   50        /* synaptics,power-delay-ms = 0x32 */
#define RESET_ACTIVE_MS  10        /* synaptics,reset-active-ms = 0xa */
#define RESET_DELAY_MS   200       /* synaptics,reset-delay-ms = 0xc8 */
/* the bus: first boot 5 MHz, conservative; the chip allows up to 15 MHz [TBD §6.3] */
#define SPI_HZ           5000000   /* DTB synaptics,ubl-max-freq = 0xe4e1c0 = 15 MHz */

/* the largest payload accepted: the application info is 46 bytes, touch reports with ten
 * 4-byte objects are 40; 512 leaves room for whatever else the chip sends */
#define TCM_PAYLOAD_MAX   512
#define TCM_MAX_TOUCHES   10
/* the biggest payload sent: the 11-byte report configuration (touch.c:875-891) */
#define TCM_CMD_PAYLOAD_MAX 11

/* What has been seen so far: said once a minute, so that the end of the log always tells. */
static struct {
	uint32_t polls, reports, touches, bad, last_x, last_y;
} seen;

/* core.h:324-330 struct syna_tcm_identification */
struct tcm_identification {
	uint8_t version;
	uint8_t mode;
	uint8_t part_number[16];
	uint8_t build_id[4];
	uint8_t max_write_size[2];
};

/* core.h:332-349 struct syna_tcm_boot_info */
struct tcm_boot_info {
	uint8_t version;
	uint8_t status;
	uint8_t asic_id[2];
	uint8_t write_block_size_words;
	uint8_t erase_page_size_words[2];
	uint8_t max_write_payload_size[2];
	uint8_t last_reset_reason;
	uint8_t pc_at_time_of_last_reset[2];
	uint8_t boot_config_start_block[2];
	uint8_t boot_config_size_blocks[2];
	uint8_t display_config_start_block[4];
	uint8_t display_config_length_blocks[2];
	uint8_t backup_display_config_start_block[4];
	uint8_t backup_display_config_length_blocks[2];
	uint8_t custom_otp_start_block[2];
	uint8_t custom_otp_length_blocks[2];
};

/* core.h:351-368 struct syna_tcm_app_info */
struct tcm_app_info {
	uint8_t version[2];
	uint8_t status[2];
	uint8_t static_config_size[2];
	uint8_t dynamic_config_size[2];
	uint8_t app_config_start_write_block[2];
	uint8_t app_config_size[2];
	uint8_t max_touch_report_config_size[2];
	uint8_t max_touch_report_payload_size[2];
	uint8_t customer_config_id[16];
	uint8_t max_x[2];
	uint8_t max_y[2];
	uint8_t max_objects[2];
	uint8_t num_of_buttons[2];
	uint8_t num_of_image_rows[2];
	uint8_t num_of_image_cols[2];
	uint8_t has_hybrid_data[2];
};

/* one message, its code and payload length */
static uint8_t tcm_msg[TCM_MESSAGE_HEADER_SIZE + TCM_PAYLOAD_MAX + 1];
static uint8_t tcm_spi_tx[TCM_MESSAGE_HEADER_SIZE + TCM_PAYLOAD_MAX + 1];     /* read dummies, all 0xff (spi.c:426-483) */
static uint32_t tcm_code, tcm_plen;

static struct tcm_identification id_info;
static struct tcm_boot_info boot_info;
static struct tcm_app_info app_info;
static uint16_t app_status;

/* the panel, from the DTB until the application info says otherwise */
static uint32_t max_x = 1200, max_y = 2608;    /* DTB synaptics,max-x/max-y = 0x4b0/0xa30 */
static uint32_t max_objects = TCM_MAX_TOUCHES;
static uint32_t flip;                          /* boot-arg touch_flip: 1 = mirror left-right, 2 = mirror top-bottom */
/* [TBD §6.9: the fod window (DTB synaptics,fod-* = 597,2406 246x246) does not change the
 * touch report; coordinates pass through as reported] */

static uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

/* spi.c:426-483 syna_tcm_spi_read: a full-duplex transfer of `length` bytes with 0xff
 * dummy bytes on MOSI. The write path sends the data plain, no dummies. */
static bool
tcm_read_raw(uint8_t *rx, uint32_t length)
{
	if (length > sizeof(tcm_spi_tx)) {
		return false;
	}
	return spi_transfer(tcm_spi_tx, rx, length);
}

/* core.c:1167-1331 syna_tcm_read_message, with core.c:815-939 syna_tcm_continued_read:
 * the first read takes TCM_MIN_READ_LENGTH = 9 bytes (core.c:47). Status reports
 * (codes 0x00..0x0f and 0xff) only carry a payload for STATUS_OK; IDLE/BUSY/
 * CONTINUED_READ end the message right there (core.c:1230-1243). A payload that does
 * not fit the first read comes in continued-read chunks prefixed [0xa5,
 * STATUS_CONTINUED_READ] (core.c:896-915); a trailing single padding byte is not read
 * but synthesized (core.c:867-871). Returns false when there is nothing (or nothing
 * valid) to read. */
static bool
tcm_read(void)
{
	uint32_t total, offset, remaining;
	bool retried = false;

again:
	if (!tcm_read_raw(tcm_msg, TCM_MIN_READ_LENGTH)) {
		if (!retried) {
			sleep_us((TCM_READ_RETRY_US_MIN + TCM_READ_RETRY_US_MAX) / 2);
			retried = true;
			goto again;
		}
		return false;
	}
	if (tcm_msg[0] != TCM_MESSAGE_MARKER) {
		if (!retried) {
			sleep_us((TCM_READ_RETRY_US_MIN + TCM_READ_RETRY_US_MAX) / 2);
			retried = true;
			goto again;
		}
		return false;
	}
	tcm_code = tcm_msg[1];
	tcm_plen = tcm_msg[2] | (uint32_t)tcm_msg[3] << 8;

	if (tcm_code <= TCM_STATUS_ERROR || tcm_code == TCM_STATUS_INVALID) {
		switch (tcm_code) {
		case TCM_STATUS_OK:
			break;                          /* a response: its payload follows (core.c:1233-1234) */
		case TCM_STATUS_CONTINUED_READ:     /* out of sync (core.c:1235-1237) */
		case TCM_STATUS_IDLE:
		case TCM_STATUS_BUSY:
			tcm_plen = 0;                   /* core.c:1240: nothing more to read */
			return true;
		default:
			break;                          /* core.c:1244-1258: an error status still has its payload
			                                   on the wire; read it so the stream stays in sync */
		}
	}
	if (tcm_plen > TCM_PAYLOAD_MAX) {
		return false;
	}
	total = TCM_MESSAGE_HEADER_SIZE + tcm_plen + 1;
	if (tcm_plen == 0) {
		tcm_msg[total - 1] = TCM_MESSAGE_PADDING;       /* core.c:1271-1274: synthesized */
	} else if (total > TCM_MIN_READ_LENGTH) {
		/* core.c:815-939 continued_read; one unlimited chunk (RD_CHUNK_SIZE 0, core.h:63) */
		offset = TCM_MIN_READ_LENGTH;
		remaining = total - TCM_MIN_READ_LENGTH;
		if (remaining == 1) {
			tcm_msg[offset] = TCM_MESSAGE_PADDING;      /* core.c:867-871: synthesized */
		} else {
			if (remaining + 2 > sizeof(tcm_spi_tx)) {
				return false;
			}
			if (!tcm_read_raw(tcm_spi_tx, remaining + 2)) {
				return false;
			}
			if (tcm_spi_tx[0] != TCM_MESSAGE_MARKER || tcm_spi_tx[1] != TCM_STATUS_CONTINUED_READ) {
				return false;                           /* core.c:899-915 */
			}
			memcpy(tcm_msg + offset, tcm_spi_tx + 2, remaining);
		}
	}
	if (tcm_msg[total - 1] != TCM_MESSAGE_PADDING) {
		return false;                                   /* core.c:1293-1300 */
	}
	return true;
}

/* core.c:1350-1553 syna_tcm_write_message, the single-chunk case (every payload here is
 * at most 11 bytes; wr_chunk_size is unlimited): the write frame is [0]=command,
 * [1..2]=length little-endian, then the payload -- no marker, no trailing padding
 * (core.c:1441-1445). The response is a message whose code is STATUS_OK with the
 * payload (core.c:664-665); for RESET and RUN_APPLICATION_FIRMWARE the completion
 * arrives as the REPORT_IDENTIFY the chip sends when it has restarted
 * (core.c:758-767, `identify_completes`). */
static bool
tcm_command(uint8_t command, const uint8_t *payload, uint32_t length, uint8_t *resp, uint32_t *resp_length,
    bool identify_completes)
{
	uint8_t out[TCM_MESSAGE_HEADER_SIZE + TCM_CMD_PAYLOAD_MAX];
	uint64_t deadline;

	if (length > sizeof(out) - 3) {
		return false;
	}
	out[0] = command;
	out[1] = (uint8_t)length;
	out[2] = (uint8_t)(length >> 8);
	if (length) {
		memcpy(out + 3, payload, length);
	}
	if (!spi_transfer(out, NULL, length + 3)) {
		return false;
	}

	clock_interval_to_deadline(TCM_RESPONSE_TIMEOUT_MS, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		if (!tcm_read()) {
			sleep_us(1000);
			continue;
		}
		if (tcm_code == TCM_STATUS_OK) {
			if (resp && resp_length) {
				uint32_t copy = tcm_plen < *resp_length ? tcm_plen : *resp_length;

				memcpy(resp, tcm_msg + TCM_MESSAGE_HEADER_SIZE, copy);
				*resp_length = tcm_plen;
			}
			return true;
		}
		if (identify_completes && tcm_code == TCM_REPORT_IDENTIFY) {
			/* core.c:758-767: the chip reports its new mode instead of an OK; the payload
			 * is the identification (core.c:727-737) */
			if (tcm_plen >= sizeof(id_info)) {
				memcpy(&id_info, tcm_msg + TCM_MESSAGE_HEADER_SIZE, sizeof(id_info));
			}
			return true;
		}
		if (tcm_code == TCM_STATUS_ERROR || tcm_code == TCM_STATUS_INVALID) {
			return false;
		}
		/* IDLE, BUSY, CONTINUED_READ and every report: wait on */
	}
	return false;
}

/* core.c:2047-2105 syna_tcm_get_app_info: while the application reports BOOTING or
 * UPDATING, retry every 100 ms for up to 1000 ms (core.c:67-69). */
static bool
tcm_get_app_info(bool verbose)
{
	int timeout = TCM_APP_STATUS_POLL_TIMEOUT_MS;

	for (;;) {
		uint32_t length = sizeof(app_info);

		if (!tcm_command(TCM_CMD_GET_APPLICATION_INFO, NULL, 0, (uint8_t *)&app_info, &length, false)) {
			return false;
		}
		app_status = le16(app_info.status);
		if ((app_status == TCM_APP_STATUS_BOOTING || app_status == TCM_APP_STATUS_UPDATING) && timeout > 0) {
			sleep_ms(TCM_APP_STATUS_POLL_MS);
			timeout -= TCM_APP_STATUS_POLL_MS;
			continue;
		}
		break;
	}
	if (verbose) {
		printf("syna: application firmware %u.%u, status %#x, coordinates up to %u x %u, %u objects\n",
		    app_info.version[1], app_info.version[0], app_status, le16(app_info.max_x), le16(app_info.max_y),
		    le16(app_info.max_objects));
	}
	return true;
}

/* core.c:2107-2150 syna_tcm_get_boot_info */
static bool
tcm_get_boot_info(bool verbose)
{
	uint32_t length = sizeof(boot_info);

	if (!tcm_command(TCM_CMD_GET_BOOT_INFO, NULL, 0, (uint8_t *)&boot_info, &length, false)) {
		return false;
	}
	if (verbose) {
		printf("syna: bootloader firmware %u, status %#x, asic %u\n", boot_info.version, boot_info.status,
		    le16(boot_info.asic_id));
	}
	return true;
}

/* core.c:2152-2233 syna_tcm_identify: CMD_IDENTIFY and its payload is the
 * identification (core.h:324-330); then, per mode, the application or the boot info. */
static bool
tcm_identify(bool verbose)
{
	uint32_t length = sizeof(id_info);

	if (!tcm_command(TCM_CMD_IDENTIFY, NULL, 0, (uint8_t *)&id_info, &length, false)) {
		return false;
	}
	if (verbose) {
		printf("syna: \"%.16s\" (build %02x %02x %02x %02x), firmware mode %#x\n", id_info.part_number,
		    id_info.build_id[0], id_info.build_id[1], id_info.build_id[2], id_info.build_id[3], id_info.mode);
	}
	switch (id_info.mode) {
	case TCM_MODE_APPLICATION:
		return tcm_get_app_info(verbose);
	case TCM_MODE_BOOTLOADER:
	case TCM_MODE_TDDI_BOOTLOADER:
		return tcm_get_boot_info(verbose);
	default:
		return true;                            /* core.c:2221-2222: unknown mode, nothing more to read */
	}
}

/* touch.c:875-891 touch_set_report_config: the report we want -- for each active object
 * its index (4 bits), classification (4 bits), x (12 bits) and y (12 bits), i.e. four
 * bytes per object, followed by FOREACH_END and END. [TBD §6.7: the 12-bit widths] */
static bool
tcm_set_report_config(void)
{
	static const uint8_t config[] = {
		TOUCH_FOREACH_ACTIVE_OBJECT,
		TOUCH_OBJECT_N_INDEX, 4,
		TOUCH_OBJECT_N_CLASSIFICATION, 4,
		TOUCH_OBJECT_N_X_POSITION, 12,
		TOUCH_OBJECT_N_Y_POSITION, 12,
		TOUCH_FOREACH_END,
		TOUCH_END,
	};

	return tcm_command(TCM_CMD_SET_TOUCH_REPORT_CONFIG, config, sizeof(config), NULL, NULL, false);
}

/* testing.c:417-429 testing_enable_report: the one-byte payload names the report. */
static bool
tcm_enable_report(void)
{
	const uint8_t report_type = TCM_REPORT_TOUCH;

	return tcm_command(TCM_CMD_ENABLE_REPORT, &report_type, 1, NULL, NULL, false);
}

/* touch.c:177-229 touch_get_report_data: bits are packed least significant first. */
static uint32_t
touch_bits(const uint8_t *report, uint32_t report_bytes, uint32_t *offset, uint32_t bits)
{
	uint32_t data = 0, remaining = bits;
	uint32_t bit_offset = *offset % 8, byte_offset = *offset / 8;

	if (bits == 0 || bits > 32 || *offset + bits > report_bytes * 8) {
		*offset += bits;                        /* touch.c:197-200: past the end reads as 0 */
		return 0;
	}
	while (remaining) {
		uint8_t byte_data = report[byte_offset] >> bit_offset;
		uint32_t available = 8 - bit_offset, take = available < remaining ? available : remaining;

		byte_data &= 0xff >> (8 - take);
		data |= (uint32_t)byte_data << (bits - remaining);
		bit_offset = 0;
		byte_offset++;
		remaining -= take;
	}
	*offset += bits;
	return data;
}

/* one touch object's worth of a report */
struct touch_object {
	uint32_t status, x, y;
};
static struct touch_object object[TCM_MAX_TOUCHES];
static int prev_status[TCM_MAX_TOUCHES];        /* touch.c: prev_status[]; LIFT until proven otherwise */
static struct {
	bool down;
	uint32_t x, y;
} contact[TCM_MAX_TOUCHES];

/* touch.c:616-669 touch_report: posts UP/DOWN/MOVE through redwood_touch_post; see tcm_report_diff */
static void tcm_report_diff(void);

/* touch.c:238-410 touch_parse_report, over the fixed configuration of
 * tcm_set_report_config(): FOREACH_ACTIVE_OBJECT walks the payload while bits remain
 * (touch.c:292-301), each pass reading index / classification / x / y
 * (touch.c:322-364). */
static void
tcm_report(const uint8_t *payload, uint32_t plen)
{
	/* the same bytes as tcm_set_report_config() sends (touch.c:875-891) */
	static const uint8_t config[] = { 1, 6, 4, 7, 4, 8, 12, 9, 12, 3, 0 };
	uint32_t idx = 0, offset = 0, obj = 0, next = 0, objects = 0, active_objects = 0;
	bool active_only = false, num_of_active_objects = false;

	memset(object, 0, sizeof(object));
	while (idx < sizeof(config)) {
		uint8_t code = config[idx++];

		switch (code) {
		case TOUCH_END:
			goto parsed;                        /* touch.c:280-281 */
		case TOUCH_FOREACH_ACTIVE_OBJECT:
			obj = 0;
			next = idx;
			active_only = true;                 /* touch.c:282-286 */
			break;
		case TOUCH_FOREACH_END:
			/* touch.c:292-307: another object while bits remain in the payload */
			if (active_only) {
				if (num_of_active_objects) {
					objects++;
					if (objects < active_objects) {
						idx = next;
					}
				} else if (offset < plen * 8) {
					idx = next;
				}
			}
			break;
		case TOUCH_OBJECT_N_INDEX: {
			obj = touch_bits(payload, plen, &offset, config[idx++]);      /* touch.c:322-331 */
			break;
		}
		case TOUCH_OBJECT_N_CLASSIFICATION: {
			uint32_t status = touch_bits(payload, plen, &offset, config[idx++]);

			if (obj < TCM_MAX_TOUCHES) {
				object[obj].status = status;    /* touch.c:332-342 */
			}
			break;
		}
		case TOUCH_OBJECT_N_X_POSITION: {
			uint32_t x = touch_bits(payload, plen, &offset, config[idx++]);

			if (obj < TCM_MAX_TOUCHES) {
				object[obj].x = x;              /* touch.c:343-353 */
			}
			break;
		}
		case TOUCH_OBJECT_N_Y_POSITION: {
			uint32_t y = touch_bits(payload, plen, &offset, config[idx++]);

			if (obj < TCM_MAX_TOUCHES) {
				object[obj].y = y;              /* touch.c:354-364 */
			}
			break;
		}
		default:
			break;
		}
	}
parsed:
	tcm_report_diff();
}

/* touch.c:616-669 touch_report, without the Linux input layer: a lifted object posts
 * REDWOOD_TOUCH_UP, a new one REDWOOD_TOUCH_DOWN, a moved one REDWOOD_TOUCH_MOVE
 * (redwood_input.h:20-22), coordinates scaled to 0..65535. */
static void
tcm_report_diff(void)
{
	for (uint32_t idx = 0; idx < max_objects && idx < TCM_MAX_TOUCHES; idx++) {
		uint32_t status;

		/* touch.c:617-621: LIFT after LIFT is NOP, otherwise the object's own status */
		status = prev_status[idx] == TOUCH_LIFT && object[idx].status == TOUCH_LIFT
		    ? (uint32_t)TOUCH_NOP : object[idx].status;
		switch (status) {
		case TOUCH_LIFT:
			if (contact[idx].down) {
				contact[idx].down = false;
				redwood_touch_post(REDWOOD_TOUCH_UP, idx, contact[idx].x, contact[idx].y);
			}
			break;
		case TOUCH_FINGER:
		case TOUCH_GLOVED_FINGER: {
			uint32_t x = object[idx].x, y = object[idx].y;

			/* no swap-axes / x-flip / y-flip: the DTB sets none of them [TBD §6.8];
			 * boot-arg touch_flip overrides */
			x = x >= max_x ? 65535 : x * 65535 / max_x;
			y = y >= max_y ? 65535 : y * 65535 / max_y;
			if (flip & 1) {
				x = 65535 - x;
			}
			if (flip & 2) {
				y = 65535 - y;
			}
			seen.touches++;
			seen.last_x = x;
			seen.last_y = y;
			if (!contact[idx].down) {
				redwood_touch_post(REDWOOD_TOUCH_DOWN, idx, x, y);
			} else if (contact[idx].x != x || contact[idx].y != y) {
				redwood_touch_post(REDWOOD_TOUCH_MOVE, idx, x, y);
			}
			contact[idx].down = true;
			contact[idx].x = x;
			contact[idx].y = y;
			break;
		}
		default:
			break;
		}
		prev_status[idx] = (int)object[idx].status;     /* touch.c:669 */
	}
}

/* The plan's §3 first-boot flow, i.e. core.c:2633-2759 without the soft reset and
 * without GET_FEATURES: reset already released -> IDENTIFY -> in APPLICATION,
 * GET_APPLICATION_INFO (done by tcm_identify); in BOOTLOADER, RUN_APPLICATION_FIRMWARE
 * + 100 ms (core.c:83) + IDENTIFY again -> application firmware with APP_STATUS_OK ->
 * SET_TOUCH_REPORT_CONFIG -> ENABLE_REPORT -> take max_x/max_y/max_objects from the
 * application info (the DTB values stay as the fallback). */
static bool
tcm_probe(bool verbose)
{
	if (!tcm_identify(verbose)) {
		return false;
	}
	if (id_info.mode != TCM_MODE_APPLICATION) {
		if (id_info.mode != TCM_MODE_BOOTLOADER && id_info.mode != TCM_MODE_TDDI_BOOTLOADER) {
			printf("syna: firmware mode %#x -- no application firmware to run [TBD §6.5]\n", id_info.mode);
			return false;
		}
		printf("syna: boot status %#x, running the application firmware\n", boot_info.status);   /* core.c:2731-2734 */
		if (!tcm_command(TCM_CMD_RUN_APPLICATION_FIRMWARE, NULL, 0, NULL, NULL, true)) {
			printf("syna: the chip would not run its application firmware\n");
			return false;
		}
		sleep_ms(TCM_MODE_SWITCH_DELAY_MS);     /* core.c:83 */
		if (!tcm_identify(verbose)) {
			return false;
		}
	}
	if (id_info.mode != TCM_MODE_APPLICATION) {
		printf("syna: still mode %#x after RUN_APPLICATION_FIRMWARE, boot status %#x\n", id_info.mode,
		    boot_info.status);
		return false;
	}
	if (app_status != TCM_APP_STATUS_OK) {
		/* BAD_APP_CONFIG (core.h:183) would mean reflashing, which this port does not
		 * do (the plan §3.6) [TBD §6.6] */
		printf("syna: application status %#x\n", app_status);
		return false;
	}
	if (!tcm_set_report_config()) {
		printf("syna: the chip would not take the touch report configuration\n");
		return false;
	}
	if (!tcm_enable_report()) {
		printf("syna: the chip would not enable the touch report\n");
		return false;
	}
	{
		uint32_t objects = le16(app_info.max_objects), x = le16(app_info.max_x), y = le16(app_info.max_y);

		if (objects == 0) {
			objects = TCM_MAX_TOUCHES;
		}
		if (objects > TCM_MAX_TOUCHES) {
			objects = TCM_MAX_TOUCHES;
		}
		max_objects = objects;
		if (x && y) {
			max_x = x;
			max_y = y;
		}
	}
	printf("syna: touch report configured (%u x %u, up to %u objects), 12-bit x and y [TBD §6.7]\n", max_x, max_y,
	    max_objects);
	return true;
}

/* Read one message if there is one, and turn a touch report into samples. */
static void
tcm_poll(void)
{
	seen.polls++;
	if (!tcm_read()) {
		return;
	}
	switch (tcm_code) {
	case TCM_REPORT_TOUCH:
		seen.reports++;
		tcm_report(tcm_msg + TCM_MESSAGE_HEADER_SIZE, tcm_plen);
		break;
	case TCM_REPORT_IDENTIFY:
		/* the chip has reset on its own: note its mode (core.c:727-737) */
		if (tcm_plen >= sizeof(id_info)) {
			memcpy(&id_info, tcm_msg + TCM_MESSAGE_HEADER_SIZE, sizeof(id_info));
		}
		break;
	default:
		if (tcm_code != TCM_STATUS_IDLE && tcm_code != TCM_STATUS_BUSY &&
		    tcm_code != TCM_STATUS_CONTINUED_READ) {
			seen.bad++;
		}
		break;
	}
}

/* ---- all of it together ----------------------------------------------------------------------------
 * redwood_goodix.c L935-1018, with the plan's power order */

static void
show_state(const char *when)
{
	printf("syna: %s: cbcr %#x, halts %#x %#x %#x %#x, rate cfg %#x dfs %#x; pins cs/clk/mosi/miso cfg %#x %#x %#x %#x,"
	    " reset %#x/%u, int %#x/%u, avdd %#x/%u\n", when, rd(GCC_CBCR), rd(GCC_QUP4_M_AHB_HALT),
	    rd(GCC_QUP4_S_AHB_HALT), rd(GCC_QUP4_CORE_HALT), rd(GCC_QUP4_S0_HALT), rd(GCC_QUP4_S0_CFG),
	    rd(GCC_QUP4_S0_DFSR), rd(GPIO_CFG(PIN_CS)), rd(GPIO_CFG(PIN_CLK)), rd(GPIO_CFG(PIN_MOSI)),
	    rd(GPIO_CFG(PIN_MISO)), rd(GPIO_CFG(PIN_RESET)), rd(GPIO_IO(PIN_RESET)), rd(GPIO_CFG(PIN_INT)),
	    rd(GPIO_IO(PIN_INT)), rd(GPIO_CFG(PIN_AVDD)), rd(GPIO_IO(PIN_AVDD)));
}

/* Power the chip up from nothing, the plan's §4 order: hold reset (66) low, then iovdd
 * (RPMh L4I_E0), then avdd (GPIO 117), wait the DTB's power-delay, and the interrupt
 * line last, as an input. The clocks and the SPI engine come up in bring_up();
 * tcm_reset() then releases reset and waits the DTB's reset-delay. [TBD §6.2] */
static void
power_on(void)
{
	gpio_output(PIN_RESET, false, PULL_UP, 8);     /* hold in reset while the supplies ramp */
	io_supply_on();                                 /* iovdd: RPMh L4I_E0 1.8 V (idempotent; may already be on) */
	sleep_ms(3);
	gpio_output(PIN_AVDD, true, PULL_NONE, 2);     /* avdd: touch_avdd_vreg, GPIO 117, active high */
	sleep_ms(POWER_DELAY_MS - 3);
	gpio_configure(PIN_INT, 0, PULL_UP, 8, false); /* irq line as input, last */
}

/* core.c:2650-2660 the hardware reset pulse, with the DTB's timings: hold the line low
 * for RESET_ACTIVE_MS, release, and give the chip RESET_DELAY_MS to boot. */
static void
tcm_reset(void)
{
	gpio_set(PIN_RESET, false);
	sleep_ms(RESET_ACTIVE_MS);
	gpio_set(PIN_RESET, true);
	sleep_ms(RESET_DELAY_MS);
}

static void
use_engine_pins(void)
{
	gpio_configure(PIN_MISO, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	gpio_configure(PIN_MOSI, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	gpio_configure(PIN_CLK, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	gpio_configure(PIN_CS, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
}

static bool
bring_up(void)
{
	const char *problem = "its clocks cannot be turned on";

	show_state("as found");
	power_on();
	tcm_reset();            /* release reset and let the chip boot, the plan's order: before the clocks */

	if (clocks_on()) {
		printf("syna: serial engine: firmware %#x, interface %#x, parameters %#x, source clock %u Hz (select %u)\n",
		    rd(GENI_FW_REVISION_RO), rd(GENI_IF_DISABLE_RO), rd(SE_HW_PARAM_0), se_source_hz, se_clock_select);
		uint32_t spi_hz = SPI_HZ;

		/* boot-arg syna_spi_hz=N: the bus at N Hz (the chip allows up to 15 MHz) [TBD §6.3] */
		PE_parse_boot_argn("syna_spi_hz", &spi_hz, sizeof(spi_hz));
		if (spi_hz < 100000 || spi_hz > 15000000) {
			spi_hz = SPI_HZ;
		}
		printf("syna: the bus at %u Hz\n", spi_hz);
		problem = geni_spi_init(spi_hz);
	}
	if (problem == NULL) {
		use_engine_pins();
		spi_transfer = geni_transfer;
		memset(tcm_spi_tx, 0xff, sizeof(tcm_spi_tx));   /* the read dummies (spi.c:426-483) */
		show_state("powered, on the serial engine");
		if (tcm_probe(true)) {
			return true;
		}
		printf("syna: the chip did not answer through the serial engine; trying the pins by hand\n");
	} else {
		printf("syna: %s; driving the pins by hand\n", problem);
	}

	/* by hand; which of the two data pins is the input is not written down anywhere, so try both */
	spi_transfer = bitbang_transfer;
	for (int attempt = 0; attempt < 2; attempt++) {
		bitbang_in = attempt == 0 ? PIN_MISO : PIN_MOSI;
		bitbang_out = attempt == 0 ? PIN_MOSI : PIN_MISO;
		bitbang_init();
		tcm_reset();
		if (tcm_probe(true)) {
			printf("syna: the chip answers on pins driven by hand (data in on gpio%u)\n", bitbang_in);
			return true;
		}
	}
	show_state("no answer");
	return false;
}

/* ---- the polled thread ----------------------------------------------------------------------------
 * redwood_goodix.c L1425-1500 */

static uint64_t
micros_now(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

static void
syna_thread(__unused void *parameter, __unused wait_result_t result)
{
	unsigned idle = 0, idle_looks;
	uint32_t poll_us = 8000, latency = 0, reads = 0;
	uint64_t before = 0, reading = 0;

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	PE_parse_boot_argn("touch_lat", &latency, sizeof(latency));
	if (poll_us < 250 || poll_us > 100000) {
		poll_us = 8000;
	}
	idle_looks = 80000 / poll_us;   /* a report read every 80 ms even with the line high, in case it is missed */

	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	se = ml_io_map(SE_PHYS, SE_SIZE);
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);      /* memory, not registers: no alignment rules */

	if (!bring_up()) {
		printf("syna: no Synaptics TCM touch screen\n");
		thread_terminate(current_thread());
		return;
	}
	printf("syna: Synaptics TCM touch screen ready; the interrupt line (gpio %u) is looked at every %u us\n",
	    PIN_INT, poll_us);
	for (unsigned tick = 1;; tick++) {
		uint64_t looked = micros_now(), read_at;

		/* the interrupt line is low while a report waits; look anyway every 80 ms in case it is missed */
		if (!gpio_get(PIN_INT)) {
			idle = 0;
			tcm_poll();
			read_at = micros_now();
			reads++;
			reading += read_at - looked;
			if (latency) {
				redwood_tl("tl: c before %llu seen %llu read %llu\n", before, looked, read_at);
			}
		} else if (++idle >= idle_looks) {
			idle = 0;
			tcm_poll();
			reads++;
			reading += micros_now() - looked;
		}
		before = looked;
		if (tick % (60 * (1000000 / poll_us)) == 0) {
			printf("syna: so far %u reports in %u looks (%u bad), %u finger positions, the last at %u, %u; "
			    "interrupt line %s; a report takes %llu us to read; the line looked at every %u us\n",
			    seen.reports, seen.polls, seen.bad, seen.touches, seen.last_x, seen.last_y,
			    gpio_get(PIN_INT) ? "high" : "low", reads ? reading / reads : 0, poll_us);
		}
		sleep_us(poll_us);
	}
}

/* ---- the entry point ------------------------------------------------------------------------------
 * redwood_goodix.c L1502-1515; boot-arg syna=1, so that the primary panel and the
 * GT9615 second panel (touchscreen=1) can be enabled independently [TBD §6.12] */
void
popsicle_syna_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("syna", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	PE_parse_boot_argn("touch_flip", &flip, sizeof(flip));
	if (kernel_thread_start(syna_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
