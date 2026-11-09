/*
 * popsicle_wifi.c -- FastConnect 7900 (ath12k family) survey driver.
 *
 * The radio of the Xiaomi 17 Pro Max (popsicle, SM8850 "canoe"). Everything here is a
 * survey of the hardware, gated by boot-arg wifi_probe=1, in the style of
 * redwood_goodix.c's board machinery and popsicle_syna.c's entry point: every address
 * belongs to this phone, and every access is announced before it happens. Reads only --
 * the single exception is step 0, the optional power-on, which sits behind a *second*
 * boot-arg (wifi_poweron=1, default 0) and declares and reads back every store it makes.
 * It is compiled into the kernel so the first real boot can print what the hardware
 * actually answers, before any ath12k-sized port is attempted.
 *
 * Hardware facts (vendor merged DTB /soc/qcom,cnss-peach@b0000000, fdt.mjs decode):
 *   - interconnect is PCIe: /soc/pcie@1c00000/pcie_rp/cnss_pci0 (reg all-zero = assigned
 *     by PCIe enumeration), qcom,iommu-group; ath12k-class radio, NOT redwood's WCN6750
 *     (ath11k / WPSS Hexagon DSP).
 *   - platform node reg <0xb0000000 0x10000> is named "smmu_iova_ipa" (the SMMU IOVA
 *     window, not a control register block).
 *   - GPIO: TLMM TRUE base 0x0f100000 (phandle 0x30), one 0x1000 window per pin, so
 *     pin n config = 0x0f100000 + 0x1000*n. Pins: wlan-en 16, sw-ctrl 18, bt-en 5
 *     (phandle 0xa4), host-sol 204, dev-sol 205.
 *   - Supplies (pmu_vreg_map, verbatim): VDDD_AON_0P9=S2J1, VDDA_RFA_0P9=S2J1,
 *     VDDA_RFA_1P9=S8F0, VDDA_RFA_1P3=S7F0, VDDD_WLMX_0P9=S2J1, VDDD_WLCX_0P9=S1J1,
 *     VDDD_BTCX_0P9=S2J1, VDDD_BTCMX_0P9=S2J1, VDDA_PCIE_0P9=S7F0, VDDA_PCIE_1P2=S7F0.
 *   - PCIe root: parf 0x1c00000 (reg[0] "parf"), GIC SPI 396/405 from interrupts
 *     cells 0x18c/0x195.
 *   - A legacy SPI cnss also exists (spi@1990000/spi_cnss, irq-gpio 85) but the active
 *     shape on this device is PCIe (cnss_pci0 is the child of pcie_rp).
 *   - Firmware (OTA inventory `out/modem/image`, byte-identical to the device's
 *     /vendor/firmware_mnt/image per out/firmware-vs-device.json: verdict IDENTICAL,
 *     image_root_dirs = kiwi/oemvm/peach). The radio payload is the per-chip dir
 *     "peach": /vendor/firmware_mnt/image/peach/ -- 100 files, 21332357 bytes; board
 *     data bdwlan.elf (125420) plus the per-board bdwlan.e01..e49 series (70 files,
 *     125420 each, three absent: e08/e28/e48) and the bd_p*, bd_q* and bd_songyuan*
 *     .elf board variants; DSP amss20.bin (7319616); phy_ucode.elf / phy_ucode20.elf
 *     (290816 each); aux_ucode.elf / aux_ucode20.elf (61440 each); regdb.bin /
 *     regdb_xiaomi.bin (24278 each). NOTE: a "peach" filename glob matches zero files
 *     -- the chip name is the DIRECTORY, not the filenames. Driver side
 *     (qca_cld3_peach_v2.ko, 21411008 B, in /vendor_dlkm/lib/modules) resolves
 *     /vendor/firmware/wlan/qca_cld/peach_v2 (two symlinks:
 *     WCNSS_qcom_cfg.ini -> /vendor/etc/wifi/peach_v2/WCNSS_qcom_cfg.ini (8645 B) and
 *     wlan_mac.bin -> /mnt/vendor/persist/wlan/wlan_mac.bin). TZ-authed: the driver
 *     hands the bytes over, it does not run them.
 *
 * Survey steps (reads unless stated):
 *   step 0 - OPTIONAL power-on (wifi_poweron=1 only): mux wlan-en 16 / sw-ctrl 18 to GPIO
 *            function, drive high, print before/after of every store;
 *   step 1 - TLMM config + level of the four WLAN GPIOs (boot-loader state when
 *            wifi_poweron=0, and step 0's read-back when it is 1);
 *   step 2 - PCIe root PARF identity words (safe before link training);
 *   step 2b - PCIe root PARF 0x00..0x7c raw hex dump; no semantics are assumed for it,
 *            the offsets are printed and not interpreted.
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

void popsicle_wifi_init(void);    /* called from sweet_ufs.c; prototype keeps -Wmissing-prototypes quiet */

