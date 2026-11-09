/*
 * PANDORA modem (MSS) bring-up. Boot-arg pandora_modem: 1 = TrustZone queries only,
 * 2 = also load and start the modem (pandora_modem_delay seconds after start, default 30).
 *
 * Every constant below comes from the live device, not from a guess:
 *
 *   carveouts   /soc/reserved-memory/mpss_region@8aa00000       0x8aa00000 + 0xeb00000 (no-map)
 *               /soc/reserved-memory/q6_mpss_dtb_region@99500000 0x99500000 + 0x80000   (no-map)
 *   firmware    /soc/remoteproc-mss@04080000 firmware-name = <"modem.mdt" "modem_dtb.mdt">
 *               memory-region = <&mpss_region &q6_mpss_dtb_region &linux,cma
 *                                &dsm_partition_1_region@84a00000 &dsm_partition_2_region@89f00000>
 *               compatible = "qcom,canoe-modem-pas", reg <0x04080000 0x10000>, reg-names "cx","mx"
 *   pas ids     vendor_dlkm.img (EROFS) -> /lib/modules/qcom_q6v5_pas.ko
 *               canoe_mpss_resource (.rodata va 0x74e0, 120 bytes):
 *                 +0x24 = 4   -> adsp_probe +0x4e0 str w8,[x19,#0x1d8]  (extra-image pas id,
 *                                used by qcom_mdt_pas_init at adsp_load +0x1fd8/0x1fdc)
 *                 +0x28 = 38  -> adsp_probe +0x564 str w10,[x19,#0x1dc] (main-path pas id,
 *                                used by qcom_mdt_pas_init at adsp_load +0x2c88/0x2c94 and by
 *                                qcom_scm_pas_auth_and_reset at adsp_start +0x1e5c/0x1e6c)
 *                 +0x2c = 3   -> adsp_probe +0x4e8 str w8,[x19,#0x1e0]  (id qcom_q6v5_pas shuts
 *                                down before loading, adsp_load prologue +0x2c20/0x2c2c)
 *                 +0x00 = 421, +0x04 = 0   (crash/fatal irq names index)
 *   glink       /soc/remoteproc-mss@04080000/glink-edge  qcom,remote-pid 1, transport "smem",
 *               mbox-names "mpss_smem"; child qcom,modem_qrtr qcom,glink-channels "IPCRTR"
 *               qcom,intents = <0x800 0x5> <0x2000 0x3> <0x4400 0x2>
 *   AOP / QMP   qcom,qmp = <&power-controller@c300000>  (same node the ADSP uses)
 *
 * The load itself reuses pandora_pas_load_auth() from bsd/dev/pandora_adsp.c, which does
 * qcom_mdt_read_metadata + PAS_INIT_IMAGE + PAS_MEM_SETUP + segment copy + PAS_AUTH_AND_RESET
 * with the same semantics as qcom_q6v5_pas.ko. Firmware comes from the pinned pack
 * (tools/mkfwpack.py) that pandora_fw_read() in sweet_ufs.c serves.
 *
 * The three legs of the PAS chain, and where each one stands (0229):
 *
 *   1 modem.mdt      pandora_pas_load_auth("modem", MD_PAS_MAIN, ...)            code path present
 *   2 modem_dtb.mdt  pandora_pas_load_auth("modem_dtb", MD_PAS_DTB, ...)         code path present
 *   3 mcfg           NOT a PAS image and there was no code path at all: the modem
 *                    software reads the whole modem_pr/mcfg tree itself once it is
 *                    up, over GLINK/QRTR, which this tree does not implement. Here
 *                    can own is the leg's precondition, so md_leg_check() reads each
 *                    mcfg manifest/image out of the pack, checks its size (and the
 *                    .mdt headers' ELF32 shape) and prints one line per leg. Nothing
 *                    is written and no SCM call is made for leg 3: md_survey_fw()
 *                    runs in every mode, and the load itself still needs
 *                    pandora_modem>=2 and a real TZ handoff.
 *
 * Not done yet: interconnect votes (DT interconnects: rproc_ddr, crypto_ddr), SMP2P
 * ready/handover, GLINK/QRTR, dsm crash-dump carveouts, and mcfg actually reaching the
 * modem. Same gaps as pandora_adsp.c.
 */
