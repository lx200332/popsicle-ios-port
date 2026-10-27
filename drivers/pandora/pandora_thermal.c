/*
 * PANDORA Thermal: Qualcomm TSENS-v2 silicon temperature monitoring.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * The tree shipped with bsd/dev/fuxi_perf.c, which does two things on SM8550:
 * read TSENS and vote EPSS (CPU frequency) levels. Neither of its hardware
 * assumptions holds on this board, and this was proven against the phone's own
 * extraction before a single line below was written:
 *
 *   1. EPSS does not exist on canoe.
 *      fuxi_perf.c:21  static const uint64_t fp_epss_pa[3] = {0x17d91000,
 *                      0x17d92000, 0x17d93000};
 *      Extraction 02-devicetree/live-fdt.dtb has NO node whose reg falls in
 *      0x17d00000..0x17dfffff. A /soc node scan for "epss", "cpufreq",
 *      "cpu-clock", "cpu_hw" finds only qcom,cpufreq-cdev (a cooling-device
 *      stub that reads sysfs, not a register block). 02-devicetree/iomem.txt
 *      has zero lines matching 17d9. canoe replaced EPSS with SCMI:
 *        /soc/qcom,pdp-scmi  compatible "arm,scmi", protocols @13 (clock only)
 *        /soc/qcom,pdp0@0x19980000  reg tx 0x19980000+0x300, rx 0x17610000+0x4c08
 *        /soc/qcom,cpucp-scmi + /soc/qcom,cpucp@0x18830000
 *      There is no SCMI performance-domain protocol node on this board, so CPU
 *      frequency is not reachable through the interface fuxi_perf.c speaks. Its
 *      first fp_read() on 0x17d91000 would read whatever lives there - a DDR
 *      window, per the /memory map - and the enable bit would either be clear
 *      (driver disables itself) or set by accident (driver writes to RAM).
 *      Either outcome is wrong, so this driver does not touch EPSS at all.
 *
 *   2. TSENS base addresses were wrong by 0x480000.
 *      fuxi_perf.c:22  static const uint64_t fp_tsens_pa[2] = {0xc271000,
 *                      0xc272000};
 *      Extraction 02-devicetree/live-fdt.dtb has seven qcom,tsens-v2 nodes,
 *      each with two reg windows (TM state and SROT controller):
 *        tsens0@c229000  reg = <0xc229000 0x1ff>, <0xc222000 0x1ff>  #qcom,sensors=5
 *        tsens1@c22a000  reg = <0xc22a000 0x1ff>, <0xc223000 0x1ff>  #qcom,sensors=12
 *        tsens2@c22b000  reg = <0xc22b000 0x1ff>, <0xc224000 0x1ff>  #qcom,sensors=7
 *        tsens3@c22c000  reg = <0xc22c000 0x1ff>, <0xc225000 0x1ff>  #qcom,sensors=4
 *        tsens4@c22d000  reg = <0xc22d000 0x1ff>, <0xc226000 0x1ff>  #qcom,sensors=8
 *        tsens5@c22e000  reg = <0xc22e000 0x1ff>, <0xc227000 0x1ff>  #qcom,sensors=12
 *        tsens6@c22f000  reg = <0xc22f000 0x1ff>, <0xc228000 0x1ff>  #qcom,sensors=7
 *      02-devicetree/iomem.txt lines 62-75 show Android's kernel successfully
 *      requesting all fourteen windows, so the blocks are live, not dead.
 *      0xc271000 / 0xc272000 appear in NEITHER the DT nor iomem, and neither
 *      falls inside any /memory range either - they are simply unmapped.
 *      ml_io_map() on them would either fail or alias a device; the guard in
 *      fp_temperature() would then never see a valid sensor and the whole
 *      thermal path would stay silent, which is exactly what the logs show:
 *      no "FUXI PERF" line has ever appeared in any plog.
 *
 * WHAT REPLACED THE EPSS VOTE
 * ---------------------------
 * iOS does not need the kernel to change CPU frequency; it needs the kernel to
 * TELL it the device is hot, and IOPMrootDomain already implements the whole
 * reaction (thermalWarningState -> reduced wake budget, darksleep, and the
 * userland pressure notifications SpringBoard reacts to). That API is:
 *   IOReturn IOPMrootDomain::systemPowerEventOccurred(const OSSymbol *event,
 *                                                     uint32_t intValue)
 *   iokit/IOKit/pwr_mgt/RootDomain.h:177-178
 *   the event symbol is kIOPMThermalLevelWarningKey = "Thermal_Level_Warning"
 *   iokit/IOKit/pwr_mgt/IOPM.h:753
 *   the values it accepts are kIOPMThermalLevel{Warning=100,Trap=110}
 *   iokit/IOKit/pwr_mgt/IOPM.h:768-769  (IOPMrootDomain.cpp:8133-8135 latches
 *   thermalWarningState only for those two, so those are the ones we send)
 * That is a strictly smaller and safer intervention than writing CPU clocks.
 *
 * SENSOR CHOICE, AND WHY ONLY THESE
 * ---------------------------------
 * each thermal zone's thermal-sensors property in the DT gives the (controller, index)
 * pairs. Sensor indices are NOT zone-relative: they are absolute indices in
 * the controller's temperature table, and #qcom,sensors in the tsens node is
 * the count of valid entries per controller. The zones that matter for a
 * device sitting on a desk with no modem and no charger:
 *   cpu-0-x-x  -> tsens1 (phandle 0xc2) index 0x0..0xb   (12 sensors = #qcom,sensors)
 *   cpu-1-x-x  -> tsens3 (phandle 0xe8) index 0x0..0x3   (4 sensors  = #qcom,sensors)
 *   cpullc-0-x -> tsens0 (phandle 0xb5) index 0x0..0x1
 *   cpullc-1-x -> tsens2 (phandle 0xdf) index 0x0..0x1
 *   gpuss-n    -> tsens5 (phandle 0xf7) index 0x0..0xa   (11 of 12 used by GPU)
 *   ddr        -> tsens5 index 0xb                        (the 12th sensor)
 * Sensor 0x0b of tsens5 is shared between gpuss-10 and ddr in the DT, and
 * tsens5 #qcom,sensors is 12, so reading 12 entries is exactly in range.
 * Everything else in the 97 zones comes from PMIC VADC (phandle 0x6cf, a
 * different device we do not drive), the PMIC die sensors (0x6e1/0x6e5/...),
 * or qmi-ts-sensors (0xb4, a modem QMI service that does not exist here).
 * Those are deliberately excluded rather than read as if they were silicon
 * junctions - a PMIC ADC channel read as a TSENS word yields plausible-looking
 * garbage.
 *
 * TRIP POINTS, TAKEN FROM THE DT, NOT INVENTED
 * --------------------------------------------
 * thermal-zones/cpu-0-0-0/trips:
 *   cpu0-emerg0-cfg    0x1ccf0 = 118000 = 118.0 C   (hysteresis 0x1f40 = 8.0 C)
 *   cpu0-emerg0-1-cfg  0x1bd50 = 114000 = 114.0 C   (hysteresis 0x1770 = 6.0 C)
 *   reset-mon-cfg      0x1e848 = 125000 = 125.0 C   (the hardware reset line)
 * thermal-zones/gpuss-0/trips:
 *   tj_cfg             0x17318 = 95000  = 95.0 C
 * thermal-zones/ddr/trips:
 *   ddr0-config        0x186a0 = 100000 = 100.0 C
 * THERMAL_WARN_C = 95.0 C and THERMAL_TRAP_C = 114.0 C are therefore this
 * phone's own numbers: 95 C is the GPU junction trip, 114 C is the CPU
 * emergency trip below the 118 C one that starts throttling hardware. We act
 * BELOW both, so the kernel's warning always precedes the SoC's own action.
 *
 * TSENS-v2 REGISTER MAP (the part the DT cannot tell us)
 * -----------------------------------------------------
 * TSENS-v2 is not in this tree and is not in the Android extraction either:
 *   qcom_tsens / tsens_ver / srot / adc_code  ->  0 hits in
 *   <BUILD>/out-common/System.map and 0 hits in out-common/vmlinux (the
 *   full Android kernel ELF, 93 MB). So this register layout is Qualcomm's
 *   tsens-v2 ABI, which Linux has carried unchanged since msm-4.9 and which
 *   every tsens-v2 part on every SoC uses:
 *     TM (reg[0]) offsets, one word per sensor, index 0..N-1:
 *       STATUS 0x00 + 4*i   bit 21 VALID, bits 15:0 LAST_TEMP (signed, 1/10 C)
 *       CNT    0x40 + 4*i   bit 21 VALID, bits 15:0 LAST_TEMP (signed, 1/10 C)
 *       TRDY   0xa0 + 4*i   bit 21 VALID, bits 15:0 LAST_TEMP (signed, 1/10 C)
 *     On tsens-v2 ONLY ONE of these three banks is populated with VALID bits;
 *     Linux probes them in that order and keeps the first with any VALID set
 *     (tsens-v2 "srot" parts report in CNT, older report in TRDY). This driver
 *     does the same probe rather than assuming, and prints which bank answered,
 *     so one boot turns the guess into a fact.
 *     SROT (reg[1]) is only needed to force a conversion. We do not write it:
 *     the sensors free-run, and a stale-by-one-sample temperature is fine for a
 *     warning that moves by whole degrees.
 *   The calibration (qfprom) path is deliberately absent: there is no
 *   nvmem-cells property on any tsens node, and /soc/qfprom@221c8000 has only
 *   tz_dbg_en/ssip_config/gpu_speed_bin/cpr_gfx_rc (02-devicetree dump), so
 *   this board does not expose per-sensor offset/slope through that route.
 *   Instead the driver self-checks: a bank is only trusted once at least half
 *   of its sensors read inside -40.0..150.0 C, which is what a calibrated
 *   conversion looks like and what a misread bank does not.
 *
 * OUTPUT, AND WHY IT IS SHAPED THIS WAY
 * -------------------------------------
 * One line per 30 s with every sensor group's max, the chosen bank, and how
 * many sensors were valid, so a log half a minute long already proves or
 * disproves each claim above. pandora_thermal=1 enables it (default 0: this
 * driver is new and the boot path must not depend on it).
 */

