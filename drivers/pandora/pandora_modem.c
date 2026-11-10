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
 * The load itself reuses pandora_pas_load_auth_part() from bsd/dev/pandora_adsp.c, which does
 * qcom_mdt_read_metadata + PAS_INIT_IMAGE + PAS_MEM_SETUP + segment copy + PAS_AUTH_AND_RESET
 * with the same semantics as qcom_q6v5_pas.ko.
 *
 * Where the firmware is (0230): NOT in the pinned pack, and NOT in modem_a either.
 * modem_a's image/ is device firmware_mnt (457 files, the OTA's modem/image) and holds
 * adsp.mdt but no modem.mdt at all; modem.mdt, modem.b00..b26 (67,100,917 bytes) and the
 * whole modem_pr tree are in modemfirmware_a. The pack is 16384 blocks (64 MiB) and the
 * modem images alone are 67 MB, so the pack can never hold them: this driver reads
 * FW_MF (modemfirmware_<slot>) through pandora_fw_volume_size/read plus
 * pandora_pas_load_auth_part(). The pack is still what the GPU reads.
 *
 * The three legs of the PAS chain, and where each one stands (0229):
 *
 *   0 shutdown       qcom_scm_pas_shutdown(MD_PAS_STOP) -- qcom_q6v5_pas shuts the
 *                    remote down before it touches any image (adsp_load prologue:
 *                    ldr w0,[x21,#0x1e0]; cbz; bl sp->shutdown). Without this the
 *                    modem may still be running the boot loader's image and both
 *                    PAS_INIT_IMAGE calls fight it.
 *   1 modem_dtb.mdt  the DTB image goes first, and MD_PAS_DTB (38) is the id that
 *                    carries PAS_AUTH_AND_RESET -- canoe_mpss_resource +0x28 is the
 *                    main-path id, +0x24 the extra image. adsp_start +0x1e6c reads
 *                    +0x1dc (which adsp_probe fills from +0x28) and calls
 *                    qcom_scm_pas_auth_and_reset with it.
 *   2 modem.mdt      loaded as the extra image (MD_PAS_MAIN = 4 = +0x24): init_image
 *                    and mem_setup only, no auth. Authing it as well, as an earlier
 *                    revision of this driver did, asks TZ to reset a processor twice.
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
void pandora_modem_init(void);

#define MD_PAS_MAIN   4u          /* canoe_mpss_resource +0x24 : modem.mdt as extra image */
#define MD_PAS_DTB    38u         /* canoe_mpss_resource +0x28 : modem_dtb.mdt, auth_and_reset */
#define MD_PAS_STOP   3u          /* canoe_mpss_resource +0x2c : shut down before loading  */
#define MD_MEM_PA     0x8aa00000ull
#define MD_MEM_SIZE   0xeb00000ull
#define MD_DTB_PA     0x99500000ull
#define MD_DTB_SIZE   0x80000ull

/* modemfirmware_<slot>: modem.mdt + modem.b00..b26 + modem_pr. pandora_adsp.c owns the
 * volumes (FW_MODEM is modem_a, FW_MF is modemfirmware_a). */
#define MD_VOL        FW_MF

extern int pandora_pas_load_auth_part(const char *file, unsigned pas, uint64_t mem_pa,
    uint64_t mem_size, int part);
extern int pandora_fw_volume_size(int part, const char *path, uint64_t *size);
extern int pandora_fw_volume_read(int part, const char *path, uint64_t off, void *dst, uint64_t len);

/* pandora_adsp.c defines these; this file only names the volume it wants. */
#define FW_PACK   0
#define FW_MODEM  1
#define FW_MF     2

#define MD_ELF_HDR       52u       /* sizeof(Elf32_Ehdr): e_phnum lives at 44 */
#define MD_MDT_MIN       52ull
#define MD_MDT_MAX       0x10000ull
#define MD_CFG_MAX       (1ull << 20)

/* pandora_adsp.c owns the SMC transport and its lock; this file only needs the shutdown
 * pair, so it borrows the same entry point rather than opening a second one. */
struct scm_res {
	uint64_t a0, a1, a2, a3;
};

extern void pandora_scm_smc(uint64_t fn, uint64_t arginfo, uint64_t x2, uint64_t x3,
    uint64_t x4, uint64_t x5, struct scm_res *res);

#define SCM_FN(svc, cmd) \
	((1ULL << 30) | (2ULL << 24) | (((uint64_t)(svc) & 0xff) << 8) | ((cmd) & 0xff))
#define SCM_ARGS(n)      ((uint64_t)(n) & 0xf)


static int md_chan;

/* One leg of the chain: its path inside the volume (relative to the volume root, so the
 * FAT's "image/" prefix included), the size window it must fall in, what it is. */
struct md_leg {
	const char *what;
	const char *path;
	uint64_t min, max;
	bool header;               /* parse the first 52 bytes as an Elf32_Ehdr */
};

static const struct md_leg md_legs[] = {
	{ "modem (PAS metadata)", "image/modem.mdt", MD_MDT_MIN, MD_MDT_MAX, true },
	{ "modem_dtb (PAS metadata)", "image/modem_dtb.mdt", MD_MDT_MIN, MD_MDT_MAX, true },
	{ "mcfg software manifest", "image/modem_pr/mcfg/configs/mcfg_sw/mbn_sw.txt", 1, MD_CFG_MAX, false },
	{ "mcfg hardware manifest", "image/modem_pr/mcfg/configs/mcfg_hw/mbn_hw.txt", 1, MD_CFG_MAX, false },
	{ "mcfg hardware image",
	  "image/modem_pr/mcfg/configs/mcfg_hw/generic/common/Kaanapali/STANDALONE/LA/SS/mcfg_hw.mbn",
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
 * md_survey_fw() and md_mdt_shaped() index uint8_t header arrays, so every one of
 * those accesses is an unsafe-buffer one. clang 18 folds
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

/* The first 52 bytes must be a 32-bit little-endian ELF header with 2..128 phdrs,
 * the same shape pandora_adsp.c pas_load() insists on before it touches SCM. */
static bool
md_mdt_shaped(const struct md_leg *leg)
{
	uint8_t hdr[MD_ELF_HDR];
	unsigned ptype, phnum;
	int err;

	err = pandora_fw_volume_read(MD_VOL, leg->path, 0, hdr, sizeof(hdr));
	if (err) {
		printf("PANDORA MODEM: %s: header read failed %d\n", leg->path, err);
		return false;
	}
	ptype = hdr[0] | (unsigned)hdr[1] << 8 | (unsigned)hdr[2] << 16 | (unsigned)hdr[3] << 24;
	phnum = hdr[44] | (unsigned)hdr[45] << 8;
	if (ptype != 0x464c457fu || hdr[4] != 1 || phnum < 2 || phnum > 128) {
		printf("PANDORA MODEM: %s: not a 32-bit ELF header (magic %#x class %u phnum %u)\n",
		    leg->path, ptype, (unsigned)hdr[4], phnum);
		return false;
	}
	printf("PANDORA MODEM: %s: ELF32, %u phdrs\n", leg->path, phnum);
	return true;
}

/* Stat the leg in the modemfirmware volume, check its size, print one line. Nothing is
 * read into the carveout and no SCM call is made. */
static int
md_leg_check(const struct md_leg *leg)
{
	uint64_t size = 0;
	const char *shape = "";
	int err;

	err = pandora_fw_volume_size(MD_VOL, leg->path, &size);
	if (err) {
		printf("PANDORA MODEM: leg %-24s %-72s MISSING from modemfirmware (%d)\n",
		    leg->what, leg->path, err);
		return err;
	}
	if (size < leg->min || size > leg->max) {
		printf("PANDORA MODEM: leg %-24s %s is %llu bytes, outside %llu..%llu\n",
		    leg->what, leg->path, (unsigned long long)size,
		    (unsigned long long)leg->min, (unsigned long long)leg->max);
		return EINVAL;
	}
	if (leg->header) {
		if (!md_mdt_shaped(leg)) {
			printf("PANDORA MODEM: leg %-24s %s is %llu bytes, header shape bad\n",
			    leg->what, leg->path, (unsigned long long)size);
			return EINVAL;
		}
		shape = " (ELF32 ok)";
	}
	printf("PANDORA MODEM: leg %-24s %s: %llu bytes%s\n", leg->what, leg->path,
	    (unsigned long long)size, shape);
	return 0;
}

/*
 * Every leg's precondition, in every mode: the firmware checklist the driver can state
 * about itself, and the whole of the mcfg code path (leg 3).
 */
static void
md_survey_fw(void)
{
	unsigned i;
	int worst = 0;

	for (i = 0; i < MD_NLEGS; i++) {
		int err = md_leg_check(&md_legs[i]);
		if (err && !worst) {
			worst = err;
		}
	}
	printf("PANDORA MODEM: firmware checklist -> %d (%u legs)\n", worst, (unsigned)MD_NLEGS);
}

#if defined(MD_PARSER_PRAGMA)
#pragma clang diagnostic pop
#undef MD_PARSER_PRAGMA
#endif

/*
 * qcom_scm_pas_shutdown: SCM_FN(2, 6) is PAS_SHUTDOWN and SCM_FN(2, 7) is
 * PAS_SUPPORTED, the same pair the ADSP path uses (pandora_adsp.c adsp_boot). Asking
 * "supported" first turns an unsupported id from a silent failure into a log line.
 */
static void
md_pas_shutdown(unsigned pas)
{
	struct scm_res sup, res;

	pandora_scm_smc(SCM_FN(2, 7), SCM_ARGS(1), pas, 0, 0, 0, &sup);
	pandora_scm_smc(SCM_FN(2, 6), SCM_ARGS(1), pas, 0, 0, 0, &res);
	printf("PANDORA MODEM: pas %u supported a0 %#llx a1 %#llx, shutdown -> a0 %#llx a1 %#llx\n",
	    pas, sup.a0, sup.a1, res.a0, res.a1);
}

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

	/* Legs 1-3: presence, size and header shape in modemfirmware_<slot>, read-only,
	 * every mode. */
	md_survey_fw();

	if (mode < 2) {
		printf("PANDORA MODEM: queries only: nothing is loaded\n");
		goto out;
	}
	md_sleep(delay_s * 1000u);

	/* Order copied from qcom_q6v5_pas, which is what the vendor driver runs: stop the
	 * remote first, load the DTB image (the one that carries auth), then the main
	 * image as an extra one. The header gives the offset behind each id. */
	md_pas_shutdown(MD_PAS_STOP);

	err = pandora_pas_load_auth_part("modem_dtb", MD_PAS_DTB, MD_DTB_PA, MD_DTB_SIZE, MD_VOL);
	printf("PANDORA MODEM: dtb image (pas %u, carries auth) -> %d\n", MD_PAS_DTB, err);
	if (err) {
		/* This is the image TZ authenticates. Staging the main image after it
		 * failed would only produce a second, less specific error, so stop and
		 * say which leg broke. */
		printf("PANDORA MODEM: dtb image failed, main image not attempted\n");
		goto out_stop;
	}
	err = pandora_pas_load_auth_part("modem", MD_PAS_MAIN, MD_MEM_PA, MD_MEM_SIZE, MD_VOL);
	printf("PANDORA MODEM: main image (pas %u, extra image) -> %d\n", MD_PAS_MAIN, err);
	if (!err) {
		printf("PANDORA MODEM: modem released from reset\n");
		md_sleep(2000);
	}
	goto out;
out_stop:
	/* qcom_pas_start's error path: undo the auth of the image that did load. */
	md_pas_shutdown(MD_PAS_DTB);
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
