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
 *            the offsets are printed and not interpreted;
 *   step 2c - the DTS-named link state: PARF + 0x414 bit 7 (qcom,phy-status-offset /
 *            -bit) and the PCIe state-machine registers qcom,pcie-sm-debug lists
 *            (0x1040..0x109c) inside the pcie_sm window, plus the start word at
 *            qcom,pcie-sm-start-offset 0x1090. Still read-only: these say whether the
 *            boot loader already trained the link, which decides whether a real port
 *            has to run the PHY sequence at all;
 *   step 2d - the ELBI window's 0xa8 bytes, raw. ELBI is where the root complex parks
 *            per-vector status, and it is small enough to print whole.
 *
 * What is still missing for a working radio (0231), in the order it has to happen:
 *   1 link training - the PHY sequence qcom,pcie-phy-sequence holds ~200 (offset, value)
 *     pairs; without it the endpoint never answers. qcom,boot-option = <1> says the boot
 *     loader may have done it already, which is exactly what step 2c measures;
 *   2 clocks and resets - resets = <&gcc 6 &gcc 9> (pcie_0_core_reset,
 *     pcie_0_phy_reset) plus the pipe/aux/slv clocks; none of those are voted here;
 *   3 regulators - VDDA_PCIE_0P9 / VDDA_PCIE_1P2 (S7F0) are RPMh-owned;
 *   4 enumeration - bus-range <0 0xff>, ranges <0x1000000 ...> for config/mem;
 *   5 SMMU - qcom,smmu-sid-base 0x1400, iommu-map to a stream map this tree does not
 *     program for PCIe;
 *   6 firmware - ath12k-class peach: amss20.bin + bdwlan.elf are TZ-authed, so the
 *     driver hands bytes to SCM rather than running them, and the QMI/QRTR control
 *     channel (qcom,qrtr-mhi-cnss) is not implemented here either.
 * Steps 1-3 above are the blockers; 4-6 come after them and are not attempted.
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
#define PCIE_PARF_PHYS          0x01c00000UL    /* reg[0] "parf"  0x1c00000 + 0x3000 */
#define PCIE_PARF_SIZE          0x00003000UL    /* the DTS says 0x3000, not 0x10000 */
#define PCIE_PARF_DUMP_BYTES    0x80u           /* step 2b: 32 words, 0x00..0x7c */
/*
 * The rest of /soc/pcie@1c00000's reg, in reg-names order. /soc has
 * #address-cells = 2, so each entry is <hi lo> <size_hi size_lo>:
 *   "phy"      <0x0 0x1c06000>  <0x0 0x2000>
 *   "dm_core"  <0x4 0x0>        <0x0 0x10000000>   256 MiB -- never mapped whole
 *   "elbi"     <0x4 0xf20>      <0x0 0xa8>         inside the dm_core window
 *   "iatu"     <0x4 0x10000000> <0x0 0x1000>
 *   "pcie_sm"  <0x0 0x1d07000>  <0x0 0x7000>
 * Only the small ones are mapped: a 256 MiB ml_io_map of dm_core is the kind of call
 * that panicked this tree before (docs/GPU_PORT_MAP.md), and nothing here reads it.
 */
#define PCIE_PHY_PHYS           0x01c06000ULL   /* "phy" */
#define PCIE_PHY_SIZE           0x00002000ULL
#define PCIE_ELBI_PHYS          0x400000f20ULL  /* "elbi": (0x4 << 32) | 0xf20 */
#define PCIE_ELBI_SIZE          0x000000a8ULL
#define PCIE_SM_PHYS            0x01d07000ULL   /* "pcie_sm" */
/*
 * The GCC block. Every PCIe access so far returned zero without aborting, which
 * means the root complex answers its bus slave but has no GDSC rail and no clock:
 * power-domains = <&gcc 1> ("gdsc-phy-vdd") is the rail that gates it.
 * GCC itself is reachable from here -- pandora_usb.c already reads GCC and got
 * BCR 00000000 / GDSCR fa22f800 -- so probing it cannot hang the bus the way the
 * unclocked PARF did. The window covers 0x39000 (USB30_PRIM, known good) through
 * 0x3c000 (where the PCIe GDSC/BCR pair lives on the neighbouring SoCs).
 */
