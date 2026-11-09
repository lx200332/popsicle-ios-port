/*
 * pandora_idle.c -- the CPU idle side, BSD half.
 *
 * What this file is: a census of the idle states the device tree describes for
 * this SoC, printed once, plus one explicit statement that XNU's arm64 idle
 * loop does not consume them. It registers nothing and changes no behaviour.
 *
 * 本文件仅登记意图：arm64 cpuidle 未接入 idle loop。
 *
 * Why the census lives here and not in osfmk: osfmk/arm/pandora_cpu_idle.c
 * (patch 0215) already owns the arm64 side. It exports pandora_idle_start(),
 * pandora_idle_wfi() and pandora_idle_log(), probes PSCI over SMC and replaces
 * the WFI in cpu_idle with a core-retention entry when boot-arg pandora_idle=1.
 * That file is upstream and outside this module's write scope, so nothing here
 * touches osfmk. This file deliberately defines no symbol that file defines,
 * and references no pandora_* symbol at all -- a strong reference to a missing
 * one would fail the link and a weak one is not accepted by this kernel link
 * (see the note at bsd/dev/pandora_thermal.c:371).
 *
 * The states below are copied from dts/canoe_base.dts, node idle-states, not
 * read from the device tree at run time: the DT is already unflattened and
 * queryable through SecureDT, but this census has to agree with what osfmk's
 * patch 0215 hard-codes anyway (560 us / 686 us at pandora_cpu_idle.c:118-119),
 * and a printed constant is easier to diff against the DTS than a pointer walk
 * whose failure modes would need their own error path.
 *
 *   canoe_base.dts:261  idle-states {
 *   canoe_base.dts:262      entry-method = "psci";      -- PSCI CPU_SUSPEND
 *   canoe_base.dts:263      medium-cluster0-c4         ret,       560 us, param 0x4
 *   canoe_base.dts:272      large-cluster1-c4          ret,       686 us, param 0x4
 *   canoe_base.dts:281      medium-cluster-cl5         ret-pll-off, 9144 us, param 0x1000054
 *   canoe_base.dts:290      large-cluster-cl5          ret-pll-off, 9144 us, param 0x1000054
 *   canoe_base.dts:299      cluster-ss3                apps-pc,  10150 us, param 0x200c354
 *   canoe_base.dts:104      cpu@0..    cpu-idle-states = <0x3>  -- medium-cluster0-c4
 *   canoe_base.dts:221      cpu@10000  cpu-idle-states = <0xc>  -- large-cluster1-c4
 *
 * Only the two c4 ("ret", i.e. WFI plus core power-down with retention) states
 * are reachable today, because those are the only ones a CPU node names. The
 * cl5 and ss3 states have no CPU node pointing at them, so they are a cluster
 * and SoC level intent with no consumer -- listed for the record, not entered.
 *
 * Gated on boot-arg pandora_idle_census (default 1). The name is distinct from
 * pandora_idle, which osfmk already owns for its core-retention switch.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <pexpert/pexpert.h>

#if defined(PANDORA)

/*
 * One idle state as dts/canoe_base.dts describes it. Separate named objects
 * rather than one array on purpose: an array subscript is what clang's
 * -Wunsafe-buffer-usage fires on, and this file is held to zero warnings.
 */
struct pidle_dt_state {
	const char *name;
	const char *entry;      /* arm,idle-state-name */
	uint32_t    residency_us;
	uint64_t    suspend_param;
	int         reachable;  /* some CPU node's cpu-idle-states names it */
};

/* canoe_base.dts:263, phandle 0x3, named by cpu@0..cpu@500 (:104) */
static const struct pidle_dt_state pidle_medium_c4 = {
	"medium-cluster0-c4", "ret", 560, 0x4ull, 1
};
/* canoe_base.dts:272, phandle 0xc, named by cpu@10000/cpu@10100 (:221) */
static const struct pidle_dt_state pidle_large_c4 = {
	"large-cluster1-c4", "ret", 686, 0x4ull, 1
};
/* canoe_base.dts:281, no CPU node names it */
static const struct pidle_dt_state pidle_medium_cl5 = {
	"medium-cluster-cl5", "ret-pll-off", 9144, 0x1000054ull, 0
};
/* canoe_base.dts:290, no CPU node names it */
static const struct pidle_dt_state pidle_large_cl5 = {
	"large-cluster-cl5", "ret-pll-off", 9144, 0x1000054ull, 0
};
/* canoe_base.dts:299, no CPU node names it */
static const struct pidle_dt_state pidle_ss3 = {
	"cluster-ss3", "apps-pc", 10150, 0x200c354ull, 0
};

static void
pidle_row(const struct pidle_dt_state *s)
{
	printf("PANDORA IDLE:   %-19s %-12s min-residency %5u us, suspend param 0x%llx%s\n",
	    s->name, s->entry, s->residency_us, (unsigned long long)s->suspend_param,
	    s->reachable ? "" : " (no CPU node names it)");
}

/*
 * Print the census once. Safe to call more than once and from any thread: the
 * body is five printf calls, and the "done" flag keeps a second call quiet.
 */
void pandora_idle_init(void);

void
pandora_idle_init(void)
{
	static int done;
	int census = 1;

	if (__atomic_load_n(&done, __ATOMIC_RELAXED)) {
		return;
	}
	if (!PE_parse_boot_argn("pandora_idle_census", &census, sizeof(census)) || census <= 0) {
		return;
	}
	__atomic_store_n(&done, 1, __ATOMIC_RELAXED);

	printf("PANDORA IDLE: arm64 cpuidle is not wired into the idle loop; this file only records the intent\n");
	printf("PANDORA IDLE: DT idle-states 5 (entry-method psci, PSCI CPU_SUSPEND), 2 named by a CPU node\n");
	pidle_row(&pidle_medium_c4);
	pidle_row(&pidle_large_c4);
	pidle_row(&pidle_medium_cl5);
	pidle_row(&pidle_large_cl5);
	pidle_row(&pidle_ss3);
	printf("PANDORA IDLE: osfmk/arm/pandora_cpu_idle.c owns the entry itself (boot-arg pandora_idle, 560/686 us)\n");
}

#endif /* PANDORA */
