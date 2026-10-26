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
 */

#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern kern_return_t thread_terminate(thread_t thread);

/* ------------------------------------------------------------------ board facts */

#define TLMM_PHYS               0x0f000000UL    /* /soc/pinctrl@f000000 (popsicle); NOT redwood's 0x0f100000 */
#define TLMM_SIZE               0x00210000UL

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

#define TLMM_CFG(pin)           (0x1000 + (pin) * 0x10)
#define TLMM_IO(pin)            (0x1004 + (pin) * 0x10)

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

/* ---- the thread ---------------------------------------------------------------------------------
 * popsicle_wifi.c shape: run once, print, unmap, exit. */
static void
camera_thread(__unused void *parameter, __unused wait_result_t result)
{
	printf("popsicle_camera: 4-sensor OFILM survey, boot-arg camera_probe=1\n");

	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	cci0 = ml_io_map(CCI0_PHYS, CCI_SIZE);
	cci1 = ml_io_map(CCI1_PHYS, CCI_SIZE);
	cci2 = ml_io_map(CCI2_PHYS, CCI_SIZE);
	if (tlmm == 0 || cci0 == 0 || cci1 == 0 || cci2 == 0) {
		printf("popsicle_camera: cannot map TLMM/CCI\n");
		if (tlmm != 0) {
			ml_io_unmap(tlmm, TLMM_SIZE);
		}
		if (cci0 != 0) {
			ml_io_unmap(cci0, CCI_SIZE);
		}
		if (cci1 != 0) {
			ml_io_unmap(cci1, CCI_SIZE);
		}
		if (cci2 != 0) {
			ml_io_unmap(cci2, CCI_SIZE);
		}
		thread_terminate(current_thread());
		return;
	}

	cam_step1_pins();
	cam_step2_cci();

	printf("popsicle_camera: survey done (pin levels are the boot-loader state; "
	       "no sensor transaction was issued -- no writes)\n");

	ml_io_unmap(cci2, CCI_SIZE);
	ml_io_unmap(cci1, CCI_SIZE);
	ml_io_unmap(cci0, CCI_SIZE);
	ml_io_unmap(tlmm, TLMM_SIZE);
	thread_terminate(current_thread());
}

/* ---- the entry point ------------------------------------------------------------------------------
 * popsicle_syna.c L1502-1515: PE_parse_boot_argn("camera_probe"), then the thread. */
void
popsicle_camera_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("camera_probe", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	if (kernel_thread_start(camera_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
