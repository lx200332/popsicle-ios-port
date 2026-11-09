/*
 * popsicle_camera.c -- Xiaomi 17 Pro Max (popsicle, SM8850 "canoe") camera survey.
 *
 * READ-ONLY survey, gated by boot-arg camera_probe=1. The four sensors are
 * STX9500U (wide, s0) + OV50M (front, s1) + OV50M (ultra, s2) + S5KGN8 (tele 4x,
 * s3), all OFILM (biocam-modem-facts.md, getprop caesar+1 decode). Each sensor's
 * real driver is a CCI/I2C framework client; this file only reads board state so
 * the first real boot prints what XBL/ABL left behind -- no bus transactions are
 * issued (a probe-less CCI read of an unpowered sensor would hang or fault).
 *
 * Hardware facts (biocam-modem-facts.md):
 *   - CCI controllers: cci0@941b000, cci1@941c000, cci2@941d000; each reg 0x1000,
 *     GIC SPI 426/427/428, 37.5 MHz;
 *   - s0 (wide)  on cci0 master=1;            MCLK GPIO94,  reset GPIO106 (1P8? no: reset line), csiphy1
 *   - s1 (front) on i2c@884000 reg=0x10;      MCLK GPIO93,  reset GPIO7,   csiphy4
 *   - s2 (ultra) on i2c@880000 reg=0x36;      MCLK GPIO91,  reset GPIO3,   csiphy2
 *   - s3 (tele)  on cci0 master=0 shared clk; MCLK GPIO92,  reset GPIO15,  csiphy0
 *   - MCLK all 19.2 MHz; eeprom/actuator share the s2 bus at 0x51/0x50/0x9.
 *
 * Survey steps (all reads, no writes):
 *   step 1 - TLMM config + level of the four MCLK pins and the four reset pins;
 *   step 2 - CCI controller identity words (HW version register, safe to read).
 *
 * WRITE PATH (added later, boot-arg camera_i2c_probe=1, DEFAULT OFF):
 *   The survey above stays byte-for-byte what it was; everything below the
 *   "write path" banner only runs when camera_i2c_probe=1 is passed. It drives
 *   the MCLK and reset lines as plain GPIO and reads the csiphy register window
 *   raw. It deliberately does NOT issue a CCI/I2C transaction: the repo contains
 *   no CCI register map (see cam_cci_read_reg() for the evidence), and inventing
 *   one would be worse than reporting nothing.
 */

#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <vm/pmap.h>	/* VM_WIMG_IO for ml_io_map_unmappable */

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, uint32_t wimg);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern kern_return_t thread_terminate(thread_t thread);

/* ------------------------------------------------------------------ board facts */

#define TLMM_PHYS               0x0f100000UL    /* TRUE base: pinctrl-canoe.ko pin table = 0x100000 + 0x1000*n (wsl/pandora_touch.c:8-10); DT /soc/pinctrl@f000000 reg=<0x0f000000 0x202000> is the pre-offset base. 0x0f0xxxxx hangs the bus. */
#define TLMM_SIZE               0x00210000UL    /* >= DT 0x202000, covers 132 pins at 0x1000 stride */

#define CCI0_PHYS               0x0941b000UL    /* qcom,cci0@941b000 */
#define CCI1_PHYS               0x0941c000UL
#define CCI2_PHYS               0x0941d000UL
#define CCI_SIZE                0x00001000UL

#define GPIO_MCLK_S0            94
#define GPIO_MCLK_S1            93
#define GPIO_MCLK_S2            91
#define GPIO_MCLK_S3            92
#define GPIO_RST_S0             106
#define GPIO_RST_S1             7
#define GPIO_RST_S2             3
#define GPIO_RST_S3             15

#define TLMM_CFG(pin)           ((pin) * 0x1000)
#define TLMM_IO(pin)            ((pin) * 0x1000 + 4)

static vm_offset_t tlmm, cci0, cci1, cci2;

static void
cam_survey_print(const char *what, uint32_t value)
{
	printf("popsicle_camera: %s = 0x%x\n", what, value);
}

static uint32_t
tlmm_read_cfg(unsigned pin)
{
	return *(volatile uint32_t *)(tlmm + TLMM_CFG(pin));
}