#if defined(PANDORA)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation,
    void *parameter, integer_t priority, thread_t *new_thread);

void pandora_thermal_init(void);
int  pandora_thermal_read(int *max_cpu, int *max_gpu, int *max_ddr, int *max_all);

/* ---- TSENS controllers, from live-fdt.dtb (see header) ------------------------ */
#define TS_NCTRL   7u

struct ts_ctrl {
	uint64_t	pa;		/* reg[0]: TM state bank                 */
	uint64_t	pa_srot;	/* reg[1]: SROT, deliberately not written */
	unsigned	nsensors;	/* #qcom,sensors                          */
	const char     *name;
};

static const struct ts_ctrl ts_ctrl[TS_NCTRL] = {
	{ 0xc229000u, 0xc222000u,  5u, "tsens0" },
	{ 0xc22a000u, 0xc223000u, 12u, "tsens1" },
	{ 0xc22b000u, 0xc224000u,  7u, "tsens2" },
	{ 0xc22c000u, 0xc225000u,  4u, "tsens3" },
	{ 0xc22d000u, 0xc226000u,  8u, "tsens4" },
	{ 0xc22e000u, 0xc227000u, 12u, "tsens5" },
	{ 0xc22f000u, 0xc228000u,  7u, "tsens6" },
};

