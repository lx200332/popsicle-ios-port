/*
 * The secondary touch screen of the Xiaomi 17 Pro Max (popsicle): a Goodix GT9615
 * ("Berlin-D", the same chip family as redwood's GT9916) on the SPI bus of the SoC's serial
 * engine qupv3_3 (se@1988000), as a source of touches for /dev/redwood-touch
 * (sys/redwood_input.h). Only with boot-arg touchscreen=1.
 *
 * This file is a port of bsd/dev/redwood_goodix.c (GT9916, redwood, SM7325): everything the
 * two phones share is kept verbatim -- the GENI SPI in FIFO mode, polled; the bit-bang
 * fallback on the same pins; the Goodix Berlin-D register reads (firmware version, IC info,
 * the touch report) and the report layout. The five board-level differences are changed in
 * place; the FocalTech half of the reference is removed, since popsicle's second panel has
 * no second vendor and no chip-ID strap GPIOs.
 *
 * The five differences, every value annotated with its source:
 *  1. serial engine 0x1988000 (qupv3_3, SE2)  -- redwood: 0x00a94000 (SE13).
 *     DTB /soc/qcom,qupv3_3_geni_se@19c0000/spi@1988000: reg = <0x1988000 0x4000>.
 *  2. TLMM 0x0f100000 (TRUE base: pin table = 0x100000 + 0x1000*n) -- redwood: 0x0f100000.
 *     DTB /soc/pinctrl@f000000: reg = <0x0f000000 0x202000> (pre-offset base).
 *  3. GPIOs: MISO 12, MOSI 13, CLK 14, CS 15 (function qup3_se2_l0..l3), RESET 185,
 *     INT 201, AVDD 82 -- redwood: 52/53/54/55, 105, 81, 107.
 *     DTB pinctrl groups mi_ts_spi_miso/mosi/clk/cs_active_second, mi_ts_reset_active,
 *     mi_ts_int_active; /soc/touch_avdd_vreg_second: gpio = <0x30 0x52 0> = GPIO 82.
 *     The mux number for "qup3_se2" is 1: mainline pinctrl-sm8750.c PINGROUP(12..15) puts
 *     qup2_se3 (= the vendor name qupv3_se2) in the first non-GPIO slot. VERIFY ON HARDWARE;
 *     the bit-bang fallback needs no mux (function 0 = GPIO) and is the safe first boot path.
 *  4. supplies: iovdd is the RPMh VRM "L2F_E0" (regulator pmh0110_f_l2, 1.2 V; XBL has
 *     almost certainly turned it on -- the DTB sets init-mode), avdd is the fixed regulator
 *     touch_avdd_vreg_second behind GPIO 82 (its voltage is not in the DTB; Goodix analog
 *     supply, typically 3.3 V). redwood: RPMh "ldoc2" 1.8 V + GPIO 107.
 *  5. clocks: the qupv3 branch enables share one CBCR at 0x52018 and the halt registers are
 *     voted: M_AHB bit 8 / halt 0x233f0, S_AHB bit 9 / halt 0x233f4, core bit 10 / halt
 *     0x233f8, core_2x bit 11 / halt 0x2340c, SE2 bit 14 / halt 0xa827c; the SE2 RCG sits at
 *     0xa828c (CMD/CFG at +0/+4, DFSR +0x14, PERF_CFG/M/N +0x1c/+0x5c/+0x9c per level).
 *     All from the vendor gcc-kaanapali.c structs gcc_qupv3_wrap3_* (clk_branch / clk_rcg2).
 *     Parent map 0 = { TCXO 0, GPLL0_OUT_MAIN 1 (600 MHz, TAYCAN EKO-T fixed PLL),
 *     GPLL0_OUT_EVEN 6 (300 MHz) }; the first DFS preset is 7372800 = 300e6*384/15625 Hz.
 *     redwood: GCC_VOTE 0x52008 bits 18-21/27, SE5 RCG at 0x18600.
 *
 * Power-up order (the task's specified sequence): hold reset low, then avdd (GPIO 82), then
 * iovdd (RPMh; writing it is idempotent and a failure is only reported -- XBL likely left
 * the LDO on), then the interrupt line is configured as an input, and touch_reset() releases
 * reset when the chip is probed. The interrupt line is polled 125 times a second (boot-arg
 * touch_poll_us=N), no GIC interrupt is used even though the DTB wires GIC SPI 201.
 *
 * The chip is a Berlin-D whose firmware and config live in its own flash: like redwood, this
 * driver downloads nothing and goodix_probe()'s IC_INFO read supplies the coordinate range.
 * The DTB's firmware-name/config-name (goodix_firmware_popsicle_csot /
 * goodix_cfg_group_popsicle_csot) are Linux-filesystem concepts, unused here.
 *
 * Register layouts: Linux drivers/spi/spi-geni-qcom.c, drivers/soc/qcom/{qcom-geni-se,
 * rpmh-rsc, cmd-db}.c, drivers/clk/qcom/gcc-kaanapali.c, drivers/pinctrl/qcom/
 * pinctrl-sm8750.c, drivers/input/touchscreen/goodix_berlin_*, and the phone's own device
 * tree (kaanapali-xiaomi-popsicle.dtb).
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

/* ---- pins (TLMM) ------------------------------------------------------------------------------- */