static uint32_t
tlmm_read_io(unsigned pin)
{
	return *(volatile uint32_t *)(tlmm + TLMM_IO(pin));
}

/* Step 1: MCLK + reset pins exactly as XBL/ABL left them. */
static void
cam_step1_pins(void)
{
	cam_survey_print("s0 wide mclk cfg (gpio94)", tlmm_read_cfg(GPIO_MCLK_S0));
	cam_survey_print("s1 front mclk cfg (gpio93)", tlmm_read_cfg(GPIO_MCLK_S1));
	cam_survey_print("s2 ultra mclk cfg (gpio91)", tlmm_read_cfg(GPIO_MCLK_S2));
	cam_survey_print("s3 tele mclk cfg (gpio92)", tlmm_read_cfg(GPIO_MCLK_S3));
	cam_survey_print("s0 reset cfg (gpio106)", tlmm_read_cfg(GPIO_RST_S0));
	cam_survey_print("s1 reset cfg (gpio7)", tlmm_read_cfg(GPIO_RST_S1));
	cam_survey_print("s2 reset cfg (gpio3)", tlmm_read_cfg(GPIO_RST_S2));
	cam_survey_print("s3 reset cfg (gpio15)", tlmm_read_cfg(GPIO_RST_S3));
	cam_survey_print("s0 reset level", tlmm_read_io(GPIO_RST_S0));
	cam_survey_print("s3 reset level", tlmm_read_io(GPIO_RST_S3));
}

/* Step 2: CCI controller identity (safe reads; the CCI HW version register does
 * not depend on sensor power). No transaction is issued. */
static void
cam_step2_cci(void)
{
	cam_survey_print("cci0 hw version (0x0)", *(volatile uint32_t *)(cci0 + 0x0));
	cam_survey_print("cci0 hw version (0x4)", *(volatile uint32_t *)(cci0 + 0x4));
	cam_survey_print("cci1 hw version (0x0)", *(volatile uint32_t *)(cci1 + 0x0));
	cam_survey_print("cci2 hw version (0x0)", *(volatile uint32_t *)(cci2 + 0x0));
}

/* ====================================================================================
 * write path -- everything below is gated by camera_i2c_probe=1 (DEFAULT OFF)
 * ====================================================================================
 *
 * The write path exists so one boot can answer three questions the read-only
 * survey cannot: (a) do the TLMM registers accept a configuration store at all,
 * (b) what does the csiphy register window read back, (c) can a CCI transaction
 * be built from in-repo evidence. (c) is answered "no" and the reason is
 * recorded in code, not invented.
 */

/* csiphy windows: only the unit address is evidence (node names in
 * canoe_base.dts:19500/19525/19550/19575/19600). The `reg` properties of
 * csiphy0/1/2 are masked in the dumped base DT (they read as the strings
 * "R0"/"RP"/"Rp"), so the size 0x2000 below comes from the unmasked
 * csiphy3/4/5 nodes (canoe_base.dts:19621 reg=<0x0 0x9529000 0x0 0x2000>). */
#define CSIPHY0_PHYS            0x09523000UL    /* qcom,csiphy0@9523000 -- s3 tele */
#define CSIPHY1_PHYS            0x09525000UL    /* qcom,csiphy1@9525000 -- s0 wide */
#define CSIPHY2_PHYS            0x09527000UL    /* qcom,csiphy2@9527000 -- s2 ultra */
#define CSIPHY3_PHYS            0x09529000UL    /* qcom,csiphy3@9529000 -- unused */
#define CSIPHY4_PHYS            0x0952b000UL    /* qcom,csiphy4@952B000 -- s1 front */
#define CSIPHY_SIZE             0x00002000UL

#define TLMM_WR32(off, value)   (*(volatile uint32_t *)(ptlmm + (off)) = (uint32_t)(value))
#define TLMM_CFG_WR(pin, v)     TLMM_WR32(TLMM_CFG(pin), (v))
#define TLMM_IO_WR(pin, v)      TLMM_WR32(TLMM_IO(pin), (v))