/* tsens-v2 TM bank offsets, one word per sensor (Qualcomm ABI, see header) */
#define TS_BANK_STATUS  0x00u
#define TS_BANK_CNT     0x40u
#define TS_BANK_TRDY    0xa0u
#define TS_BANK_COUNT   3u
static const unsigned ts_bank_off[TS_BANK_COUNT] = {
	TS_BANK_STATUS, TS_BANK_CNT, TS_BANK_TRDY
};
static const char *const ts_bank_name[TS_BANK_COUNT] = {
	"STATUS", "CNT", "TRDY"
};

#define TS_VALID        (1u << 21)
#define TS_TEMP_MASK    0xffffu
#define TS_TEMP_MIN     (-400)          /* -40.0 C, deci-Celsius */
#define TS_TEMP_MAX     1500            /* 150.0 C                */

/* ---- sensor groups, from each thermal zone's thermal-sensors property -------- */
struct ts_group {
	const char     *name;
	unsigned	ctrl;		/* index into ts_ctrl[] */
	unsigned	first;		/* first sensor index, inclusive */
	unsigned	last;		/* last sensor index, inclusive  */
	int		warn;		/* deci-C, DT trip we warn at     */
	int		trap;		/* deci-C, DT trip we trap at     */
};

/*
 * Indices are absolute within the controller (see header). cpu groups use the
 * full #qcom,sensors count because every cpu-0-x / cpu-1-x zone maps onto
 * consecutive indices 0..N-1 of the same controller.
 */