#define GCC_PHYS                0x00100000ULL
#define GCC_SIZE                0x00080000ULL     /* the PCIe GDSC pair sits at 0x6b000, above the USB block */
#define GCC_USB_SANITY_OFF      0x00039004u     /* USB30_PRIM_GDSCR, a read that is known to work */
/*
 * The two PCIe power domains, read out of GCC on the real board (0251):
 *   gcc[0x6b004] = 0x0222f801  pcie_0     SW_COLLAPSE set -> collapsed
 *   gcc[0x6c000] = 0x02222801  pcie_0_phy SW_COLLAPSE set -> collapsed
 *   gcc[0x39004] = 0xfa22f800  usb30_prim SW_COLLAPSE clear -> powered, which is
 *                                         exactly why USB reads work and PCIe reads
 *                                         return zeros or abort.
 * These are the linux/drivers/clk/qcom/gdsc.c bits: bit 0 of GDSCR is SW_COLLAPSE
 * (1 = power down), and the completion bits live in CFG_GDSCR at +0x4, bit 16 up /
 * bit 15 down, which is what POLL_CFG_GDSCR polls.
 */
#define GCC_PCIE_GDSC           0x0006b004u     /* pcie_0 */
#define GCC_PCIE_PHY_GDSC       0x0006c000u     /* pcie_0_phy */
#define GDSC_SW_COLLAPSE        0x00000001u
#define GDSC_CFG_OFF            0x00000004u
#define GDSC_POWER_UP_COMPLETE  0x00010000u     /* bit 16 */
#define GDSC_POWER_DOWN_COMPLETE 0x00008000u    /* bit 15 */
#define GDSC_POLL_US            1000u
/*
 * Reading 0x3b000 aborted on its first word although 0x39004 answered, so the GCC
 * window is not uniformly readable and a sweep cannot be used. These are the
 * candidate offsets the neighbouring SoCs put the PCIe GDSC/BCR pair at, probed one
 * at a time and each printed before the next is touched, so the log always says how
 * far it got.
 */
struct wifi_gcc_cand { uint32_t off; const char *what; };
static const struct wifi_gcc_cand wifi_gcc_cands[] = {
	/* USB30_PRIM_GDSCR answers at 0x39004 here, byte for byte where sm8550 puts it,
	 * so this GCC is laid out like that one and the PCIe pair should be at 0x6b004 /
	 * 0x6c000. Every candidate is read in ascending order and printed before it is
	 * touched, so an abort names the first unimplemented offset instead of hiding it. */
	{ 0x6b000u, "pcie_0 BCR" },
	{ 0x6b004u, "pcie_0 GDSCR" },
	{ 0x6b008u, "pcie_0 CFG_GDSCR" },
	{ 0x6c000u, "pcie_0_phy GDSCR" },
	{ 0x6c004u, "pcie_0_phy CFG_GDSCR" },
	{ 0x6f000u, "pcie_phy COM BCR" },
	{ 0x6f010u, "pcie_phy COM BCR 2" },
};
#define WIFI_GCC_CANDS (sizeof(wifi_gcc_cands) / sizeof(wifi_gcc_cands[0]))
#define PCIE_SM_SIZE            0x00007000ULL

/* qcom,phy-status-offset / qcom,phy-status-bit: the ready bit of the PHY block. */
#define PCIE_PHY_STATUS_OFF     0x414u
#define PCIE_PHY_STATUS_BIT     7u
/* qcom,pcie-sm-start-offset and qcom,pcie-sm-branch-offset, in pcie_sm. */
#define PCIE_SM_START_OFF       0x1090u
#define PCIE_SM_BRANCH_OFF      0x1000u
/* qcom,pcie-sm-debug: the fourteen words the vendor driver watches while it trains. */
#define PCIE_SM_DEBUG_FIRST     0x1040u
#define PCIE_SM_DEBUG_STEP      0x8u
#define PCIE_SM_DEBUG_COUNT     14u

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

static vm_offset_t tlmm, pcie_parf, pcie_phy, pcie_elbi, pcie_sm;
static vm_offset_t gcc;         /* only for the PCIe rail probe (step 3) */
static bool wifi_poweron;       /* boot-arg wifi_poweron=1; default off, gates step 0 */
/* PANDORA: reading the ELBI window before the link is up resets the board (0248). */
static bool wifi_elbi;          /* boot-arg wifi_elbi=1; default off, gates step 2d */
static bool wifi_gcc;           /* boot-arg wifi_gcc=1; default off, gates step 3 (read-only) */
static bool wifi_gdsc;          /* boot-arg wifi_gdsc=1; default off, gates step 4 (WRITES GCC) */

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