#define TLMM_FUNC_GPIO          0u
#define TLMM_PULL_NONE          0u
#define TLMM_CTL(pull, func, ma, oe) \
	((uint32_t)(pull) | ((uint32_t)(func) << 2) | ((uint32_t)((ma) / 2 - 1) << 6) | ((oe) ? (1u << 9) : 0u))
#define TLMM_IO_LOW             0u              /* io word: bit0 = level in, bit1 = level out */
#define TLMM_IO_HIGH            2u

/* Sentinel addressing for the sensor side. Never a real address: a CCI
 * transaction must not be attempted with a value somebody guessed. */
#define CAM_ADDR_UNKNOWN        0xffu
#define CAM_REG_UNKNOWN         0xffffu

/* The probe thread has its own mappings: camera_probe=1 and camera_i2c_probe=1
 * may both be set, and two threads must not overwrite one vm_offset_t while the
 * other is dereferencing it. */
static vm_offset_t ptlmm;
static vm_offset_t pcsiphy0, pcsiphy1, pcsiphy2, pcsiphy3, pcsiphy4;

static uint32_t
ptlmm_read_cfg(unsigned pin)
{
	return *(volatile uint32_t *)(ptlmm + TLMM_CFG(pin));
}

static uint32_t
ptlmm_read_io(unsigned pin)
{
	return *(volatile uint32_t *)(ptlmm + TLMM_IO(pin));
}

/*
 * Drive one pin as plain GPIO to a static level, and prove the store took.
 *
 * This is popsicle_wifi.c's wifi_power_on_pin() shape (level first, then OE, then
 * read back) applied to the camera pins. It is NOT a MCLK enable: an MCLK pin
 * only produces a clock once the TLMM mux selects the cam_mclk /
 * cam_asc_mclk2 function, and those function *numbers* appear nowhere in this
 * repo -- the DT pinctrl nodes (canoe_base.dts:5178/5202/5226/5250) name the
 * function as a string only. So the pin is left parked at a static level with
 * the mux on GPIO; the sensor sees no clock and no transaction can occur.
 */
static void
cam_drive_pin(unsigned pin, const char *what, uint32_t level)
{
	uint32_t cfg_before = ptlmm_read_cfg(pin);
	uint32_t io_before = ptlmm_read_io(pin);
	uint32_t cfg_value = (cfg_before & ~0x3ffu) |
	    TLMM_CTL(TLMM_PULL_NONE, TLMM_FUNC_GPIO, 2, 1);
	uint32_t cfg_after, io_after;

	printf("popsicle_camera: drive %s (gpio%u) before cfg=0x%08x io=0x%08x\n",
	    what, pin, cfg_before, io_before);
	TLMM_IO_WR(pin, level);           /* latch the level before OE turns on */
	TLMM_CFG_WR(pin, cfg_value);      /* mux = GPIO, no pull, 2 mA, output */
	cfg_after = ptlmm_read_cfg(pin);
	io_after = ptlmm_read_io(pin);
	/* Two separate verdicts on purpose. cfg==value proves the mux/pull/drive
	 * store landed. io bit1 is the driven output level -- but a low level reads
	 * back 0 whether or not the store worked, so the level check only has
	 * evidence value on the HIGH edges. Say so instead of printing one
	 * combined "OK" that hides an ambiguous low. */
	printf("popsicle_camera: drive %s (gpio%u) level=%u after cfg=0x%08x io=0x%08x "
	    "cfg=%s level=%s\n",
	    what, pin, (unsigned)(level >> 1), cfg_after, io_after,
	    (cfg_after == cfg_value) ? "OK" : "MISMATCH",
	    ((io_after & 2u) == (level & 2u))
	    ? ((level & 2u) ? "OK" : "OK-but-low-is-indistinguishable")
	    : "MISMATCH");
}