/* ------------------------------------------------------------------ board facts */

#define TLMM_PHYS               0x0f100000UL    /* TRUE base: pinctrl-canoe.ko pin table = 0x100000 + 0x1000*n (wsl/pandora_touch.c:8-10); DT /soc/pinctrl@f000000 reg=<0x0f000000 0x202000> is the pre-offset base. 0x0f0xxxxx hangs the bus. */
#define TLMM_STRIDE             0x1000UL        /* one config window per pin on this SoC -- NOT the 0x10 stride of older TLMM blocks */
#define GPIO_PIN_MAX            205             /* dev-sol, the highest pin in the table below */
#define TLMM_SIZE               (((GPIO_PIN_MAX) + 1) * TLMM_STRIDE)    /* 0x0ce000: derived from the pin table, not copied from the DT's pre-offset 0x202000 */
#define PCIE_PARF_PHYS          0x01c00000UL    /* /soc/pcie@1c00000 reg[0] "parf" */
#define PCIE_PARF_SIZE          0x00010000UL
#define PCIE_PARF_DUMP_BYTES    0x80u           /* step 2b: 32 words, 0x00..0x7c */

#define GPIO_WLAN_EN            16
#define GPIO_SW_CTRL            18
#define GPIO_HOST_SOL           204
#define GPIO_DEV_SOL            205

/*
 * TLMM GPIO registers: [1:0] pull (1 down, 3 up), [5:2] function, [8:6] drive = mA/2-1,
 * [9] output enable -- same decode as fuxi_touch.c and popsicle_syna.c. The per-pin
 * stride on this SoC is 0x1000 (pinctrl-canoe.ko: 0x100000 + 0x1000*n), so the config
 * word is the window base and the io word is base + 4. (The old "offset = 0x1000 +
 * pin*0x10" comment described the pre-offset f000000 layout and was wrong here.)
 */
#define TLMM_CFG(pin)           ((vm_offset_t)(pin) * TLMM_STRIDE)
#define TLMM_IO(pin)            ((vm_offset_t)(pin) * TLMM_STRIDE + 4)

/*
 * Matching write macros (the file previously had read-only accessors and no stores).
 * VM_WIMG_IO maps Device memory (nGnRnE), so a volatile store followed by a volatile
 * read is ordered on arm64 -- the read-back below needs no explicit barrier.
 */
#define TLMM_WR32(off, value)   (*(volatile uint32_t *)(tlmm + (off)) = (uint32_t)(value))
#define TLMM_CFG_WR(pin, v)     TLMM_WR32(TLMM_CFG(pin), (v))
#define TLMM_IO_WR(pin, v)      TLMM_WR32(TLMM_IO(pin), (v))

#define TLMM_FUNC_GPIO          0u
#define TLMM_PULL_NONE          0u
#define TLMM_CTL(pull, func, ma, oe) \
	((uint32_t)(pull) | ((uint32_t)(func) << 2) | ((uint32_t)((ma) / 2 - 1) << 6) | ((oe) ? (1u << 9) : 0u))
#define TLMM_IO_LOW             0u              /* io word: bit0 = level in, bit1 = level out */
#define TLMM_IO_HIGH            2u

/* step 2b must stay inside the mapped window: fail the build, not the boot. */
_Static_assert(PCIE_PARF_DUMP_BYTES <= PCIE_PARF_SIZE, "PARF dump exceeds PCIE_PARF_SIZE");

static vm_offset_t tlmm, pcie_parf;
static bool wifi_poweron;       /* boot-arg wifi_poweron=1; default off, gates step 0 */

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
 * Step 0: OPTIONAL power-on, only with wifi_poweron=1. Two pins, and nothing else:
 * mux wlan-en 16 and sw-ctrl 18 to GPIO function, no pull, 2 mA, output enabled, and
 * drive them high. Every store prints the pre-values, the value written, and the
 * read-back, so a boot log alone says whether the write took. The regulators
 * (VDDD_AON/VDDD_WLMX/VDDA_RFA..., S2J1/S1J1/S7F0/S8F0) are NOT touched: they are
 * RPMh-owned and a cold survey driving them would fight the boot loader's votes.
 *
 * One pin per call, no table: a `pins[i]` walk trips -Wunsafe-buffer-usage, and the
 * two pins differ in nothing a loop would save.
 */