#define TLMM_PHYS               0x0f100000UL    /* TRUE base: pinctrl-canoe.ko pin table = 0x100000 + 0x1000*n (ios-pandora DT_FINDINGS.md:101); DT /soc/pinctrl@f000000 is pre-offset */
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define PULL_NONE               0
#define PULL_UP                 3

/* DTB pinctrl@f000000: mi_ts_spi_*_active_second (gpio12..15, "qup3_se2_l0..l3"),
 * mi_ts_reset_active (gpio185, "gpio"), mi_ts_int_active (gpio201, "gpio"), and the
 * fixed regulator /soc/touch_avdd_vreg_second (gpio 0x52 = 82). (redwood: 52..55/105/81/107) */
#define PIN_MISO                12
#define PIN_MOSI                13
#define PIN_CLK                 14
#define PIN_CS                  15
#define PIN_INT                 201     /* low = the chip has a report */
#define PIN_RESET               185     /* low = in reset */
#define PIN_AVDD                82      /* high = analog supply on */
#define PIN_FUNCTION_SPI        1       /* "qup3_se2": slot 1 of PINGROUP(12..15) in pinctrl-sm8750.c -- VERIFY */

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

/* ---- the I/O supply (RPMh) ----------------------------------------------------------------------- */

#define CMD_DB_PHYS             0x81c60000UL    /* DTB aop_cmd_db_region@81c60000 (redwood: 0x80860000) */
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x18920000UL    /* DTB rsc@18900000, third reg = drv-2 (redwood: 0x18220000) */
#define RSC_SIZE                0x10000UL
#define TCS(reg)                (rsc + 0xd00 + (reg))           /* DTB drv@2: qcom,tcs-offset = <0xd00> */
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
		printf("gt9615: no command database (magic %02x %02x %02x %02x)\n", db->magic[0], db->magic[1], db->magic[2],
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
	printf("gt9615: RPMh: timeout waiting for %s (register %#x = %#x)\n", what, reg, rd(TCS(reg)));
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
			printf("gt9615: RPMh: the command slots stay busy\n");
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

/* The chip's I/O supply: 1.2 V from the PMH0110's LDO2 on the F die ("L2F_E0", the DTB's
 * rpmh-regulator-l2f-e0 resource; regulator-pmh0110-f-l2, 0x124f80 = 1200000). XBL has almost
 * certainly left it on, so a failure here is reported but not fatal. (redwood: 1.8 V "ldoc2") */
static bool
io_supply_on(void)
{
	uint32_t base = rpmh_address("L2F_E0");
	uint32_t address[3] = { base + VRM_VOLTAGE, base + VRM_ENABLE, base + VRM_MODE };
	uint32_t data[3] = { 1200, 1, VRM_MODE_LDO_HPM };

	/* addresses of regulators ("VRM") have 4 in bits 19:16; anything else is not what we are looking for */
	if (base == 0 || (base >> 16 & 0xf) != 4) {
		printf("gt9615: RPMh: no regulator \"L2F_E0\" (address %#x); assuming XBL left the I/O supply on\n", base);
		return false;
	}
	printf("gt9615: RPMh: L2F_E0 is at %#x; slot status %#x, control %#x, irq enable %#x status %#x\n", base,
	    rd(TCS(TCS_STATUS)), rd(TCS(TCS_CONTROL)), rd(TCS(TCS_IRQ_ENABLE)), rd(TCS(TCS_IRQ_STATUS)));
	if (!rpmh_write(address, data, 3)) {
		printf("gt9615: RPMh: could not write L2F_E0; continuing (the LDO may already be on)\n");
		return false;
	}
	printf("gt9615: I/O supply on (1.2 V)\n");
	return true;
}

/* ---- clocks (GCC) -------------------------------------------------------------------------------- */

#define GCC_PHYS                0x00100000UL    /* DTB /soc/clock-controller@100000 */
#define GCC_SIZE                0x60000UL
/*
 * The qupv3 branch enables share one CBCR: vendor gcc-kaanapali.c, structs
 * gcc_qupv3_wrap3_* / gcc_qupv3_wrap_3_* -- enable_reg 0x52018, enable_mask BIT(n).
 * Every halt register is BRANCH_HALT_VOTED: bit 31 (CBCR_CLK_OFF) reads 0 while the clock
 * runs. (redwood: GCC_VOTE 0x52008 with one bit per branch, SE5 RCG at 0x18600.)
 */
#define GCC_CBCR                (gcc + 0x52018)
#define VOTE_QUP3_CORE_2X       (1u << 11)      /* halt 0x2340c */
#define VOTE_QUP3_CORE          (1u << 10)      /* halt 0x233f8 */
#define VOTE_QUP3_M_AHB         (1u << 8)       /* halt 0x233f0 */
#define VOTE_QUP3_S_AHB         (1u << 9)       /* halt 0x233f4 */
#define VOTE_QUP3_S2            (1u << 14)      /* halt 0xa827c */
#define GCC_QUP3_M_AHB_HALT     (gcc + 0x233f0)
#define GCC_QUP3_S_AHB_HALT     (gcc + 0x233f4)
#define GCC_QUP3_CORE_HALT      (gcc + 0x233f8)
#define GCC_QUP3_S2_HALT        (gcc + 0xa827c)
#define GCC_QUP3_S2_CMD         (gcc + 0xa828c)         /* gcc_qupv3_wrap3_s2_clk_src: cmd_rcgr = 0xa828c; bit 0: take the new configuration */
#define GCC_QUP3_S2_CFG         (gcc + 0xa8290)         /* [4:0] 2*divider-1, [10:8] source, [13:12] mode */
#define GCC_QUP3_S2_DFSR        (gcc + 0xa82a0)         /* cmd_rcgr + 0x14; bit 0: the engine chooses among preset rates itself */
#define GCC_QUP3_S2_PERF_CFG(l) (gcc + 0xa82a8 + 4 * (l))       /* cmd_rcgr + 0x1c + 4*l */
#define GCC_QUP3_S2_PERF_M(l)   (gcc + 0xa82e8 + 4 * (l))       /* cmd_rcgr + 0x5c + 4*l */
#define GCC_QUP3_S2_PERF_N(l)   (gcc + 0xa8328 + 4 * (l))       /* cmd_rcgr + 0x9c + 4*l */
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
	wr(GCC_CBCR, rd(GCC_CBCR) | VOTE_QUP3_CORE_2X | VOTE_QUP3_CORE | VOTE_QUP3_M_AHB | VOTE_QUP3_S_AHB);
	if (rd(GCC_QUP3_S2_DFSR) & 1) {
		/* preset rates: the lowest that is at least 4 MHz (wrap3_s2 is in gcc_dfs_clocks) */
		for (uint32_t level = 0; level < 8; level++) {
			uint32_t hz = rcg_rate(rd(GCC_QUP3_S2_PERF_CFG(level)), rd(GCC_QUP3_S2_PERF_M(level)),
			    rd(GCC_QUP3_S2_PERF_N(level)));

			if (hz >= 4000000 && (se_source_hz == 0 || hz < se_source_hz)) {
				se_source_hz = hz;
				se_clock_select = level;
			}
		}
		if (se_source_hz == 0) {
			printf("gt9615: no usable preset clock rate\n");
			return false;
		}
	} else {
		/* the crystal, undivided: nothing else has to be running */
		wr(GCC_QUP3_S2_CFG, (rd(GCC_QUP3_S2_CFG) & ~0x371fu) | 1);
		wr(GCC_QUP3_S2_CMD, rd(GCC_QUP3_S2_CMD) | 1);
		for (int i = 0; i < 500 && (rd(GCC_QUP3_S2_CMD) & 1); i++) {
			sleep_us(10);
		}
		if (rd(GCC_QUP3_S2_CMD) & 1) {
			printf("gt9615: the serial engine's clock does not take its configuration (%#x)\n", rd(GCC_QUP3_S2_CMD));
			return false;
		}
		se_source_hz = XO_HZ;
		se_clock_select = 0;
	}
	wr(GCC_CBCR, rd(GCC_CBCR) | VOTE_QUP3_S2);
	for (int i = 0; i < 1000; i++) {
		if (!((rd(GCC_QUP3_M_AHB_HALT) | rd(GCC_QUP3_S_AHB_HALT) | rd(GCC_QUP3_S2_HALT)) & CLOCK_OFF)) {
			return true;
		}
		sleep_us(10);
	}
	printf("gt9615: clocks stay off: %#x %#x %#x\n", rd(GCC_QUP3_M_AHB_HALT), rd(GCC_QUP3_S_AHB_HALT),
	    rd(GCC_QUP3_S2_HALT));
	return false;
}

