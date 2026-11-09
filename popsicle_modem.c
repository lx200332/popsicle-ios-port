/*
 * popsicle_modem.c -- Xiaomi 17 Pro Max (popsicle, SM8850 "canoe") modem survey.
 *
 * READ-ONLY survey, gated by boot-arg modem_probe=1. The modem stack is
 * MPSS.DE.9.0-1a29cb0006 (peach_v2 firmware), X85-class; its firmware is signed
 * with QCOM PIL-TZ (modem.mdt/modem_dtb.mdt via remoteproc-mss@04080000, and
 * ipa_fws via qcom,pil-tz-generic) and there is no hackios modem framework in this
 * tree, so the REAL driver is blocked -- see "blocked on" below. This file prints
 * the board state a port would need first: the IPA hardware version and the modem
 * power-domain voting state XBL/ABL left behind. No bus transactions, no writes.
 *
 * Hardware facts (biocam-modem-facts.md):
 *   - IPA: qcom,ipa@3e00000, reg 0x3e00000 (84000) + 0x3e40000 (0xf800) + ...,
 *     ipa-hw-ver=0x18, ee=0, ipa-irq GIC 655 (0x28f), gsi-irq GIC 432 (0x1b0),
 *     smmu ap/wlan/uc/11ad; firmware ipa_fws pas-id 0xf.
 *   - Modem PAS: remoteproc-mss@04080000, qcom,canoe-modem-pas, wdog GIC 264,
 *     glink mpss/QRTR/DS, SMP2P modem;
 *   - reserved-memory: mpss_region, ipa_fw_region@99580000, ipa_gsi_region@99590000.
 *
 * Survey order -- step 1 is deliberately the safe one, because it is also the
 * link-up evidence step 2 needs:
 *   step 1 - the GCC modem/ipa branch CBCR pair (0x52008/0x5200c): which modem and
 *            ipa clocks XBL/ABL left voted. GCC sits in the always-on domain and
 *            pandora_bt.c bt_clock_dump()/bt_clock_scan() already read this exact
 *            window on this board, so it answers in every power state.
 *   step 2 - the IPA identity words (0x3e00000+0x0/0x4 and the 0x3e40000 block).
 *            These do NOT answer in every power state: a boot with just
 *            modem_probe=1 took a synchronous external abort (ESR 0x96000010) on
 *            the first word, pc in modem_thread, because the IPA was neither
 *            powered nor link-up and an unclocked register read faults the bus.
 *            Every one of these reads therefore goes through modem_ipa_rd(), which
 *            refuses unless boot-arg modem_ipa_read allows it AND step 1 says the
 *            branch is clocked. Each call prints exactly one line: either
 *            "skipped (ipa not up: ...)" or "= 0x... (read)".
 *            The IPA window is not even mapped when the gate cannot open, so a
 *            zero gate cannot reach a NULL/short mapping either.
 *
 * Boot args:
 *   modem_probe=1      start the survey thread (absent/0: nothing runs)
 *   modem_ipa_read=0   skip every IPA read                                    (default)
 *   modem_ipa_read=1   read the IPA, but only while the GCC branch clock is up
 *   modem_ipa_read=2   read the IPA regardless of the clock evidence; this is the
 *                      only setting that can still panic, and it is opt-in
 *   modem_deep=1       retired name, still accepted as modem_ipa_read=1
 *
 * Blocked on: QCOM PIL-TZ firmware signature chain (modem.mdt + ipa_fws) -- the
 * firmware cannot be loaded without the TZ-authenticated PAS handoff, and no
 * hackios modem framework source exists in this tree to model it against.
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

void popsicle_modem_init(void);

/* ------------------------------------------------------------------ board facts */

#define IPA_PHYS                0x03e00000UL    /* qcom,ipa@3e00000 reg[0] */
#define IPA_SIZE                0x00090000UL    /* 84000 + 0xf800 rounded up */
#define GCC_PHYS                0x00100000UL    /* /soc/clock-controller@100000 */
#define GCC_SIZE                0x001f4200UL    /* DT /soc/clock-controller@100000 reg size (fdtget 0x1f4200); window must cover 0x52018 / 0xa90a8 actually read */

/* GCC modem/ipa branch CBCR pair; pandora_bt.c bt_clock_dump reads the same words. */
#define GCC_MD_CBCR             0x52008UL
#define GCC_MD_CBCR_ALT         0x5200cUL

/* Interval where the modem and ipa GCC branches live (pandora_bt.c bt_clock_scan). */
#define GCC_CBCR_OFF_BIT        0x80000000u     /* bit31 set means that branch is OFF */

/* modem_ipa_read values. */
#define IPA_GATE_OFF            0
#define IPA_GATE_CLOCK          1
#define IPA_GATE_FORCE          2

static vm_offset_t ipa, gcc;
static int ipa_gate = IPA_GATE_OFF;     /* boot-arg modem_ipa_read */
static bool ipa_linked;                 /* step 1 says the modem/ipa branch is up */

static void
modem_survey_print(const char *what, uint32_t value)
{
	printf("popsicle_modem: %s = 0x%x\n", what, value);
}

/*
 * Step 1: GCC state left by XBL/ABL. Safe in every power state, and the answer is
 * the link-up evidence step 2 is gated on. bt_clock_scan prints "off" for a CBCR
 * word with bit31 set and "ON" otherwise; the same rule decides ipa_linked.
 */