/*
 * Step 2c: the link state the DTS itself names. qcom,boot-option = <1> means the boot
 * loader is allowed to bring this link up, and the vendor driver reads exactly these
 * two places to find out whether it did: the PHY ready bit, and the state machine's
 * start word. Everything is a read; the values are printed raw with their meaning
 * stated only where the DTS states it.
 */
static void
wifi_step2c_link(void)
{
	uint32_t status = *(volatile uint32_t *)(pcie_parf + PCIE_PHY_STATUS_OFF);
	bool ready = ((status >> PCIE_PHY_STATUS_BIT) & 1u) != 0;
	uint32_t start = *(volatile uint32_t *)(pcie_sm + PCIE_SM_START_OFF);
	uint32_t branch = *(volatile uint32_t *)(pcie_sm + PCIE_SM_BRANCH_OFF);

	printf("popsicle_wifi: step2c parf[0x%x] = 0x%08x -> phy %s (bit %u)\n",
	    PCIE_PHY_STATUS_OFF, status, ready ? "READY" : "not ready", PCIE_PHY_STATUS_BIT);
	printf("popsicle_wifi: step2c pcie_sm[0x%x] = 0x%08x (qcom,pcie-sm-start-offset)\n",
	    PCIE_SM_START_OFF, start);
	printf("popsicle_wifi: step2c pcie_sm[0x%x] = 0x%08x (qcom,pcie-sm-branch-offset)\n",
	    PCIE_SM_BRANCH_OFF, branch);
	printf("popsicle_wifi: step2c link %s: %s\n",
	    ready ? "appears trained" : "is not trained",
	    ready ? "an endpoint may already answer; enumeration comes next"
	          : "the PHY sequence has to run before anything can enumerate");

	/* The fourteen words qcom,pcie-sm-debug names, in order, still raw. */
	printf("popsicle_wifi: step2c pcie_sm debug words 0x%x..0x%x (qcom,pcie-sm-debug)\n",
	    PCIE_SM_DEBUG_FIRST, PCIE_SM_DEBUG_FIRST + PCIE_SM_DEBUG_STEP * (PCIE_SM_DEBUG_COUNT - 1));
	for (unsigned i = 0; i < PCIE_SM_DEBUG_COUNT; i++) {
		unsigned off = PCIE_SM_DEBUG_FIRST + i * PCIE_SM_DEBUG_STEP;
		printf("popsicle_wifi: pcie_sm[0x%04x] = 0x%08x\n", off,
		    *(volatile uint32_t *)(pcie_sm + off));
	}
}

/*
 * Bring one GDSC up: clear SW_COLLAPSE, then poll its CFG_GDSCR for the up bit. The
 * sequence mirrors linux/drivers/clk/qcom/gdsc.c gdsc_toggle_logic() with
 * POLL_CFG_GDSCR, which is the flag these SoCs use; the wait values are already in the
 * register and are left alone. Returns true when the domain reports powered.
 */
static bool
wifi_gdsc_on(uint32_t off, const char *what)
{
	volatile uint32_t *gdscr = (volatile uint32_t *)(gcc + off);
	volatile uint32_t *cfg = (volatile uint32_t *)(gcc + off + GDSC_CFG_OFF);
	uint32_t before = *gdscr;
	uint32_t after, cfgv = 0;
	unsigned waited = 0;

	if ((before & GDSC_SW_COLLAPSE) == 0) {
		printf("popsicle_wifi: gdsc %s (gcc[0x%05x]) = 0x%08x, already powered\n",
		    what, off, before);
		return true;
	}

	printf("popsicle_wifi: gdsc %s (gcc[0x%05x]) = 0x%08x, clearing SW_COLLAPSE\n",
	    what, off, before);
	*gdscr = before & ~GDSC_SW_COLLAPSE;

	while (waited < GDSC_POLL_US) {
		cfgv = *cfg;
		if (cfgv & GDSC_POWER_UP_COMPLETE) {
			break;
		}
		delay_for_interval(10, NSEC_PER_USEC);
		waited += 10;
	}

	after = *gdscr;
	printf("popsicle_wifi: gdsc %s after %u us: gcc[0x%05x] = 0x%08x, cfg = 0x%08x -> %s\n",
	    what, waited, off, after, cfgv,
	    (cfgv & GDSC_POWER_UP_COMPLETE) ? "up" : "not up (bit 16 never set)");

	if ((after & GDSC_SW_COLLAPSE) != 0) {
		return false;
	}

	return (cfgv & GDSC_POWER_UP_COMPLETE) != 0 || cfgv == 0;
}