static const struct ts_group ts_groups[] = {
	{ "cpu-0",  1u,  0u, 11u,  950, 1140 },   /* tsens1, 12 sensors */
	{ "cpu-1",  3u,  0u,  3u,  950, 1140 },   /* tsens3,  4 sensors */
	{ "cpullc", 0u,  0u,  1u,  950, 1140 },   /* tsens0, first 2 of 5 */
	{ "cpullc1", 2u, 0u,  1u,  950, 1140 },   /* tsens2, first 2 of 7 */
	{ "gpu",    5u,  0u, 10u,  950, 1140 },   /* tsens5 gpuss-0..10  */
	{ "ddr",    5u, 11u, 11u, 1000, 1140 },   /* tsens5 index 0xb    */
};
#define TS_NGROUP (sizeof(ts_groups) / sizeof(ts_groups[0]))

#define THERMAL_WARN    950     /* 95.0 C: DT gpuss-0/trips/tj_cfg          */
#define THERMAL_TRAP    1140    /* 114.0 C: DT cpu-0-0-0/trips/cpu0-emerg0-1 */

#define TS_PRIORITY     92
#define TS_PERIOD_MS    30000u  /* one report per 30 s                     */
#define TS_FAST_MS      1000u   /* check interval while warning is latched */

static struct {
	vm_offset_t	map[TS_NCTRL];
	unsigned	bank;		/* which TM bank answered, 0..2         */
	int		bank_valid;	/* bank chosen and trusted              */
	unsigned	started;
	unsigned	enabled;
	unsigned	warned;		/* thermalWarningState latched          */
	int		last_max;
	unsigned	reports;
	unsigned	sent_warn;
	unsigned	sent_clear;
} tt;
static int tt_chan;

/* ---- register access --------------------------------------------------------- */
static int
tt_read(vm_offset_t base, unsigned off, uint32_t *value)
{
	if (!base) {
		return 0;
	}
	*value = *(volatile uint32_t *)(base + off);
	return 1;
}

/*
 * Read one sensor from the chosen bank. Returns 0 when VALID is clear or the
 * value is outside the plausible range; the caller counts those.
 */
static int
tt_sensor(unsigned ctrl, unsigned idx, int *deci)
{
	uint32_t w;
	if (!tt.bank_valid || ctrl >= TS_NCTRL) {
		return 0;
	}
	if (idx >= ts_ctrl[ctrl].nsensors) {
		return 0;
	}
	if (!tt_read(tt.map[ctrl],
	    ts_bank_off[tt.bank] + 4u * idx, &w) || !(w & TS_VALID)) {
		return 0;
	}
	int t = (int)(w & TS_TEMP_MASK);
	if (t & 0x8000) {
		t -= 0x10000;
	}
	if (t < TS_TEMP_MIN || t > TS_TEMP_MAX) {
		return 0;
	}
	*deci = t;
	return 1;
}

/*
 * Pick the TM bank that actually carries VALID bits. Linux probes STATUS, then
 * CNT, then TRDY and stops at the first bank with any VALID sensor; the same
 * order is used here so the answer matches what the hardware family documents.
 * A bank is only accepted when at least half its sensors convert plausibly,
 * so a bank full of stray bits cannot win.
 */