/* Step 3: park the four MCLK pins. No clock is produced; see cam_drive_pin(). */
static void
cam_step3_mclk(void)
{
	printf("popsicle_camera: step3 mclk -- cam_mclk/cam_asc_mclk2 TLMM function numbers "
	    "are not in repo evidence, so the mux stays on GPIO and these pins output a "
	    "static level, not a 19.2 MHz clock\n");

	cam_drive_pin(GPIO_MCLK_S0, "s0 wide mclk", TLMM_IO_HIGH);
	cam_drive_pin(GPIO_MCLK_S1, "s1 front mclk", TLMM_IO_HIGH);
	cam_drive_pin(GPIO_MCLK_S2, "s2 ultra mclk", TLMM_IO_HIGH);
	cam_drive_pin(GPIO_MCLK_S3, "s3 tele mclk", TLMM_IO_HIGH);
}

/* Step 4: reset pulse, low -> settle -> high -> settle, with read-back each edge.
 * Reset lines are active low, so the sensor is held in reset for 1 ms and then
 * released. */
static void
cam_step4_reset(void)
{
	printf("popsicle_camera: step4 reset -- low 1ms, high, settle 5ms\n");

	cam_drive_pin(GPIO_RST_S0, "s0 wide reset", TLMM_IO_LOW);
	cam_drive_pin(GPIO_RST_S1, "s1 front reset", TLMM_IO_LOW);
	cam_drive_pin(GPIO_RST_S2, "s2 ultra reset", TLMM_IO_LOW);
	cam_drive_pin(GPIO_RST_S3, "s3 tele reset", TLMM_IO_LOW);
	delay_for_interval(1000, NSEC_PER_USEC);
	cam_drive_pin(GPIO_RST_S0, "s0 wide reset", TLMM_IO_HIGH);
	cam_drive_pin(GPIO_RST_S1, "s1 front reset", TLMM_IO_HIGH);
	cam_drive_pin(GPIO_RST_S2, "s2 ultra reset", TLMM_IO_HIGH);
	cam_drive_pin(GPIO_RST_S3, "s3 tele reset", TLMM_IO_HIGH);
	delay_for_interval(5000, NSEC_PER_USEC);

	/* DT says the tele reset is gpio105 (popsicle_overlay.dts:21287,
	 * gpios = <tlmm 0x5c 0 tlmm 0x69 0>) while the board facts say gpio15
	 * (biocam-modem-facts.md:61-68). Both are printed rather than picked. */
	printf("popsicle_camera: step4 note -- s3 tele reset is gpio105 in "
	    "dts/popsicle_overlay.dts:21287 but gpio15 in biocam-modem-facts.md:61-68; "
	    "this probe drives gpio15 as the task specified\n");
}

/* Step 5: read the csiphy window raw. The addresses are evidence; the meaning of
 * word 0 is not, so nothing is decoded and no power state is claimed. Reading a
 * powered-down CAM domain can fault the interconnect -- this only runs with
 * camera_i2c_probe=1 for exactly that reason. */
static void
cam_csiphy_word(const char *what, vm_offset_t base)
{
	if (base == 0) {
		printf("popsicle_camera: csiphy %s -- not mapped, skipped\n", what);
		return;
	}
	printf("popsicle_camera: csiphy %s word0=0x%08x word1=0x%08x "
	    "(raw; REG_0 / PHY-status layout is not in repo evidence, not decoded)\n",
	    what, *(volatile uint32_t *)(base + 0x0),
	    *(volatile uint32_t *)(base + 0x4));
}

static void
cam_step5_csiphy(void)
{
	cam_csiphy_word("0 (s3 tele)", pcsiphy0);
	cam_csiphy_word("1 (s0 wide)", pcsiphy1);
	cam_csiphy_word("2 (s2 ultra)", pcsiphy2);
	cam_csiphy_word("3 (unused)", pcsiphy3);
	cam_csiphy_word("4 (s1 front)", pcsiphy4);
}