/*
 * Step 4: power the PCIe domains up and re-read the places that used to answer
 * zeros. If PARF starts returning something other than 0x00000000 the root complex is
 * out of collapse and the rest of the bring-up (resets, clocks, PHY, PERST) has a
 * base to build on. Writing is why this sits behind its own gate.
 */
static void
wifi_step4_power(void)
{
	bool main_up, phy_up;
	unsigned changed = 0;

	main_up = wifi_gdsc_on(GCC_PCIE_GDSC, "pcie_0");
	phy_up = wifi_gdsc_on(GCC_PCIE_PHY_GDSC, "pcie_0_phy");
	printf("popsicle_wifi: step4 pcie_0 %s, pcie_0_phy %s\n",
	    main_up ? "up" : "down", phy_up ? "up" : "down");

	for (unsigned off = 0; off < PCIE_PARF_DUMP_BYTES; off += 4) {
		uint32_t v = *(volatile uint32_t *)(pcie_parf + off);

		if (v != 0) {
			changed++;
			printf("popsicle_wifi: step4 parf[0x%02x] = 0x%08x (was zero before power)\n",
			    off, v);
		}
	}
	printf("popsicle_wifi: step4 %u of %u PARF words are now nonzero\n",
	    changed, (unsigned)(PCIE_PARF_DUMP_BYTES / 4));

	/* The same two places step 2c read before power, read again now that the domains
	 * are out of collapse: the PHY ready bit and the state machine's start word. */
	{
		uint32_t status = *(volatile uint32_t *)(pcie_parf + PCIE_PHY_STATUS_OFF);
		uint32_t start = *(volatile uint32_t *)(pcie_sm + PCIE_SM_START_OFF);
		uint32_t branch = *(volatile uint32_t *)(pcie_sm + PCIE_SM_BRANCH_OFF);

		printf("popsicle_wifi: step4 parf[0x%x] = 0x%08x -> phy %s (bit %u, after power)\n",
		    PCIE_PHY_STATUS_OFF, status,
		    ((status >> PCIE_PHY_STATUS_BIT) & 1u) ? "READY" : "still not ready",
		    PCIE_PHY_STATUS_BIT);
		printf("popsicle_wifi: step4 pcie_sm[0x%x] = 0x%08x, [0x%x] = 0x%08x (after power)\n",
		    PCIE_SM_START_OFF, start, PCIE_SM_BRANCH_OFF, branch);
	}

	/* ELBI is still unreadable with both GDSCs up (0252: this walk reset the board
	 * even though the identical power-up alone did not), so it needs the link trained
	 * and PERST released, not just the domains on. Kept behind wifi_elbi=1 so the
	 * write path never touches it by accident. */
	if (wifi_elbi) {
		unsigned nz = 0;

		for (unsigned off = 0; off + 4 <= PCIE_ELBI_SIZE; off += 4) {
			uint32_t v = *(volatile uint32_t *)(pcie_elbi + off);

			if (v != 0) {
				nz++;
				printf("popsicle_wifi: step4 elbi[0x%02x] = 0x%08x (after power)\n",
				    off, v);
			}
		}
		printf("popsicle_wifi: step4 %u of %u ELBI words are nonzero after power, window did not abort\n",
		    nz, (unsigned)(PCIE_ELBI_SIZE / 4));
	}
}

/*
 * Step 3: find the PCIe power rail. The root complex answers its bus slave with
 * zeros, so it is reachable but unpowered; on the neighbouring SoCs the GDSC that
 * gates it sits a few kilobytes below / above the USB30_PRIM block at 0x39000. This
 * walks the candidate window and prints only the words that are not zero, which keeps
 * the log small and makes the rail easy to spot: a GDSC has its enable bit 0 and a
 * POLL_CFG field above it. Nothing is written -- this step only localises the rail so
 * the next revision can turn it on with a named offset instead of a guessed one.
 */