/* ---- SPI through the serial engine (GENI) ---------------------------------------------------------- */
/* The GENI register layout is the same IP block as redwood's; only SE_PHYS changes. */

#define SE_PHYS                 0x01988000UL    /* DTB spi@1988000 (redwood: 0x00a94000) */
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
		printf("gt9615: SPI transfer of %u bytes failed: status %#x, sent %u, received %u\n", length, status, sent,
		    received);
	}
	geni_recover();
	return false;
}

/* ---- SPI by hand, on the same pins ----------------------------------------------------------------- */

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

/* ---- the Goodix chip ------------------------------------------------------------------------------- */
/* GT9615 is a Berlin-D, the same family as redwood's GT9916: the register addresses, the SPI
 * framing, the IC_INFO structure and the report layout below are shared (goodix_berlin_*). */

#define GOODIX_FW_VERSION       0x10014         /* 28 bytes */
#define GOODIX_IC_INFO          0x10070         /* a 16-bit length, then that many bytes */
#define GOODIX_MAX_READ         1024
#define GOODIX_MAX_TOUCHES      10
#define GOODIX_STATUS_TOUCH     0x80
#define GOODIX_STATUS_REQUEST   0x40
#define GOODIX_REQUEST_RESET    3

static uint8_t spi_tx[8 + GOODIX_MAX_READ], spi_rx[8 + GOODIX_MAX_READ];
static uint32_t touch_address, command_address;
static const char *report_rate = "240";        /* what the status line says (touch_rate=480) */
static uint32_t max_x = 97600, max_y = 59600;   /* the DTB's panel-max-x/y (0x17d40/0xe8d0); the chip may say otherwise */

