/*
 * popsicle_audio.c -- Xiaomi 17 Pro Max (popsicle, SM8850 "canoe") audio survey.
 *
 * READ-ONLY survey of the audio chain, gated by boot-arg audio_probe=1, same shape
 * as popsicle_wifi.c: TLMM pin states (left by XBL/ABL) and the GENI serial-engine
 * status of the main Cirrus I2C bus, plus the GCC clock state. No bus transactions
 * are issued and no pins are written -- the I2C register map of the CS35L43 (closed
 * firmware handshake) and the AudioReach GPR client are the real driver's job.
 *
 * Hardware facts (audio-facts.md, DTB + real dmesg):
 *   - WCD9395 split codec: control I2C i2c@a8c000 @0xe (RST GPIO162), RX/TX over
 *     SoundWire (masters at 0x6ad0000 / 0x7630000).
 *   - CS35L43 x2 + CS40L26A on i2c-13 = i2c@1980000 (qupv3_3 GENI engine, the same
 *     engine whose SPI shape carries the GT9615 second-panel touch):
 *       cs35l43@40  addr 0x28 (reg 0x40), RST GPIO10,  GIC SPI 11,  prefix "B"
 *       cs35l43@42  addr 0x2a (reg 0x42), RST GPIO54,  GIC SPI 47,  prefix "T",
 *                   plus GPIO174 earpiece switch
 *       cs40l26@43  addr 0x43,            RST GPIO9 (active high), GIC SPI 80
 *   - All three Cirrus share TDM0 (48kHz/32bit/4slot, dmesg hw_params channels=3):
 *       SCK GPIO122, WS GPIO125, SD0 GPIO123, SD1 GPIO124 (base-tree pinctrl).
 *   - The ADSP speaks AudioReach GPR, NOT redwood's APR (ASM/ADM/AFE): that part of
 *     redwood_sound.c cannot be reused. QEMU has no audio hardware here; the virtio
 *     sound driver stays the QEMU path.
 *
 * Survey steps (all reads, no writes):
 *   step 1 - TLMM config + level of every audio GPIO above;
 *   step 2 - GENI engine status words of i2c@1980000 (0x1980000);
 *   step 3 - the GCC wrap3 CBCR at 0x52018 (which bits XBL left voted).
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
#define GENI_I2C_PHYS           0x01980000UL    /* i2c@1980000, the qupv3_3 engine (Cirrus trio) */
#define GENI_I2C_SIZE           0x00004000UL
#define GCC_PHYS                0x00100000UL    /* /soc/clock-controller@100000 */
#define GCC_SIZE                0x00010000UL

#define GPIO_CIRRUS_B_RST       10
#define GPIO_CIRRUS_T_RST       54
#define GPIO_CS40L26_RST        9
#define GPIO_WCD9395_RST        162
#define GPIO_TDM_SCK            122
#define GPIO_TDM_WS             125
#define GPIO_TDM_SD0            123
#define GPIO_TDM_SD1            124
#define GPIO_EARPIECE_SW        174

#define TLMM_CFG(pin)           (0x1000 + (pin) * 0x10)
#define TLMM_IO(pin)            (0x1004 + (pin) * 0x10)

/* GCC wrap3 branch-enable CBCR shared by the qupv3_3 engine (from the touch work:
 * gcc-kaanapali.c; this is the same engine family as the GT9615 SPI) */
#define GCC_CBCR_WRAP3          0x52018UL

static vm_offset_t tlmm, geni, gcc;

static void
audio_survey_print(const char *what, uint32_t value)
{
	printf("popsicle_audio: %s = 0x%x\n", what, value);
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

/* Step 1: the pins, exactly as XBL/ABL left them. */
static void
audio_step1_pins(void)
{
	audio_survey_print("cs35l43 B rst cfg/level", tlmm_read_cfg(GPIO_CIRRUS_B_RST));
	audio_survey_print("cs35l43 B rst level", tlmm_read_io(GPIO_CIRRUS_B_RST));
	audio_survey_print("cs35l43 T rst cfg", tlmm_read_cfg(GPIO_CIRRUS_T_RST));
	audio_survey_print("cs35l43 T rst level", tlmm_read_io(GPIO_CIRRUS_T_RST));
	audio_survey_print("cs40l26 rst cfg/level", tlmm_read_cfg(GPIO_CS40L26_RST));
	audio_survey_print("wcd9395 rst cfg/level", tlmm_read_cfg(GPIO_WCD9395_RST));
	audio_survey_print("tdm sck cfg (gpio122)", tlmm_read_cfg(GPIO_TDM_SCK));
	audio_survey_print("tdm ws cfg (gpio125)", tlmm_read_cfg(GPIO_TDM_WS));
	audio_survey_print("tdm sd0 cfg (gpio123)", tlmm_read_cfg(GPIO_TDM_SD0));
	audio_survey_print("tdm sd1 cfg (gpio124)", tlmm_read_cfg(GPIO_TDM_SD1));
	audio_survey_print("earpiece sw cfg (gpio174)", tlmm_read_cfg(GPIO_EARPIECE_SW));
}

/* Step 2: the GENI engine status. These are controller-side registers only; no
 * transaction is issued, the I2C slaves are not touched. */
static void
audio_step2_geni(void)
{
	audio_survey_print("geni status (0x40)", *(volatile uint32_t *)(geni + 0x40));
	audio_survey_print("geni irq status (0x50)", *(volatile uint32_t *)(geni + 0x50));
	audio_survey_print("geni m cmd ctrl (0x608)", *(volatile uint32_t *)(geni + 0x608));
}

/* Step 3: which wrap3 branch clocks XBL left voted on. */
static void
audio_step3_gcc(void)
{
	audio_survey_print("gcc wrap3 cbcr (0x52018)", *(volatile uint32_t *)(gcc + GCC_CBCR_WRAP3));
}

/* ---- the thread ---------------------------------------------------------------------------------
 * popsicle_wifi.c shape: run once, print, unmap, exit. */
static void
audio_thread(__unused void *parameter, __unused wait_result_t result)
{
	printf("popsicle_audio: WCD9395/Cirrus survey, boot-arg audio_probe=1\n");

	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	geni = ml_io_map(GENI_I2C_PHYS, GENI_I2C_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	if (tlmm == 0 || geni == 0 || gcc == 0) {
		printf("popsicle_audio: cannot map TLMM/GENI/GCC\n");
		if (tlmm != 0) {
			ml_io_unmap(tlmm, TLMM_SIZE);
		}
		if (geni != 0) {
			ml_io_unmap(geni, GENI_I2C_SIZE);
		}
		if (gcc != 0) {
			ml_io_unmap(gcc, GCC_SIZE);
		}
		thread_terminate(current_thread());
		return;
	}

	audio_step1_pins();
	audio_step2_geni();
	audio_step3_gcc();

	printf("popsicle_audio: survey done (pin levels are the boot-loader state; no I2C "
	       "transaction was issued -- no writes)\n");

	ml_io_unmap(gcc, GCC_SIZE);
	ml_io_unmap(geni, GENI_I2C_SIZE);
	ml_io_unmap(tlmm, TLMM_SIZE);
	thread_terminate(current_thread());
}

/* ---- the entry point ------------------------------------------------------------------------------
 * popsicle_syna.c L1502-1515: PE_parse_boot_argn("audio_probe"), then the thread. */
void
popsicle_audio_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("audio_probe", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	if (kernel_thread_start(audio_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
