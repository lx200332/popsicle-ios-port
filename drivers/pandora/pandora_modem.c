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
 * Not done yet: interconnect votes (DT interconnects: rproc_ddr, crypto_ddr), SMP2P
 * ready/handover, GLINK/QRTR, dsm crash-dump carveouts. Same gaps as pandora_adsp.c.
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

#define MD_PAS_MAIN   4u          /* canoe_mpss_resource +0x24 : modem.mdt as extra image */
#define MD_PAS_DTB    38u         /* canoe_mpss_resource +0x28 : modem_dtb.mdt, auth_and_reset */
#define MD_PAS_STOP   3u          /* canoe_mpss_resource +0x2c : shut down before loading  */
#define MD_MEM_PA     0x8aa00000ull
#define MD_MEM_SIZE   0xeb00000ull
#define MD_DTB_PA     0x99500000ull
#define MD_DTB_SIZE   0x80000ull

int pandora_pas_load_auth(const char *file, unsigned pas, uint64_t mem_pa, uint64_t mem_size);

static int md_chan;

static void
md_sleep(unsigned ms)
{
	uint64_t deadline;

	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&md_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
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
	if (mode < 2) {
		printf("PANDORA MODEM: queries only\n");
		goto out;
	}
	md_sleep(delay_s * 1000u);

	/* qcom_q6v5_pas shuts MD_PAS_STOP down before touching the firmware
	 * (adsp_load prologue: ldr w0,[x21,#0x1e0]; cbz; bl qcom_scm_pas_shutdown). */
	err = pandora_pas_load_auth("modem_dtb", MD_PAS_DTB, MD_DTB_PA, MD_DTB_SIZE);
	printf("PANDORA MODEM: dtb image -> %d\n", err);
	err = pandora_pas_load_auth("modem", MD_PAS_MAIN, MD_MEM_PA, MD_MEM_SIZE);
	printf("PANDORA MODEM: main image -> %d\n", err);
	if (!err) {
		printf("PANDORA MODEM: modem released from reset\n");
		md_sleep(2000);
	}
	printf("PANDORA MODEM: stop id %u is the pre-load shutdown handle\n", MD_PAS_STOP);
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