static bool
goodix_read(uint32_t address, uint8_t *data, uint32_t length)
{
	if (length > GOODIX_MAX_READ) {
		return false;
	}
	spi_tx[0] = 0xf1;
	spi_tx[1] = (uint8_t)(address >> 24);
	spi_tx[2] = (uint8_t)(address >> 16);
	spi_tx[3] = (uint8_t)(address >> 8);
	spi_tx[4] = (uint8_t)address;
	spi_tx[5] = spi_tx[6] = spi_tx[7] = 0xff;
	memset(spi_tx + 8, 0, length);
	if (!spi_transfer(spi_tx, spi_rx, 8 + length)) {
		return false;
	}
	memcpy(data, spi_rx + 8, length);
	return true;
}

static bool
goodix_write(uint32_t address, const uint8_t *data, uint32_t length)
{
	if (length > GOODIX_MAX_READ) {
		return false;
	}
	spi_tx[0] = 0xf0;
	spi_tx[1] = (uint8_t)(address >> 24);
	spi_tx[2] = (uint8_t)(address >> 16);
	spi_tx[3] = (uint8_t)(address >> 8);
	spi_tx[4] = (uint8_t)address;
	memcpy(spi_tx + 5, data, length);
	return spi_transfer(spi_tx, NULL, 5 + length);
}

