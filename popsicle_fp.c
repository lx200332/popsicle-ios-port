/*
 * popsicle_fp.c -- Xiaomi 17 Pro Max (popsicle, SM8850 "canoe") fingerprint survey.
 *
 * READ-ONLY survey, gated by boot-arg fp_probe=1. The real driver is Xiaomi's
 * mi_fp (fp_driver.c: fp_probe(struct spi_device*), compatible "xiaomi,xiaomi-fp",
 * netlink-event 25, vendor string P2_n2_132_431) -- a standard SPI-framework driver
 * whose SPEED and protocol live in the vendor driver, not in this file.
 *
 * What is KNOWN (biocam-modem-facts.md):
 *   - IRQ / IPC line = TLMM GPIO 99 (fp_irq, fingerprint_irq_default pinctrl);
 *   - fp_1v8 = fixed regulator behind GPIO 56;
 *   - fp_3v3 = RPMh L9B (pmh0101_l9 3.3 V) -- an RPMh VRM, readable only through the
 *     AOP mailbox, so this survey reports it as "not readable here";
 *   - vendor string P2_n2_132_431, fp screen = the second (976x596) panel's Goodix
 *     GT9615 touch (fingerprint-screen), i.e. the sensor is under the second screen.
 *
 * What is BLOCKED (the honest part): the SPI bus of the sensor is not in the vendor
 * DTB -- the four spi@ nodes are ir-spi (se0), GT9615 touch (spi@1988000, se15),
 * spi_cnss (spi@1990000) and the Synaptics TCM (spi@1a80000, se19); there is no
 * fingerprint SPI child, no xiaomi-fp probe line in the real dmesg, and no
 * fingerprint device in input-devices.txt. So this survey only prints the pin state
 * XBL/ABL left behind (the first piece of evidence needed to find the bus on the
 * device); the real driver stays blocked on "SPI 通路无据".
 *
 * Survey step (all reads, no writes): TLMM config + level of GPIO 99 and GPIO 56.
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

#define GPIO_FP_IRQ             99              /* fp_irq: IRQ + IPC line (tlmm 99) */
#define GPIO_FP_1V8_EN          56              /* fingerprint_1v8_vdd fixed regulator */

#define TLMM_CFG(pin)           (0x1000 + (pin) * 0x10)
#define TLMM_IO(pin)            (0x1004 + (pin) * 0x10)

static vm_offset_t tlmm;

static void
fp_survey_print(const char *what, uint32_t value)
{
	printf("popsicle_fp: %s = 0x%x\n", what, value);
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

/* ---- the thread ---------------------------------------------------------------------------------
 * popsicle_wifi.c shape: run once, print, unmap, exit. */
static void
fp_thread(__unused void *parameter, __unused wait_result_t result)
{
	printf("popsicle_fp: fingerprint survey, boot-arg fp_probe=1\n");

	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	if (tlmm == 0) {
		printf("popsicle_fp: cannot map TLMM\n");
		thread_terminate(current_thread());
		return;
	}

	fp_survey_print("irq/ipc cfg (gpio99)", tlmm_read_cfg(GPIO_FP_IRQ));
	fp_survey_print("irq/ipc level", tlmm_read_io(GPIO_FP_IRQ));
	fp_survey_print("1v8-en cfg (gpio56)", tlmm_read_cfg(GPIO_FP_1V8_EN));
	fp_survey_print("1v8-en level", tlmm_read_io(GPIO_FP_1V8_EN));

	printf("popsicle_fp: survey done; the sensor SPI bus is NOT in the DTB "
	       "(blocked on 'SPI 通路无据' -- the four spi@ nodes are ir-spi / GT9615 touch / "
	       "spi_cnss / syna TCM). Pin levels above are the boot-loader state; no writes)\n");

	ml_io_unmap(tlmm, TLMM_SIZE);
	thread_terminate(current_thread());
}

/* ---- the entry point ------------------------------------------------------------------------------
 * popsicle_syna.c L1502-1515: PE_parse_boot_argn("fp_probe"), then the thread. */
void
popsicle_fp_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("fp_probe", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	if (kernel_thread_start(fp_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