static void
wifi_power_on_pin(unsigned pin, const char *what)
{
	uint32_t cfg_before = tlmm_read_cfg(pin);
	uint32_t io_before = tlmm_read_io(pin);
	uint32_t cfg_value = (cfg_before & ~0x3ffu) |
	    TLMM_CTL(TLMM_PULL_NONE, TLMM_FUNC_GPIO, 2, 1);
	uint32_t cfg_after, io_after;

	printf("popsicle_wifi: power-on %s (gpio%u) before cfg=0x%08x io=0x%08x\n",
	    what, pin, cfg_before, io_before);
	printf("popsicle_wifi: power-on %s (gpio%u) writing io=0x%x then cfg=0x%08x\n",
	    what, pin, (unsigned)TLMM_IO_HIGH, cfg_value);

	TLMM_IO_WR(pin, TLMM_IO_HIGH);   /* latch the level before OE turns on */
	TLMM_CFG_WR(pin, cfg_value);     /* mux = GPIO, no pull, 2 mA, output */

	cfg_after = tlmm_read_cfg(pin);
	io_after = tlmm_read_io(pin);
	printf("popsicle_wifi: power-on %s (gpio%u) after cfg=0x%08x io=0x%08x%s\n",
	    what, pin, cfg_after, io_after,
	    (cfg_after == cfg_value && (io_after & 2u) != 0) ? " OK" : " MISMATCH");
}

static void
wifi_step0_power_on(void)
{
	printf("popsicle_wifi: step0 power-on requested (wifi_poweron=1): "
	    "writing TLMM cfg/io for wlan-en 16 and sw-ctrl 18\n");

	wifi_power_on_pin(GPIO_WLAN_EN, "wlan-en");
	wifi_power_on_pin(GPIO_SW_CTRL, "sw-ctrl");

	/* Let the rail settle before step 2 starts reading PARF. */
	delay_for_interval(2000, NSEC_PER_USEC);
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
	wifi_survey_print("sw-ctrl level", tlmm_read_io(GPIO_SW_CTRL));
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

/*
 * Step 2b: raw PARF dump, 32 words across 0x00..0x7c. Deliberately uninterpreted: the
 * link-status / device-id word offsets are not established for this IP revision, so
 * only the offset and the value are printed. The whole range is inside the mapped
 * PCIE_PARF_SIZE window (asserted at build time above), and PARF is the root-complex
 * always-on block the existing step 2 already reads.
 */
static void
wifi_step2b_parf_dump(void)
{
	printf("popsicle_wifi: step2b PCIe PARF hex dump 0x00..0x7c "
	    "(32 x u32, raw -- link-status/device-id semantics NOT decoded)\n");

	for (unsigned off = 0; off < PCIE_PARF_DUMP_BYTES; off += 4) {
		uint32_t value = *(volatile uint32_t *)(pcie_parf + off);
		printf("popsicle_wifi: parf[0x%02x] = 0x%08x\n", off, value);
	}
}

/* ---- the thread ---------------------------------------------------------------------------------
 * popsicle_syna.c L1446: same polled-thread shape; the survey runs once and exits. */
static void
wifi_thread(__unused void *parameter, __unused wait_result_t result)
{
	printf("popsicle_wifi: FastConnect 7900 survey, boot-arg wifi_probe=1%s\n",
	    wifi_poweron ? " (wifi_poweron=1: step 0 will WRITE gpio16/gpio18)" : "");

	tlmm = ml_io_map_unmappable(TLMM_PHYS, TLMM_SIZE, VM_WIMG_IO);
	pcie_parf = ml_io_map_unmappable(PCIE_PARF_PHYS, PCIE_PARF_SIZE, VM_WIMG_IO);
	if (tlmm == 0 || pcie_parf == 0) {
		printf("popsicle_wifi: cannot map TLMM/PCIe PARF\n");
		if (tlmm != 0) {
/* PANDORA: ml_io_map_unmappable mappings are permanent; unmap panics (pandora_dpu.c r1b) */
		}
		if (pcie_parf != 0) {
/* PANDORA: ml_io_map_unmappable mappings are permanent; unmap panics (pandora_dpu.c r1b) */
		}
		thread_terminate(current_thread());
		return;
	}

	if (wifi_poweron) {
		wifi_step0_power_on();
	}
	wifi_step1_pins();
	wifi_step2_pcie();
	wifi_step2b_parf_dump();

	printf("popsicle_wifi: survey done (GPIO levels are the boot-loader state; reads were TLMM "
	       "config/level and PCIe PARF 0x00..0x7c -- %s)\n",
	    wifi_poweron ? "plus the announced gpio16/gpio18 power-on writes" : "no writes");
/* PANDORA: ml_io_map_unmappable mappings are permanent; unmap panics (pandora_dpu.c r1b) */
/* PANDORA: ml_io_map_unmappable mappings are permanent; unmap panics (pandora_dpu.c r1b) */
	thread_terminate(current_thread());
}

/* ---- the entry point ------------------------------------------------------------------------------
 * popsicle_syna.c L1502-1515: PE_parse_boot_argn("wifi_probe"), then the thread.
 * wifi_poweron is the second gate for the only write stage (step 0); absent or 0
 * means the driver stays read-only. */
void
popsicle_wifi_init(void)
{
	uint32_t enabled = 0;
	uint32_t poweron = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("wifi_probe", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	wifi_poweron = PE_parse_boot_argn("wifi_poweron", &poweron, sizeof(poweron)) && poweron;

	if (kernel_thread_start(wifi_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