/* The chip's blocks end in the 16-bit sum of their bytes; all zeros or all ones is no answer at all. */
static bool
goodix_checksum_ok(const uint8_t *data, uint32_t length)
{
	uint32_t sum = 0;
	bool blank = true;

	if (length < 2) {
		return false;
	}
	for (uint32_t i = 0; i < length - 2; i++) {
		sum += data[i];
		blank &= data[i] == 0 || data[i] == 0xff;
	}
	return !blank && (uint16_t)sum == (uint16_t)(data[length - 2] | data[length - 1] << 8);
}

static uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t
le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void
touch_reset(void)
{
	gpio_set(PIN_RESET, false);
	sleep_ms(2);
	gpio_set(PIN_RESET, true);
	sleep_ms(100);
}

/* Is the chip there and running its firmware? Finds where it keeps the touch report. */
static bool
goodix_probe(bool verbose)
{
	static uint8_t info[GOODIX_MAX_READ];
	uint8_t version[28];
	uint32_t length, offset;

	if (!goodix_read(GOODIX_FW_VERSION, version, sizeof(version))) {
		return false;
	}
	if (!goodix_checksum_ok(version, sizeof(version))) {
		if (verbose) {
			printf("gt9615: no valid firmware version: %02x %02x %02x %02x %02x %02x %02x %02x ... %02x %02x\n",
			    version[0], version[1], version[2], version[3], version[4], version[5], version[6], version[7],
			    version[26], version[27]);
		}
		return false;
	}
	printf("gt9615: chip \"%.6s\", firmware \"%.8s\" %02x.%02x.%02x.%02x, sensor %u\n", (const char *)version,
	    (const char *)version + 10, version[18], version[19], version[20], version[21], version[23]);

	if (!goodix_read(GOODIX_IC_INFO, info, 2)) {
		return false;
	}
	length = le16(info);
	if (length < 40 || length >= GOODIX_MAX_READ || !goodix_read(GOODIX_IC_INFO, info, length) ||
	    !goodix_checksum_ok(info, length)) {
		printf("gt9615: bad chip description (length %u)\n", length);
		return false;
	}
	/* the length, the version (16 bytes), the features (10), four counts, then five lists of 16-bit numbers */
	offset = 2 + 16 + 10 + 4;
	for (int i = 0; i < 5; i++) {
		if (offset >= length) {
			return false;
		}
		offset += 1 + 2u * info[offset];
	}
	if (offset + 48 + 2 > length) {
		printf("gt9615: chip description too short (%u, lists end at %u)\n", length, offset);
		return false;
	}
	touch_address = le32(info + offset + 44);
	command_address = le32(info + offset + 0);      /* (the "misc" part: the command buffer comes first) */
	if (offset + 122 + 20 + 2 <= length) {
		uint16_t x = le16(info + offset + 122 + 16), y = le16(info + offset + 122 + 18);

		if (x && y) {
			max_x = x;
			max_y = y;
		}
	}
	printf("gt9615: touch report at %#x, coordinates up to %u x %u\n", touch_address, max_x, max_y);
	return touch_address != 0;
}