/*
 * Step 6: the CCI read -- SKELETON ONLY, and this is a deliberate result.
 *
 * What the repo does prove about CCI:
 *   - base addresses 0x0941b000 / 0x0941c000 / 0x0941d000 and a 0x1000 window
 *     (canoe_base.dts:19670, :19734 area, :19818 area);
 *   - IRQ 426/427/428 (canoe_base.dts:19666 interrupts = <0x0 0x1aa 0x1>);
 *   - clock 37.5 MHz (canoe_base.dts:19662 clock-rates = <0x23c3460 ...>) and the
 *     cci_0_clk_src / cci_0_clk pair (:19663-19664);
 *   - per-mode I2C timing children (qcom,i2c_fast_mode etc.) that carry only
 *     hw-tsp / hw-tlow / hw-thigh, i.e. I2C *timing*, never a register offset.
 *
 * What it does NOT prove, anywhere: the CCI register map. grep over the repo and
 * over the kernel build tree for CCI_HW_VERSION / CCI_QUEUE / cam_cci / cci_queue
 * returns zero hits outside this file, and there is no cam_cci source in the
 * kernel tree. Without the offset of the queue head, the command register and the
 * read-data register there is no way to start a transfer, so this function
 * refuses instead of guessing.
 *
 * The sensor side is just as unproven: dts/popsicle_overlay.dts gives an I2C
 * address for s1 (0x10, :21466) and s2 (0x36, :21390) but none for s0/s3 on
 * cci0, and no sensor datasheet in the repo gives a chip-id register offset for
 * STX9500U / OV50M / S5KGN8.
 */
static kern_return_t
cam_cci_read_reg(unsigned cci, unsigned master, uint8_t addr, uint16_t reg, uint8_t *value)
{
	(void)cci;
	(void)master;
	(void)addr;
	(void)reg;
	(void)value;

	/* Filled in when a CCI register map (queue head, command, read-data) comes
	 * into the tree from a vendor cam_cci source or a hardware dump. Until then
	 * KERN_NOT_SUPPORTED is the honest return. */
	return KERN_NOT_SUPPORTED;
}

static void
cam_probe_one(const char *what, unsigned cci, unsigned master, uint8_t addr, uint16_t reg)
{
	uint8_t value = 0;
	kern_return_t kr;

	kr = cam_cci_read_reg(cci, master, addr, reg, &value);
	if (addr == CAM_ADDR_UNKNOWN || reg == CAM_REG_UNKNOWN) {
		printf("popsicle_camera: probe %s -- skipped (kr=%d): sensor i2c address "
		    "and/or chip-id register offset is not in repo evidence\n", what, (int)kr);
		return;
	}
	printf("popsicle_camera: probe %s addr=0x%02x reg=0x%04x -- skipped (kr=%d): "
	    "CCI controller register map is not in repo evidence\n",
	    what, (unsigned)addr, (unsigned)reg, (int)kr);
}

static void
cam_step6_cci_probe(void)
{
	printf("popsicle_camera: step6 cci read -- NOT IMPLEMENTED BY DESIGN; the four "
	    "chip-id reads below report why\n");

	cam_probe_one("s0 wide STX9500U", 0, 1, CAM_ADDR_UNKNOWN, CAM_REG_UNKNOWN);
	cam_probe_one("s1 front OV50M", 0, 0, 0x10u, CAM_REG_UNKNOWN);
	cam_probe_one("s2 ultra OV50M", 0, 0, 0x36u, CAM_REG_UNKNOWN);
	cam_probe_one("s3 tele S5KGN8", 0, 0, CAM_ADDR_UNKNOWN, CAM_REG_UNKNOWN);
}

/* ---- the probe thread ----------------------------------------------------------------------------
 * Same shape as camera_thread: run once, print, exit. The mappings are permanent
 * (ml_io_map_unmappable), so nothing is unmapped. */
