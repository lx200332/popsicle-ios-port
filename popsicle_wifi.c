/*
 * popsicle_wifi.c -- FastConnect 7900 (ath12k family) READ-ONLY survey driver.
 *
 * The radio of the Xiaomi 17 Pro Max (popsicle, SM8850 "canoe"). Everything here is a
 * survey of the hardware, gated by boot-arg wifi_probe=1, in the style of
 * redwood_goodix.c's board machinery and popsicle_syna.c's entry point: every address
 * belongs to this phone, every read is announced before it happens, and nothing is
 * written. It is compiled into the kernel so the first real boot can print what the
 * hardware actually answers, before any ath12k-sized port is attempted.
 *
 * Hardware facts (vendor merged DTB /soc/qcom,cnss-peach@b0000000, fdt.mjs decode):
 *   - interconnect is PCIe: /soc/pcie@1c00000/pcie_rp/cnss_pci0 (reg all-zero = assigned
 *     by PCIe enumeration), qcom,iommu-group; ath12k-class radio, NOT redwood's WCN6750
 *     (ath11k / WPSS Hexagon DSP).
 *   - platform node reg <0xb0000000 0x10000> is named "smmu_iova_ipa" (the SMMU IOVA
 *     window, not a control register block).
 *   - GPIO (TLMM 0x0f000000, phandle 0x30): wlan-en 16, sw-ctrl 18, bt-en 5 (phandle
 *     0xa4), host-sol 204, dev-sol 205.
 *   - Supplies (pmu_vreg_map, verbatim): VDDD_AON_0P9=S2J1, VDDA_RFA_0P9=S2J1,
 *     VDDA_RFA_1P9=S8F0, VDDA_RFA_1P3=S7F0, VDDD_WLMX_0P9=S2J1, VDDD_WLCX_0P9=S1J1,
 *     VDDD_BTCX_0P9=S2J1, VDDD_BTCMX_0P9=S2J1, VDDA_PCIE_0P9=S7F0, VDDA_PCIE_1P2=S7F0.
 *   - PCIe root: parf 0x1c00000 (reg[0] "parf"), GIC SPI 396/405 from interrupts
 *     cells 0x18c/0x195.
 *   - A legacy SPI cnss also exists (spi@1990000/spi_cnss, irq-gpio 85) but the active
 *     shape on this device is PCIe (cnss_pci0 is the child of pcie_rp).
 *   - Firmware: /vendor/firmware_mnt/image (qca_cld3_peach_v2 / peach bdf), TZ-authed;
 *     the driver talks to it, it does not run it.
 *
 * Survey steps (all reads, no writes):
 *   step 1 - TLMM config + level of the four WLAN GPIOs (state left by XBL/ABL);
 *   step 2 - PCIe root PARF identity words (safe before link training).
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

#define TLMM_PHYS               0x0f000000UL    /* /soc/pinctrl@f000000 (popsicle); redwood's 0x0f100000 must NOT be copied */
#define TLMM_SIZE               0x00210000UL    /* 132 pins * 0x10 + 0x1000, page-aligned up */
#define PCIE_PARF_PHYS          0x01c00000UL    /* /soc/pcie@1c00000 reg[0] "parf" */
#define PCIE_PARF_SIZE          0x00010000UL

#define GPIO_WLAN_EN            16
#define GPIO_SW_CTRL            18
#define GPIO_HOST_SOL           204
#define GPIO_DEV_SOL            205

/* TLMM GPIO registers (offset = 0x1000 + pin*0x10 on this SoC) */
#define TLMM_CFG(pin)           (0x1000 + (pin) * 0x10)
#define TLMM_IO(pin)            (0x1004 + (pin) * 0x10)

static vm_offset_t tlmm, pcie_parf;

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

static void
wifi_survey_print(const char *what, uint32_t value)
{
	printf("popsicle_wifi: %s = 0x%x\n", what, value);
}

/*
 * Step 1: read the pins. The TLMM config registers tell what each pin is muxed to;
 * the io registers tell the current level. Nothing is written.
 */
static void
wifi_step1_pins(void)
{
	wifi_survey_print("wlan-en cfg (gpio16)", tlmm_read_cfg(GPIO_WLAN_EN));
	wifi_survey_print("wlan-en level", tlmm_read_io(GPIO_WLAN_EN));
	wifi_survey_print("sw-ctrl cfg (gpio18)", tlmm_read_cfg(GPIO_SW_CTRL));
	wifi_survey_print("host-sol cfg (gpio204)", tlmm_read_cfg(GPIO_HOST_SOL));
	wifi_survey_print("dev-sol cfg (gpio205)", tlmm_read_cfg(GPIO_DEV_SOL));
}

/*
 * Step 2: read the PCIe root port's PARF block, safe even before the link is
 * trained. The first words are the controller's version/identity.
 */
static void
wifi_step2_pcie(void)
{
	wifi_survey_print("pcie parf word0 (identity)", *(volatile uint32_t *)(pcie_parf + 0x0));
	wifi_survey_print("pcie parf word1", *(volatile uint32_t *)(pcie_parf + 0x4));
	wifi_survey_print("pcie parf word2", *(volatile uint32_t *)(pcie_parf + 0x8));
}

/* ---- the thread ---------------------------------------------------------------------------------
 * popsicle_syna.c L1446: same polled-thread shape; the survey runs once and exits. */
static void
wifi_thread(__unused void *parameter, __unused wait_result_t result)
{
	printf("popsicle_wifi: FastConnect 7900 survey, boot-arg wifi_probe=1\n");

	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	pcie_parf = ml_io_map(PCIE_PARF_PHYS, PCIE_PARF_SIZE);
	if (tlmm == 0 || pcie_parf == 0) {
		printf("popsicle_wifi: cannot map TLMM/PCIe PARF\n");
		if (tlmm != 0) {
			ml_io_unmap(tlmm, TLMM_SIZE);
		}
		if (pcie_parf != 0) {
			ml_io_unmap(pcie_parf, PCIE_PARF_SIZE);
		}
		thread_terminate(current_thread());
		return;
	}

	wifi_step1_pins();
	wifi_step2_pcie();

	printf("popsicle_wifi: survey done (GPIO levels are the boot-loader state; "
	       "the only register reads were TLMM config/level and the PCIe PARF identity -- no writes)\n");

	ml_io_unmap(pcie_parf, PCIE_PARF_SIZE);
	ml_io_unmap(tlmm, TLMM_SIZE);
	thread_terminate(current_thread());
}

/* ---- the entry point ------------------------------------------------------------------------------
 * popsicle_syna.c L1502-1515: PE_parse_boot_argn("wifi_probe"), then the thread. */
void
popsicle_wifi_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("wifi_probe", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	if (kernel_thread_start(wifi_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