static void
modem_step_gcc(void)
{
	uint32_t cbcr = *(volatile uint32_t *)(gcc + GCC_MD_CBCR);
	uint32_t alt = *(volatile uint32_t *)(gcc + GCC_MD_CBCR_ALT);

	modem_survey_print("gcc ipa/modem cbcr probe (0x52008)", cbcr);
	modem_survey_print("gcc ipa/modem cbcr probe (0x5200c)", alt);
	ipa_linked = cbcr != 0xffffffffu && alt != 0xffffffffu &&
	    !(cbcr & GCC_CBCR_OFF_BIT) && !(alt & GCC_CBCR_OFF_BIT);
	printf("popsicle_modem: gcc modem/ipa branch clock %s -> IPA window %s\n",
	    ipa_linked ? "ON" : "off",
	    ipa_linked ? "plausibly up" : "not up (no link-up evidence)");
}

/*
 * Step 2 gate. One IPA read, or one refusal -- never both, and never a read the
 * gate did not clear. ipa is non-zero whenever this can reach the dereference.
 */
static void
modem_ipa_rd(const char *what, vm_offset_t offset)
{
	if (ipa_gate == IPA_GATE_OFF) {
		printf("popsicle_modem: %s: skipped (ipa not up: modem_ipa_read=0)\n", what);
		return;
	}
	if (ipa_gate == IPA_GATE_CLOCK && !ipa_linked) {
		printf("popsicle_modem: %s: skipped (ipa not up: gcc branch clock off)\n", what);
		return;
	}
	printf("popsicle_modem: %s = 0x%x (read, modem_ipa_read=%d)\n", what,
	    *(volatile uint32_t *)(ipa + offset), ipa_gate);
}

/* Step 2: IPA identity reads, every one behind modem_ipa_rd(). */
static void
modem_step_ipa(void)
{
	modem_ipa_rd("ipa hw version word0", 0x0);
	modem_ipa_rd("ipa hw version word1", 0x4);
	/* the second reg block sits at 0x40000 into the IPA window */
	modem_ipa_rd("ipa block2 word0", 0x40000);
	modem_ipa_rd("ipa block2 word1", 0x40004);
}

/* ---- the thread ---------------------------------------------------------------------------------
 * popsicle_wifi.c shape: run once, print, unmap, exit. Every mapping failure goes
 * to out the way pandora_bt.c:454 does, so the thread always unmaps what it took
 * and always terminates. */
static void
modem_thread(__unused void *parameter, __unused wait_result_t result)
{
	int deep = 0;

	printf("popsicle_modem: IPA/modem survey, boot-arg modem_probe=1\n");

	(void)PE_parse_boot_argn("modem_ipa_read", &ipa_gate, sizeof(ipa_gate));
	(void)PE_parse_boot_argn("modem_deep", &deep, sizeof(deep));
	if (deep && ipa_gate == IPA_GATE_OFF) {
		ipa_gate = IPA_GATE_CLOCK;
		printf("popsicle_modem: modem_deep=1 is the retired name, using modem_ipa_read=1\n");
	}
	if (ipa_gate < IPA_GATE_OFF || ipa_gate > IPA_GATE_FORCE) {
		printf("popsicle_modem: modem_ipa_read=%d is out of range, using 0\n", ipa_gate);
		ipa_gate = IPA_GATE_OFF;
	}
	printf("popsicle_modem: modem_ipa_read=%d (0 skips every IPA read, 1 needs the GCC "
	    "branch clock, 2 forces)\n", ipa_gate);

	/* GCC first: always clocked, and it carries the link-up evidence. */
	gcc = ml_io_map_unmappable(GCC_PHYS, GCC_SIZE, VM_WIMG_IO);
	if (!gcc) {
		printf("popsicle_modem: cannot map the GCC window\n");
		goto out;
	}
	modem_step_gcc();

	if (ipa_gate == IPA_GATE_OFF) {
		printf("popsicle_modem: IPA window left unmapped (modem_ipa_read=0)\n");
		goto out;
	}
	if (ipa_gate == IPA_GATE_CLOCK && !ipa_linked) {
		printf("popsicle_modem: IPA window left unmapped: no link-up evidence "
		    "(modem_ipa_read=2 reads it anyway)\n");
		goto out;
	}
	ipa = ml_io_map_unmappable(IPA_PHYS, IPA_SIZE, VM_WIMG_IO);
	if (!ipa) {
		printf("popsicle_modem: cannot map the IPA window\n");
		goto out;
	}
	modem_step_ipa();

out:
	if (ipa) {
		ml_io_unmap(ipa, IPA_SIZE);
		ipa = 0;
	}
	if (gcc) {
		ml_io_unmap(gcc, GCC_SIZE);
		gcc = 0;
	}
	printf("popsicle_modem: survey done; the modem firmware is BLOCKED on the QCOM "
	    "PIL-TZ signature chain (modem.mdt + ipa_fws, no hackios modem framework "
	    "in this tree) -- the reads above are all that is safe without it; no writes\n");
	thread_terminate(current_thread());
}


/* ---- the entry point ------------------------------------------------------------------------------
 * popsicle_syna.c L1502-1515: PE_parse_boot_argn("modem_probe"), then the thread. */
void
popsicle_modem_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("modem_probe", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	if (kernel_thread_start(modem_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