/*
 * A command to the chip (Android's brl_send_cmd(): goodix_brl_hw.c of the phone's kernel): sixteen
 * bytes at the command address -- state, acknowledgement, length, command, its data, and the sum of
 * the bytes from the length on -- then the acknowledgement looked for, 0x80 when done.
 */
#define GOODIX_CMD_HIGH_RATE    0xc0            /* data 1: report 480 times a second instead of 240 (Xiaomi's game mode) */
#define GOODIX_ACK_OK           0x80
#define GOODIX_ACK_BUSY         0x02

static bool
goodix_command(uint8_t command, uint8_t data)
{
	uint8_t buffer[16], answer[16];
	uint32_t sum;

	if (command_address == 0) {
		return false;
	}
	memset(buffer, 0, sizeof(buffer));
	buffer[2] = 5;
	buffer[3] = command;
	buffer[4] = data;
	sum = buffer[2] + buffer[3] + buffer[4];
	buffer[5] = (uint8_t)sum;
	buffer[6] = (uint8_t)(sum >> 8);
	for (int attempt = 0; attempt < 6; attempt++) {
		if (!goodix_write(command_address, buffer, sizeof(buffer))) {
			return false;
		}
		for (int look = 0; look < 6; look++) {
			if (!goodix_read(command_address, answer, sizeof(answer))) {
				return false;
			}
			if (answer[1] == GOODIX_ACK_OK) {
				return true;
			}
			sleep_ms(1);
			if (answer[1] != GOODIX_ACK_BUSY && answer[1] != 0) {
				break;
			}
		}
	}
	return false;
}

/* What has been seen so far: said once a minute, so that the end of the log always tells. */
static struct {
	uint32_t polls, reports, touches, bad, last_x, last_y;
} seen;

/* Which fingers were down in the last report, and where. */
static struct {
	bool down, seen;
	uint32_t x, y;
} contact[GOODIX_MAX_TOUCHES];
static uint32_t flip;           /* boot-arg touch_flip: 1 = mirror left-right, 2 = mirror top-bottom */

static void
goodix_report(const uint8_t *touches, uint32_t count)
{
	static unsigned shown;

	for (int i = 0; i < GOODIX_MAX_TOUCHES; i++) {
		contact[i].seen = false;
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *t = touches + 8 * i;
		uint32_t id = t[0] >> 4, kind = t[0] & 0xf;
		uint32_t x = le16(t + 2), y = le16(t + 4);

		if (id >= GOODIX_MAX_TOUCHES || kind == 1 || kind == 3) {       /* 1 and 3: a stylus */
			continue;
		}
		if (shown < 40) {
			shown++;
			printf("gt9615: finger %u at %u, %u (of %u x %u)\n", id, x, y, max_x, max_y);
		}
		seen.touches++;
		seen.last_x = x;
		seen.last_y = y;
		x = x >= max_x ? 65535 : x * 65535 / max_x;
		y = y >= max_y ? 65535 : y * 65535 / max_y;
		if (flip & 1) {
			x = 65535 - x;
		}
		if (flip & 2) {
			y = 65535 - y;
		}
		contact[id].seen = true;
		if (!contact[id].down) {
			redwood_touch_post(REDWOOD_TOUCH_DOWN, id, x, y);
		} else if (contact[id].x != x || contact[id].y != y) {
			redwood_touch_post(REDWOOD_TOUCH_MOVE, id, x, y);
		}
		contact[id].down = true;
		contact[id].x = x;
		contact[id].y = y;
	}
	/* a finger that is no longer in the report has lifted */
	for (uint32_t id = 0; id < GOODIX_MAX_TOUCHES; id++) {
		if (contact[id].down && !contact[id].seen) {
			contact[id].down = false;
			redwood_touch_post(REDWOOD_TOUCH_UP, id, contact[id].x, contact[id].y);
		}
	}
}