#if defined(PANDORA)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>

extern kern_return_t thread_terminate(thread_t);
extern int pandora_fw_read(uint64_t off, void *dst, uint64_t len);
void pandora_modem_init(void);

#define MD_PAS_MAIN   4u          /* canoe_mpss_resource +0x24 : modem.mdt as extra image */
#define MD_PAS_DTB    38u         /* canoe_mpss_resource +0x28 : modem_dtb.mdt, auth_and_reset */
#define MD_PAS_STOP   3u          /* canoe_mpss_resource +0x2c : shut down before loading  */
#define MD_MEM_PA     0x8aa00000ull
#define MD_MEM_SIZE   0xeb00000ull
#define MD_DTB_PA     0x99500000ull
#define MD_DTB_SIZE   0x80000ull

int pandora_pas_load_auth(const char *file, unsigned pas, uint64_t mem_pa, uint64_t mem_size);

/* The pinned firmware pack, bsd/dev/pandora_adsp.c fw_dir(): magic, count, then
 * { char name[48]; uint64_t off, size; } per entry, all inside the first 4 KiB. */
#define MD_PACK_MAGIC    "PANDFW01"
#define MD_PACK_DIR      4096u
#define MD_PACK_ENTS     63u
#define MD_PACK_NAME     48u
#define MD_ELF_HDR       52u       /* sizeof(Elf32_Ehdr): e_phnum lives at 44 */
#define MD_MDT_MIN       52ull
#define MD_MDT_MAX       0x10000ull
#define MD_CFG_MAX       (1ull << 20)

static int md_chan;

struct md_ent {
	char name[MD_PACK_NAME];
	uint64_t off, size;
};

/* One leg of the chain: the pack name, the size window it must fall in, what it is. */
struct md_leg {
	const char *what;
	const char *name;
	uint64_t min, max;
	bool header;               /* parse the first 52 bytes as an Elf32_Ehdr */
};

static const struct md_leg md_legs[] = {
	{ "modem (PAS metadata)", "modem.mdt", MD_MDT_MIN, MD_MDT_MAX, true },
	{ "modem_dtb (PAS metadata)", "modem_dtb.mdt", MD_MDT_MIN, MD_MDT_MAX, true },
	{ "mcfg software manifest", "modem_pr/mcfg/configs/mcfg_sw/mbn_sw.txt", 1, MD_CFG_MAX, false },
	{ "mcfg hardware manifest", "modem_pr/mcfg/configs/mcfg_hw/mbn_hw.txt", 1, MD_CFG_MAX, false },
	{ "mcfg hardware image",
	  "modem_pr/mcfg/configs/mcfg_hw/generic/common/Kaanapali/STANDALONE/LA/SS/mcfg_hw.mbn",
	  1, MD_CFG_MAX, false },
};

#define MD_NLEGS  (sizeof(md_legs) / sizeof(md_legs[0]))

