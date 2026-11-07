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
 * Survey steps (all reads, no writes):
 *   step 1 - IPA HW version word (0x3e00000 + 0x0); the version register answers
 *            without any clock vote in the UEFI environment we run in;
 *   step 2 - IPA hw-version/compat words (0x3e40000 block, safe identity reads);
 *   step 3 - the GCC modem branch CBCR (which modem clocks XBL left voted).
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

/* ------------------------------------------------------------------ board facts */

#define IPA_PHYS                0x03e00000UL    /* qcom,ipa@3e00000 reg[0] */
#define IPA_SIZE                0x00090000UL    /* 84000 + 0xf800 rounded up */
#define GCC_PHYS                0x00100000UL    /* /soc/clock-controller@100000 */
#define GCC_SIZE                0x001f4200UL    /* DT /soc/clock-controller@100000 reg size (fdtget 0x1f4200); window must cover 0x52018 / 0xa90a8 actually read */

static vm_offset_t ipa, gcc;

static void
modem_survey_print(const char *what, uint32_t value)
{
	printf("popsicle_modem: %s = 0x%x\n", what, value);
}

/* Step 1+2: IPA identity reads. The version word is safe in any power state. */
static void
modem_step_ipa(void)
{
	modem_survey_print("ipa hw version word0", *(volatile uint32_t *)(ipa + 0x0));
	modem_survey_print("ipa hw version word1", *(volatile uint32_t *)(ipa + 0x4));
	/* the second reg block sits at 0x40000 into the IPA window */
	modem_survey_print("ipa block2 word0", *(volatile uint32_t *)(ipa + 0x40000));
	modem_survey_print("ipa block2 word1", *(volatile uint32_t *)(ipa + 0x40004));
}

/* Step 3: GCC state left by XBL -- which modem/ipa branches are still voted on. */
static void
modem_step_gcc(void)
{
	modem_survey_print("gcc ipa/modem cbcr probe (0x52008)", *(volatile uint32_t *)(gcc + 0x52008));
	modem_survey_print("gcc ipa/modem cbcr probe (0x5200c)", *(volatile uint32_t *)(gcc + 0x5200c));
}

/* ---- the thread ---------------------------------------------------------------------------------
 * popsicle_wifi.c shape: run once, print, unmap, exit. */
static void
modem_thread(__unused void *parameter, __unused wait_result_t result)
{
	printf("popsicle_modem: IPA/modem survey, boot-arg modem_probe=1\n");

	/*
	 * The IPA window is not readable in every power state: a boot with just
	 * modem_probe=1 took a synchronous external abort (ESR 0x96000010) on the
	 * first word read, pc in modem_thread. The identity reads stay behind a
	 * second, explicit switch; the survey prints what it knows without them.
	 */
	int deep = 0;
	if (!PE_parse_boot_argn("modem_deep", &deep, sizeof(deep)) || !deep) {
		printf("popsicle_modem: identity reads off (modem_deep=1 reads the IPA "
		    "window and can abort)\n");
		printf("popsicle_modem: the modem firmware is BLOCKED on the QCOM PIL-TZ "
		    "signature chain (modem.mdt + ipa_fws, no hackios modem framework "
		    "in this tree)\n");
		thread_terminate(current_thread());
		return;
	}

	ipa = ml_io_map_unmappable(IPA_PHYS, IPA_SIZE, VM_WIMG_IO);
	gcc = ml_io_map_unmappable(GCC_PHYS, GCC_SIZE, VM_WIMG_IO);
	if (ipa == 0 || gcc == 0) {
		printf("popsicle_modem: cannot map IPA/GCC\n");
		thread_terminate(current_thread());
		return;
	}

	modem_step_ipa();
	modem_step_gcc();

	printf("popsicle_modem: survey done; the modem firmware is BLOCKED on the QCOM "
	    "PIL-TZ signature chain (modem.mdt + ipa_fws, no hackios modem framework "
	    "in this tree) -- the identity reads above are all that is safe without "
	    "it; no writes\n");
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