/* Read the report if there is one, and tell the chip it has been read. */
static void
goodix_poll(void)
{
	static uint8_t report[8 + 8 * GOODIX_MAX_TOUCHES + 2];
	static unsigned bad;
	const uint8_t zero = 0;
	const uint32_t first = 8 + 8 * 2 + 2;           /* the header and two fingers: all there is, most of the time */
	uint32_t count;

	seen.polls++;
	if (!goodix_read(touch_address, report, first) || report[0] == 0) {
		return;
	}
	seen.reports++;
	count = report[2] & 0xf;
	if ((report[0] & GOODIX_STATUS_TOUCH) && count > 2 && count <= GOODIX_MAX_TOUCHES &&
	    !goodix_read(touch_address + 8, report + 8, 8 * count + 2)) {
		return;
	}
	goodix_write(touch_address, &zero, 1);
	if (!goodix_checksum_ok(report, 8)) {
		seen.bad++;
		if (bad++ < 8) {
			printf("gt9615: bad report header: %02x %02x %02x %02x %02x %02x %02x %02x\n", report[0], report[1],
			    report[2], report[3], report[4], report[5], report[6], report[7]);
		}
		return;
	}
	if (report[0] & GOODIX_STATUS_REQUEST) {
		if ((report[2] & 0xf) == GOODIX_REQUEST_RESET || report[2] == GOODIX_REQUEST_RESET) {
			printf("gt9615: the chip asks to be reset\n");
			touch_reset();
		}
		return;
	}
	if (!(report[0] & GOODIX_STATUS_TOUCH)) {
		return;
	}
	if (count > GOODIX_MAX_TOUCHES || (count && !goodix_checksum_ok(report + 8, count * 8 + 2))) {
		seen.bad++;
		if (bad++ < 8) {
			printf("gt9615: bad report (%u touches)\n", count);
		}
		return;
	}
	goodix_report(report + 8, count);
}

/* ---- all of it together ---------------------------------------------------------------------------- */

static void
show_state(const char *when)
{
	printf("gt9615: %s: cbcr %#x, halts %#x %#x %#x, rate cfg %#x dfs %#x; pins cs/clk/mosi/miso cfg %#x %#x %#x %#x,"
	    " reset %#x/%u, int %#x/%u, avdd %#x/%u\n", when, rd(GCC_CBCR), rd(GCC_QUP3_M_AHB_HALT),
	    rd(GCC_QUP3_S_AHB_HALT), rd(GCC_QUP3_S2_HALT), rd(GCC_QUP3_S2_CFG), rd(GCC_QUP3_S2_DFSR), rd(GPIO_CFG(PIN_CS)),
	    rd(GPIO_CFG(PIN_CLK)), rd(GPIO_CFG(PIN_MOSI)), rd(GPIO_CFG(PIN_MISO)), rd(GPIO_CFG(PIN_RESET)),
	    rd(GPIO_IO(PIN_RESET)), rd(GPIO_CFG(PIN_INT)), rd(GPIO_IO(PIN_INT)), rd(GPIO_CFG(PIN_AVDD)), rd(GPIO_IO(PIN_AVDD)));
}

/* Power the chip up from nothing, in the order the task specifies: avdd -> iovdd -> reset -> irq.
 * Reset is held low first so the chip cannot run while the supplies come up; touch_reset()
 * releases it when the chip is probed. (redwood powered iovdd before avdd.) */