static void
md_sleep(unsigned ms)
{
	uint64_t deadline;

	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&md_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/*
 * The pack directory, or 0 entries when there is no pack (or no UFS yet).
 *
 * md_pack_read() through md_survey_fw() are byte-level parsers over the pack
 * directory, so every one of them indexes a uint8_t array. clang 18 folds
 * -Wunsafe-buffer-usage into -Weverything and flags all 25 of those accesses;
 * the CI toolchain (Xcode 15.0.1 / clang 1500.x) does not implement that
 * warning at all -- 0 occurrences in the 16182 warning lines of 0_build.txt
 * across 1003 compiled files, while the in-tree pandora_adsp.c picks up 282 of
 * them from a local clang 18. Suppress it for this parser block only, with the
 * __has_warning guard so a toolchain that never had the warning stays silent.
 */
#if defined(__has_warning)
#if __has_warning("-Wunsafe-buffer-usage")
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#define MD_PARSER_PRAGMA 1
#endif
#endif

static unsigned
md_pack_read(struct md_ent *ents)
{
	static uint8_t dir[MD_PACK_DIR];
	uint32_t n = 0;
	int err;

	err = pandora_fw_read(0, dir, sizeof(dir));
	if (err) {
		printf("PANDORA MODEM: firmware pack read failed %d (no pack on this boot)\n", err);
		return 0;
	}
	if (memcmp(dir, MD_PACK_MAGIC, 8) != 0) {
		printf("PANDORA MODEM: no firmware pack header (%c%c%c%c%c%c%c%c)\n",
		    dir[0], dir[1], dir[2], dir[3], dir[4], dir[5], dir[6], dir[7]);
		return 0;
	}
	memcpy(&n, dir + 8, sizeof(n));
	if (n == 0 || n > MD_PACK_ENTS) {
		printf("PANDORA MODEM: firmware pack claims %u entries (limit %u)\n", n, MD_PACK_ENTS);
		return 0;
	}
	memcpy(ents, dir + 16, (size_t)n * sizeof(*ents));
	printf("PANDORA MODEM: firmware pack: %u files\n", n);
	return n;
}

static const struct md_ent *
md_pack_find(const struct md_ent *ents, unsigned n, const char *name)
{
	unsigned i;

	for (i = 0; i < n; i++) {
		if (strncmp(ents[i].name, name, MD_PACK_NAME - 1) == 0) {
			return &ents[i];
		}
	}
	return NULL;
}

/* The first 52 bytes must be a 32-bit little-endian ELF header with 2..128 phdrs,
 * the same shape pandora_adsp.c pas_load() insists on before it touches SCM. */
static bool
md_mdt_shaped(const struct md_ent *ent)
{
	uint8_t hdr[MD_ELF_HDR];
	unsigned ptype = 0, phnum;
	int err;

	err = pandora_fw_read(ent->off, hdr, sizeof(hdr));
	if (err) {
		printf("PANDORA MODEM: %s: header read failed %d\n", ent->name, err);
		return false;
	}
	ptype = hdr[0] | (unsigned)hdr[1] << 8 | (unsigned)hdr[2] << 16 | (unsigned)hdr[3] << 24;
	phnum = hdr[44] | (unsigned)hdr[45] << 8;
	if (ptype != 0x464c457fu || hdr[4] != 1 || phnum < 2 || phnum > 128) {
		printf("PANDORA MODEM: %s: not a 32-bit ELF header (magic %#x class %u phnum %u)\n",
		    ent->name, ptype, (unsigned)hdr[4], phnum);
		return false;
	}
	printf("PANDORA MODEM: %s: ELF32, %u phdrs\n", ent->name, phnum);
	return true;
}

/* Read the leg out of the pack, check its size, print one line. Nothing is loaded. */
static int
md_leg_check(const struct md_ent *ents, unsigned n, const struct md_leg *leg)
{
	const struct md_ent *ent;
	const char *shape = "";

	ent = md_pack_find(ents, n, leg->name);
	if (!ent) {
		printf("PANDORA MODEM: leg %-24s %-72s MISSING from the pack\n",
		    leg->what, leg->name);
		return ENOENT;
	}
	if (ent->size < leg->min || ent->size > leg->max) {
		printf("PANDORA MODEM: leg %-24s %s is %llu bytes, outside %llu..%llu\n",
		    leg->what, leg->name, (unsigned long long)ent->size,
		    (unsigned long long)leg->min, (unsigned long long)leg->max);
		return EINVAL;
	}
	if (leg->header) {
		if (!md_mdt_shaped(ent)) {
			printf("PANDORA MODEM: leg %-24s %s is %llu bytes, header shape bad\n",
			    leg->what, leg->name, (unsigned long long)ent->size);
			return EINVAL;
		}
		shape = " (ELF32 ok)";
	}
	printf("PANDORA MODEM: leg %-24s %s: %llu bytes, offset %#llx%s\n", leg->what,
	    leg->name, (unsigned long long)ent->size, (unsigned long long)ent->off, shape);
	return 0;
}

/*
 * Every leg's precondition, in every mode: this is the firmware checklist the driver
 * can state about itself, and it is the whole of the mcfg code path (leg 3).
 */
static void
md_survey_fw(void)
{
	struct md_ent ents[MD_PACK_ENTS];
	unsigned n, i;
	int worst = 0;

	memset(ents, 0, sizeof(ents));
	n = md_pack_read(ents);
	if (!n) {
		printf("PANDORA MODEM: no firmware pack: all %u legs are unavailable\n",
		    (unsigned)MD_NLEGS);
		return;
	}
	for (i = 0; i < MD_NLEGS; i++) {
		int err = md_leg_check(ents, n, &md_legs[i]);
		if (err && !worst) {
			worst = err;
		}
	}
	printf("PANDORA MODEM: firmware checklist -> %d (%u legs)\n", worst, (unsigned)MD_NLEGS);

	/* The .bNN companions the loader will ask for, listed but not opened. */
	for (i = 0; i < n; i++) {
		if (strncmp(ents[i].name, "modem", 5) != 0 &&
		    strncmp(ents[i].name, "modem_pr", 8) != 0) {
			continue;
		}
		if (ents[i].size > MD_CFG_MAX && strncmp(ents[i].name, "modem_pr", 8) == 0) {
			continue;       /* the mcfg tree has hundreds of entries; the legs above name the ones used */
		}
		printf("PANDORA MODEM:   pack %-44s %10llu bytes\n", ents[i].name,
		    (unsigned long long)ents[i].size);
	}
}

#if defined(MD_PARSER_PRAGMA)
#pragma clang diagnostic pop
#undef MD_PARSER_PRAGMA
#endif

static void
modem_thread(void *arg, wait_result_t result)
{
	int mode = 0, err;
	unsigned delay_s = 30;

	(void)arg;
	(void)result;
	(void)PE_parse_boot_argn("pandora_modem", &mode, sizeof(mode));
	(void)PE_parse_boot_argn("pandora_modem_delay", &delay_s, sizeof(delay_s));
	printf("PANDORA MODEM: probe (mode %d, delay %u s)\n", mode, delay_s);

	/* Legs 1-3: presence, size and header shape, read-only, every mode. */
	md_survey_fw();

	if (mode < 2) {
		printf("PANDORA MODEM: queries only: nothing is loaded\n");
		goto out;
	}
	md_sleep(delay_s * 1000u);

	/* qcom_q6v5_pas shuts MD_PAS_STOP down before touching the firmware
	 * (adsp_load prologue: ldr w0,[x21,#0x1e0]; cbz; bl sp->shutdown). */
	err = pandora_pas_load_auth("modem_dtb", MD_PAS_DTB, MD_DTB_PA, MD_DTB_SIZE);
	printf("PANDORA MODEM: dtb image -> %d\n", err);
	err = pandora_pas_load_auth("modem", MD_PAS_MAIN, MD_MEM_PA, MD_MEM_SIZE);
	printf("PANDORA MODEM: main image -> %d\n", err);
	if (!err) {
		printf("PANDORA MODEM: modem released from reset\n");
		md_sleep(2000);
	}
	printf("PANDORA MODEM: stop id %u is the pre-load shutdown handle\n", MD_PAS_STOP);
	/*
	 * Leg 3 has no PAS call and no write: mcfg reaches the modem over GLINK/QRTR
	 * once the modem software is running, which this tree does not implement. The
	 * preconditions were checked by md_survey_fw(); say so rather than pretend.
	 */
	printf("PANDORA MODEM: mcfg leg: checked above, not loaded (needs GLINK/QRTR, "
	    "not implemented in this tree)\n");
out:
	printf("PANDORA MODEM: done\n");
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
pandora_modem_init(void)
{
	thread_t thread;
	int on = 0;

	(void)PE_parse_boot_argn("pandora_modem", &on, sizeof(on));
	if (!on) {
		return;
	}
	if (kernel_thread_start(modem_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* PANDORA */