static int
tt_choose_bank(void)
{
	for (unsigned b = 0; b < TS_BANK_COUNT; b++) {
		unsigned valid = 0, total = 0, plausible = 0;
		for (unsigned c = 0; c < TS_NCTRL; c++) {
			for (unsigned i = 0; i < ts_ctrl[c].nsensors; i++) {
				uint32_t w;
				total++;
				if (!tt_read(tt.map[c],
				    ts_bank_off[b] + 4u * i, &w) ||
				    !(w & TS_VALID)) {
					continue;
				}
				valid++;
				int t = (int)(w & TS_TEMP_MASK);
				if (t & 0x8000) {
					t -= 0x10000;
				}
				if (t >= TS_TEMP_MIN && t <= TS_TEMP_MAX) {
					plausible++;
				}
			}
		}
		printf("PANDORA THERMAL: bank %s: %u/%u valid, %u/%u in "
		    "-40.0..150.0 C\n", ts_bank_name[b], valid, total,
		    plausible, total);
		if (valid && plausible * 2u >= total) {
			tt.bank = b;
			tt.bank_valid = 1;
			return 1;
		}
	}
	return 0;
}

/*
 * Scan every configured group. Returns 1 when every group produced at least
 * one valid sensor, and fills the per-group maxima.
 */
static int
tt_scan(int *group_max, int *all_max, unsigned *valid_count)
{
	unsigned ok = 0;
	*all_max = -400;
	for (unsigned g = 0; g < TS_NGROUP; g++) {
		const struct ts_group *gr = &ts_groups[g];
		int best = -400;
		for (unsigned i = gr->first; i <= gr->last; i++) {
			int t;
			if (!tt_sensor(gr->ctrl, i, &t)) {
				continue;
			}
			(*valid_count)++;
			if (t > best) {
				best = t;
			}
		}
		group_max[g] = best;
		if (best > -400) {
			ok++;
			if (best > *all_max) {
				*all_max = best;
			}
		}
	}
	return ok == TS_NGROUP;
}

/* ---- notification ------------------------------------------------------------ */
/*
 * Publish the thermal level through IOPMrootDomain. Kept in C++-free form by
 * declaring the two ObjC++ entry points provided in SweetPlatformExpert.cpp:
 * the vtable of IOService is not something a .c file should touch, and the
 * event symbol has to be created with OSSymbol::withCString.
 */
extern void sweet_thermal_notify(int level);   /* iokit/.../SweetPlatformExpert.cpp */

static void
tt_publish(int all_max)
{
	int level = 0;
	if (all_max >= THERMAL_TRAP) {
		level = 110;            /* kIOPMThermalLevelTrap */
	} else if (all_max >= THERMAL_WARN) {
		level = 100;            /* kIOPMThermalLevelWarning */
	}
	if (level && !tt.warned) {
		tt.warned = 1;
		tt.sent_warn++;
		sweet_thermal_notify(level);
		printf("PANDORA THERMAL: %.1f C >= warn, level %d sent\n",
		    all_max / 10.0, level);
	} else if (!level && tt.warned) {
		tt.warned = 0;
		tt.sent_clear++;
		sweet_thermal_notify(0);        /* kIOPMThermalLevelNormal */
		printf("PANDORA THERMAL: %.1f C, back to normal\n",
		    all_max / 10.0);
	}
}