static void
camera_probe_thread(__unused void *parameter, __unused wait_result_t result)
{
	printf("popsicle_camera: write path, boot-arg camera_i2c_probe=1 "
	    "(mclk/reset drive + csiphy raw + cci skeleton)\n");

	ptlmm = ml_io_map_unmappable(TLMM_PHYS, TLMM_SIZE, VM_WIMG_IO);
	pcsiphy0 = ml_io_map_unmappable(CSIPHY0_PHYS, CSIPHY_SIZE, VM_WIMG_IO);
	pcsiphy1 = ml_io_map_unmappable(CSIPHY1_PHYS, CSIPHY_SIZE, VM_WIMG_IO);
	pcsiphy2 = ml_io_map_unmappable(CSIPHY2_PHYS, CSIPHY_SIZE, VM_WIMG_IO);
	pcsiphy3 = ml_io_map_unmappable(CSIPHY3_PHYS, CSIPHY_SIZE, VM_WIMG_IO);
	pcsiphy4 = ml_io_map_unmappable(CSIPHY4_PHYS, CSIPHY_SIZE, VM_WIMG_IO);
	/* cci0/1/2 are deliberately NOT mapped here: cam_cci_read_reg() returns
	 * KERN_NOT_SUPPORTED without touching the window (no register map). */

	if (ptlmm == 0) {
		printf("popsicle_camera: cannot map TLMM, write path skipped\n");
		thread_terminate(current_thread());
		return;
	}

	cam_step3_mclk();
	cam_step4_reset();
	cam_step5_csiphy();
	cam_step6_cci_probe();

	printf("popsicle_camera: write path done (mclk pins are static levels, not "
	    "clocks; cci window never touched)\n");
/* PANDORA: ml_io_map_unmappable mappings are permanent; unmap panics (pandora_dpu.c r1b) */
	thread_terminate(current_thread());
}

/* ---- the thread ---------------------------------------------------------------------------------
 * popsicle_wifi.c shape: run once, print, unmap, exit. */
static void
camera_thread(__unused void *parameter, __unused wait_result_t result)
{
	printf("popsicle_camera: 4-sensor OFILM survey, boot-arg camera_probe=1\n");

	tlmm = ml_io_map_unmappable(TLMM_PHYS, TLMM_SIZE, VM_WIMG_IO);
	cci0 = ml_io_map_unmappable(CCI0_PHYS, CCI_SIZE, VM_WIMG_IO);
	cci1 = ml_io_map_unmappable(CCI1_PHYS, CCI_SIZE, VM_WIMG_IO);
	cci2 = ml_io_map_unmappable(CCI2_PHYS, CCI_SIZE, VM_WIMG_IO);
	if (tlmm == 0 || cci0 == 0 || cci1 == 0 || cci2 == 0) {
		printf("popsicle_camera: cannot map TLMM/CCI\n");
		if (tlmm != 0) {
/* PANDORA: ml_io_map_unmappable mappings are permanent; unmap panics (pandora_dpu.c r1b) */
		}
		if (cci0 != 0) {
/* PANDORA: ml_io_map_unmappable mappings are permanent; unmap panics (pandora_dpu.c r1b) */
		}
		if (cci1 != 0) {
/* PANDORA: ml_io_map_unmappable mappings are permanent; unmap panics (pandora_dpu.c r1b) */
		}
		if (cci2 != 0) {
/* PANDORA: ml_io_map_unmappable mappings are permanent; unmap panics (pandora_dpu.c r1b) */
		}
		thread_terminate(current_thread());
		return;
	}

	cam_step1_pins();
	cam_step2_cci();

	printf("popsicle_camera: survey done (pin levels are the boot-loader state; "
	       "no sensor transaction was issued -- no writes)\n");
/* PANDORA: ml_io_map_unmappable mappings are permanent; unmap panics (pandora_dpu.c r1b) */
/* PANDORA: ml_io_map_unmappable mappings are permanent; unmap panics (pandora_dpu.c r1b) */
/* PANDORA: ml_io_map_unmappable mappings are permanent; unmap panics (pandora_dpu.c r1b) */
/* PANDORA: ml_io_map_unmappable mappings are permanent; unmap panics (pandora_dpu.c r1b) */
	thread_terminate(current_thread());
}

/* ---- the entry point ------------------------------------------------------------------------------
 * popsicle_syna.c L1502-1515: PE_parse_boot_argn("camera_probe"), then the thread.
 * camera_i2c_probe is a second, independent gate: with camera_probe=1 alone the
 * translation unit behaves exactly as it did before the write path existed. */
void
popsicle_camera_init(void)
{
	uint32_t enabled = 0;
	uint32_t probe = 0;
	thread_t thread;

	if (PE_parse_boot_argn("camera_probe", &enabled, sizeof(enabled)) && enabled) {
		if (kernel_thread_start(camera_thread, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
	if (PE_parse_boot_argn("camera_i2c_probe", &probe, sizeof(probe)) && probe) {
		if (kernel_thread_start(camera_probe_thread, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
}