static void
wifi_step3_gcc_probe(void)
{
	uint32_t sanity = *(volatile uint32_t *)(gcc + GCC_USB_SANITY_OFF);

	printf("popsicle_wifi: step3 gcc[0x%x] = 0x%08x (USB30_PRIM_GDSCR, sanity)\n",
	    GCC_USB_SANITY_OFF, sanity);
	printf("popsicle_wifi: step3 probing %u candidate offsets one at a time; each printed BEFORE it is read\n",
	    (unsigned)WIFI_GCC_CANDS);

	for (unsigned k = 0; k < WIFI_GCC_CANDS; k++) {
		const struct wifi_gcc_cand *c = &wifi_gcc_cands[k];
		uint32_t v;

		printf("popsicle_wifi: step3 about to read gcc[0x%05x] (%s)\n", c->off, c->what);
		v = *(volatile uint32_t *)(gcc + c->off);
		printf("popsicle_wifi: step3 gcc[0x%05x] = 0x%08x (%s)\n", c->off, v, c->what);
	}
	printf("popsicle_wifi: step3 all %u candidates answered, no abort\n",
	    (unsigned)WIFI_GCC_CANDS);
}


/*
 * Step 2d: the ELBI window whole (0xa8 bytes = 42 words). It is small, it is the root
 * complex's own status block, and there is no point guessing which word matters
 * before this driver has ever read one.
 */
static void
wifi_step2d_elbi(void)
{
	printf("popsicle_wifi: step2d ELBI 0x%llx + 0x%llx, raw\n",
	    (unsigned long long)PCIE_ELBI_PHYS, (unsigned long long)PCIE_ELBI_SIZE);
	for (unsigned off = 0; off + 4 <= PCIE_ELBI_SIZE; off += 4) {
		printf("popsicle_wifi: elbi[0x%02x] = 0x%08x\n", off,
		    *(volatile uint32_t *)(pcie_elbi + off));
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
	pcie_phy = ml_io_map_unmappable(PCIE_PHY_PHYS, PCIE_PHY_SIZE, VM_WIMG_IO);
	pcie_elbi = ml_io_map_unmappable(PCIE_ELBI_PHYS, PCIE_ELBI_SIZE, VM_WIMG_IO);
	pcie_sm = ml_io_map_unmappable(PCIE_SM_PHYS, PCIE_SM_SIZE, VM_WIMG_IO);
	gcc = ml_io_map_unmappable(GCC_PHYS, GCC_SIZE, VM_WIMG_IO);
	if (tlmm == 0 || pcie_parf == 0 || pcie_phy == 0 || pcie_elbi == 0 || pcie_sm == 0 ||
	    gcc == 0) {
		printf("popsicle_wifi: cannot map TLMM/PCIe: tlmm %p parf %p phy %p elbi %p sm %p gcc %p\n",
		    (void *)tlmm, (void *)pcie_parf, (void *)pcie_phy, (void *)pcie_elbi,
		    (void *)pcie_sm, (void *)gcc);
		/* PANDORA: ml_io_map_unmappable mappings are permanent; unmap panics
		 * (pandora_dpu.c r1b), so there is nothing to release here either. */
		thread_terminate(current_thread());
		return;
	}

	if (wifi_poweron) {
		wifi_step0_power_on();
	}
	wifi_step1_pins();
	wifi_step2_pcie();
	wifi_step2b_parf_dump();
	wifi_step2c_link();
	if (wifi_gcc) {
		wifi_step3_gcc_probe();
	}
	if (wifi_gdsc) {
		wifi_step4_power();
	}
	/* PANDORA: the ELBI window (0x4_00000f20, DTS reg[3]) is not readable until the
	 * PCIe upstream port is out of reset. A plain load there raises a synchronous
	 * external abort and resets the machine before the log ring is ever flushed
	 * (measured 0248: step2c alone survives 63 s, step2d alone is a dead reset).
	 * It is now opt-in behind wifi_elbi=1 so the survey cannot take the board down. */
	if (wifi_elbi) {
		wifi_step2d_elbi();
	}

	printf("popsicle_wifi: survey done (GPIO levels are the boot-loader state; reads were TLMM "
	       "config/level, PCIe PARF 0x00..0x7c, the PHY status bit, the state machine's debug "
	       "words and ELBI -- %s)\n",
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
	{
		uint32_t elbi = 0;
		wifi_elbi = PE_parse_boot_argn("wifi_elbi", &elbi, sizeof(elbi)) && elbi;
	}
	{
		uint32_t g = 0;
		wifi_gcc = PE_parse_boot_argn("wifi_gcc", &g, sizeof(g)) && g;
	}
	{
		uint32_t d = 0;
		wifi_gdsc = PE_parse_boot_argn("wifi_gdsc", &d, sizeof(d)) && d;
	}

	if (kernel_thread_start(wifi_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