static void
power_on(void)
{
	gpio_output(PIN_RESET, false, PULL_UP, 8);     /* hold in reset while the supplies ramp */
	gpio_output(PIN_AVDD, true, PULL_NONE, 2);     /* avdd: touch_avdd_vreg_second, GPIO 82 */
	sleep_ms(3);
	io_supply_on();                                 /* iovdd: RPMh L2F_E0 1.2 V (idempotent; may already be on) */
	sleep_ms(3);
	gpio_configure(PIN_INT, 0, PULL_UP, 8, false); /* irq line as input, last */
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

	if (clocks_on()) {
		printf("gt9615: serial engine: firmware %#x, interface %#x, parameters %#x, source clock %u Hz (select %u)\n",
		    rd(GENI_FW_REVISION_RO), rd(GENI_IF_DISABLE_RO), rd(SE_HW_PARAM_0), se_source_hz, se_clock_select);
		uint32_t spi_hz = 1000000;

		/* boot-arg touch_spi_hz=N: the bus at N Hz (the DTB gives the chip 15 MHz) */
		PE_parse_boot_argn("touch_spi_hz", &spi_hz, sizeof(spi_hz));
		if (spi_hz < 100000 || spi_hz > 10000000) {
			spi_hz = 1000000;
		}
		printf("gt9615: the bus at %u Hz\n", spi_hz);
		problem = geni_spi_init(spi_hz);
	}
	if (problem == NULL) {
		use_engine_pins();
		spi_transfer = geni_transfer;
		show_state("powered, on the serial engine");
		touch_reset();
		if (goodix_probe(true)) {
			return true;
		}
		printf("gt9615: the chip did not answer through the serial engine; trying the pins by hand\n");
	} else {
		printf("gt9615: %s; driving the pins by hand\n", problem);
	}

	/* by hand; which of the two data pins is the input is not written down anywhere, so try both */
	spi_transfer = bitbang_transfer;
	for (int attempt = 0; attempt < 2; attempt++) {
		bitbang_in = attempt == 0 ? PIN_MISO : PIN_MOSI;
		bitbang_out = attempt == 0 ? PIN_MOSI : PIN_MISO;
		bitbang_init();
		touch_reset();
		if (goodix_probe(true)) {
			printf("gt9615: the chip answers on pins driven by hand (data in on gpio%u)\n", bitbang_in);
			return true;
		}
	}
	show_state("no answer");
	return false;
}

static uint64_t
micros_now(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

static void
gt9615_thread(__unused void *parameter, __unused wait_result_t result)
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
		printf("gt9615: no GT9615 touch screen\n");
		thread_terminate(current_thread());
		return;
	}
	printf("gt9615: GT9615 touch screen ready; the interrupt line is looked at every %u us\n", poll_us);
	{
		uint32_t rate = 0;

		/* boot-arg touch_rate=480: the chip reports twice as often (Xiaomi's game mode; 240 otherwise) */
		if (PE_parse_boot_argn("touch_rate", &rate, sizeof(rate)) && rate == 480) {
			bool done = goodix_command(GOODIX_CMD_HIGH_RATE, 1);

			report_rate = done ? "480 (acknowledged)" : "240 (480 NOT ACKNOWLEDGED)";
			printf("gt9615: reports 480 times a second (command %#x at %#x): %s\n", GOODIX_CMD_HIGH_RATE, command_address,
			    done ? "acknowledged" : "NOT ACKNOWLEDGED");
		}
	}
	for (unsigned tick = 1;; tick++) {
		uint64_t looked = micros_now(), read_at;

		/* the interrupt line is low while a report waits; look anyway every 80 ms in case it is missed */
		if (!gpio_get(PIN_INT)) {
			idle = 0;
			goodix_poll();
			read_at = micros_now();
			reads++;
			reading += read_at - looked;
			if (latency) {
				redwood_tl("tl: c before %llu seen %llu read %llu\n", before, looked, read_at);
			}
		} else if (++idle >= idle_looks) {
			idle = 0;
			goodix_poll();
			reads++;
			reading += micros_now() - looked;
		}
		before = looked;
		if (tick % (60 * (1000000 / poll_us)) == 0) {
			printf("gt9615: so far %u reports in %u looks (%u bad), %u finger positions, the last at %u, %u; "
			    "interrupt line %s; a report takes %llu us to read; the line looked at every %u us, reports %s a second\n",
			    seen.reports, seen.polls, seen.bad, seen.touches, seen.last_x, seen.last_y,
			    gpio_get(PIN_INT) ? "high" : "low", reads ? reading / reads : 0, poll_us, report_rate);
		}
		sleep_us(poll_us);
	}
}

void
popsicle_goodix_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	PE_parse_boot_argn("touch_flip", &flip, sizeof(flip));
	if (kernel_thread_start(gt9615_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