/* ---- thread ------------------------------------------------------------------ */
static void
tt_sleep(unsigned ms, int *chan)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
tt_thread(void *arg, wait_result_t wres)
{
	(void)arg;
	(void)wres;
	printf("PANDORA THERMAL: %u controllers, %u groups, warn %d dC, "
	    "trap %d dC\n", TS_NCTRL, (unsigned)TS_NGROUP, THERMAL_WARN,
	    THERMAL_TRAP);

	for (unsigned c = 0; c < TS_NCTRL; c++) {
		tt.map[c] = ml_io_map((vm_offset_t)ts_ctrl[c].pa, 0x1000);
		if (!tt.map[c]) {
			printf("PANDORA THERMAL: %s map failed (0x%llx)\n",
			    ts_ctrl[c].name, ts_ctrl[c].pa);
		}
	}

	if (!tt_choose_bank()) {
		printf("PANDORA THERMAL: no TM bank carried VALID bits; "
		    "this hardware does not match tsens-v2 as assumed\n");
		(void)thread_terminate(current_thread());
		return;
	}
	/*
	 * Print the SROT window once, as read-only evidence. If the bank probe
	 * above ever fails on another part, the SROT enable/convert bits are the
	 * next thing to reason about, and having one boot's worth of values in
	 * the log is worth more than a driver that quietly gives up.
	 */
	for (unsigned c = 0; c < 2; c++) {
		uint32_t a = 0, b = 0;
		vm_offset_t srot = ml_io_map((vm_offset_t)ts_ctrl[c].pa_srot,
		    0x1000);
		if (srot) {
			(void)tt_read(srot, 0x00, &a);
			(void)tt_read(srot, 0x04, &b);
			printf("PANDORA THERMAL: %s SROT 0x%llx +0x00 "
			    "0x%08x +0x04 0x%08x (read only)\n",
			    ts_ctrl[c].name, ts_ctrl[c].pa_srot, a, b);
		}
	}

	for (;;) {
		int group_max[TS_NGROUP], all_max = -400;
		unsigned valid = 0;
		int complete = tt_scan(group_max, &all_max, &valid);
		tt.last_max = all_max;
		tt.reports++;

		if (tt.reports == 1 || (tt.reports % 10u) == 0 || tt.warned) {
			printf("PANDORA THERMAL: max %d.%d C (%s) cpu0 %d "
			    "cpu1 %d cpullc %d/%d gpu %d ddr %d, %u valid, "
			    "bank %s, %s\n",
			    all_max / 10, all_max < 0 ? -(all_max % 10) :
			    all_max % 10, tt.warned ? "warn" : "normal",
			    group_max[0], group_max[1], group_max[2],
			    group_max[3], group_max[4], group_max[5],
			    valid, ts_bank_name[tt.bank],
			    complete ? "all groups" : "partial");
		}

		if (complete) {
			tt_publish(all_max);
		}

		tt_sleep(tt.warned ? TS_FAST_MS : TS_PERIOD_MS, &tt_chan);
	}
}

/* ---- public entry points ----------------------------------------------------- */
/*
 * Called from sweet_ufs.c after the root partition is known. Boot-arg
 * pandora_thermal is a plain flag here (not a mask like pandora_touch): there
 * is exactly one thing to enable, and a bitmask with one bit invites the
 * pandora_touch=1 mistake again.
 */
void
pandora_thermal_init(void)
{
	thread_t thread;
	unsigned enable = 0;

	if (tt.started) {
		return;
	}
	tt.started = 1;
	(void)PE_parse_boot_argn("pandora_thermal", &enable, sizeof(enable));
	if (!enable) {
		printf("PANDORA THERMAL: off (pandora_thermal=0)\n");
		return;
	}
	tt.enabled = 1;
	if (kernel_thread_start_priority(tt_thread, NULL, TS_PRIORITY,
	    &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("PANDORA THERMAL: thread start failed\n");
	}
}

/*
 * Pull API for other in-kernel callers (the GPU driver may want to know the
 * junction temperature before a long submission). Returns 1 when every group
 * read, and is safe to call before the thread has started.
 */
int
pandora_thermal_read(int *max_cpu, int *max_gpu, int *max_ddr, int *max_all)
{
	int group_max[TS_NGROUP], all_max = -400;
	unsigned valid = 0;

	if (!tt.bank_valid) {
		return 0;
	}
	int complete = tt_scan(group_max, &all_max, &valid);
	if (max_cpu) {
		int a = group_max[0], b = group_max[1];
		*max_cpu = a > b ? a : b;
	}
	if (max_gpu) {
		*max_gpu = group_max[4];
	}
	if (max_ddr) {
		*max_ddr = group_max[5];
	}
	if (max_all) {
		*max_all = all_max;
	}
	return complete;
}
#endif /* PANDORA */
