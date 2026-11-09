/*
 * PANDORA USB. pandora_usb=2 (with qcom-usb-debug=1): the console log as a USB serial port (CDC
 * ACM; Windows binds usbser, a COM port) through QcomXnuUsbDebug (bsd/dev/qcomusb, from
 * nabu-usb-cdc's module, profile soc/Sm8850.h). The bootloader hands the controller over clocked,
 * in device role and halted (usb1), which is the state the module takes over without bringing
 * anything up. A thread bound to the boot CPU (the module serves the hardware from there only)
 * starts the module, then every PU_PERIOD_MS hands it the console ring's new bytes
 * (sweet_log_copy, from the start of the ring, so the boot is included) and polls the
 * controller, which completes enumeration and moves data. Bytes the module cannot take yet are
 * offered again; a reader that falls a whole ring behind skips ahead (sweet_log_copy). Before any
 * reset the rest goes out from the resetting CPU (pandora_usb_final).
 * pandora_usb=1, read only: the state the bootloader leaves the primary USB controller in when
 * it hands over, which decides what the serial console has to bring up itself.
 * - GCC (gcc-kaanapali.c): the five clocks of the live DT's ssusb@a600000 (core = usb30_prim_master
 *   0x39018, iface = cfg_noc_usb3_prim_axi 0x39090, bus_aggr = aggre_usb3_prim_axi 0x39094, utmi =
 *   usb30_prim_mock_utmi 0x3902c, sleep = usb30_prim_sleep 0x39028; CLK_OFF bit 31, enable bit 0),
 *   the controller's reset (BCR 0x39000) and power domain (GDSCR 0x39004, PWR_ON bit 31;
 *   CFG_GDSCR 0x39008).
 * - Only with the core and iface clocks running and the power domain on: the DWC3 (0xa600000)
 *   identity, global, device and event-buffer registers, twice 100 ms apart (an event count or
 *   state that moves means someone still runs it), and the Qualcomm QSCRATCH registers.
 * The PHY (eUSB2, 0x88e3000) is not read by the survey above: its clocks are not ours to check
 * here. pandora_usb_phy=1 (default 0) adds a second, read-only pass over it, over the controller's
 * role registers and over the PD/UCSI placeholders; nothing in that pass writes a register.
 * The apps SMMU's entry for the USB stream (0x40) is already known from the audio survey: group
 * 3 -> bank 3, translation off (spk6), so the controller's DMA sees physical addresses.
 */
#if defined(PANDORA)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>
#include <kern/cpu_number.h>
#include <kern/processor.h>
#include <IOKit/IOReturn.h>          /* kIOReturnUnsupported, for the PD placeholder */
#include "qcomusb/include/QcomXnuUsbDebug.h"

extern kern_return_t thread_terminate(thread_t);
extern uint32_t sweet_log_copy(uint64_t *pos, char *out, uint32_t max);
extern uint32_t sweet_log_copy_any(uint64_t *pos, char *out, uint32_t max);   /* 0258: unfinished bytes too */
/* osfmk's, not in the headers BSD code sees (kern/processor.h, kern/sched_prim.h); master_processor
 * itself is a per-CPU macro there */
extern processor_t cpu_to_processor(int cpu);
extern processor_t thread_bind(processor_t processor);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, uint32_t);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
void pandora_usb_init(void);
kern_return_t usb_pd_state(void);       /* placeholder: this tree has no UCSI/PD client */
void usb_phy_calibrate(vm_offset_t);    /* read-only trim dump, never writes */

#define PU_GCC_PA     0x00139000u      /* GCC 0x100000 + the USB30_PRIM block */
#define PU_DWC3_PA    0x0a60c000u      /* DWC3 global and device registers (0xc000-0xcfff) */
#define PU_QSCR_PA    0x0a6f8000u      /* QSCRATCH at 0xf8800 */
#define PU_EUSB2_PA   0x088e3000u      /* DT hsphy@88e3000, reg <0x88e3000 0x29c> (canoe_base.dts:17997) */
#define PU_EUSB2_MAP  0x1000u          /* page granularity; every offset read below is under 0x29c */
#define PU_EUSB2_RAW  0x40u            /* bytes dumped verbatim: no register map for this block in tree */
#define PU_CLK_OFF    (1u << 31)
#define PU_PWR_ON     (1u << 31)

static const struct {
	uint32_t off;
	const char *name;
} pu_clk[5] = {{0x018, "core"}, {0x090, "iface"}, {0x094, "aggr"}, {0x02c, "utmi"}, {0x028, "sleep"}};

static uint32_t
pu_rd(vm_offset_t base, uint32_t off)
{
	const uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
pu_dwc3(vm_offset_t d, vm_offset_t q, const char *when)
{
	/* offsets from 0xa600000, minus the 0xc000 the mapping starts at */
	printf("PANDORA USB: DWC3 %s: GSNPSID %08x GCTL %08x GSTS %08x GUSB2PHYCFG %08x GUSB3PIPECTL %08x DCFG %08x "
	    "DCTL %08x DEVTEN %08x DSTS %08x DALEPENA %08x GEVNT %08x:%08x size %08x count %08x\n", when,
	    pu_rd(d, 0x120), pu_rd(d, 0x110), pu_rd(d, 0x118), pu_rd(d, 0x200), pu_rd(d, 0x2c0), pu_rd(d, 0x700),
	    pu_rd(d, 0x704), pu_rd(d, 0x708), pu_rd(d, 0x70c), pu_rd(d, 0x720), pu_rd(d, 0x404), pu_rd(d, 0x400),
	    pu_rd(d, 0x408), pu_rd(d, 0x40c));
	printf("PANDORA USB: QSCRATCH %s: GENERAL_CFG %08x HS_PHY_CTRL %08x SS_PHY_CTRL %08x PWR_EVNT_IRQ_STAT %08x\n", when,
	    pu_rd(q, 0x808), pu_rd(q, 0x810), pu_rd(q, 0x830), pu_rd(q, 0x858));
}

/*
 * Data role. Two registers, and they do not say the same thing: GCTL.PrtCapDir is the role the
 * core was told to take (a control register, it reads back what was written), GSTS.CurMod is the
 * role the core reports it is in. qcomusb uses exactly this pair and trusts GSTS only
 * (bsd/dev/qcomusb/soc/QcomUsbPrepare.c:311-328: "Acceptance reads GSTS, not GCTL"). Offsets:
 * GCTL 0xc110 and GSTS 0xc118 from the core base (qcomusb/usb/Dwc3Regs.h:98,100), i.e. 0x110 and
 * 0x118 from this mapping, which starts at 0xa60c000.
 */
#define PU_GCTL      0x110u
#define PU_GSTS      0x118u
#define PU_OTG_OEVT  0xc08u     /* Dwc3Regs.h:157, inside the 0xcc00 OTG block: Dwc3Regs.h:154-159 */
#define PU_OTG_OSTS  0xc10u     /* Dwc3Regs.h:159 */

static void
pu_role(vm_offset_t d)
{
	const uint32_t gctl = pu_rd(d, PU_GCTL);
	const uint32_t gsts = pu_rd(d, PU_GSTS);
	const uint32_t dir = (gctl & (3u << 12)) >> 12;   /* DWC3_GCTL_PRTCAP, Dwc3Regs.h:172 */
	const uint32_t cur = gsts & 0x3u;                 /* GSTS_CURMOD, QcomUsbPrepare.c:30 */
	/* the two fields encode the same role differently: PrtCapDir 1 = host, 2 = device;
	 * CurMod 1 = host, 0 = device */
	const bool agree = (dir == 1u && cur == 1u) || (dir == 2u && cur == 0u);
	const bool otg = dir == 3u || cur == 2u;

	printf("PANDORA USB: role: GCTL %08x PrtCapDir %u (%s) | GSTS %08x CurMod %u (%s) | OEVT %08x OSTS %08x\n",
	    gctl, dir, dir == 1 ? "host" : dir == 2 ? "device" : dir == 3 ? "otg" : "unknown",
	    gsts, cur, cur == 0 ? "device" : cur == 1 ? "host" : cur == 2 ? "drd" : "unknown",
	    pu_rd(d, PU_OTG_OEVT), pu_rd(d, PU_OTG_OSTS));
	printf("PANDORA USB: role: %s\n",
	    otg ? "OTG capable (DT dr_mode=\"otg\", canoe_base.dts:17985) and no role switch driver in this tree"
	    : agree ? "GCTL and GSTS agree"
	    : "GCTL and GSTS disagree: GSTS is the one the core is actually in");
}

/*
 * The eUSB2 PHY at 0x88e3000. DT hsphy@88e3000 (canoe_base.dts:17995-18008, reg <0x88e3000 0x29c>,
 * compatible "qcom,usb-m31-eusb2-phy", alias eusb2_phy0 at canoe_base.dts:23862). Nothing in this
 * tree maps that block, so the named offsets below are taken from the only description of it on
 * this machine, the Linux driver drivers/phy/qualcomm/phy-qcom-m31-eusb2.c:21-57 (a Linux tree at
 * /opt/work/wslk), and every one of them is a *control* register: that driver writes them in three
 * tables (setup / override / reset) and never reads a status or calibration-done bit, because the
 * block has none. So this pass reports what the offsets say the PHY was configured to, prints the
 * first PU_EUSB2_RAW bytes raw, and claims no calibration result of any kind.
 */
#define PU_PHY_UTMI_CTRL0    0x3cu    /* UTMI_CTRL0, SLEEPM bit 0 */
#define PU_PHY_UTMI_CTRL5    0x50u    /* UTMI_CTRL5, POR bit 1 */
#define PU_PHY_HS_CTRL_COMM0 0x54u    /* HS_PHY_CTRL_COMMON0: EN bit0 SEL bit1 SIDDQ bit2 FSEL 6:4 */
#define PU_PHY_HS_CTRL2      0x64u    /* HS_PHY_CTRL2: USB2_SUSPEND_N bit2, its SEL bit3 */
#define PU_PHY_CFG0          0x94u    /* CFG0, UTMI_PHY_CMN_CTRL_OVERRIDE_EN bit 1 */
#define PU_PHY_FSEL_SEL      0xb8u    /* FSEL_SEL bit 0 */
#define PU_PHY_CFG1          0x154u   /* CFG1, PLL_EN bit 0 */
#define PU_PHY_XCFGI_7_0     0x15cu   /* PLL_LOCK_TIME 1:0 (a trim value, not a lock flag) */
#define PU_PHY_XCFGI_31_24   0x168u   /* HSTX_SLEW 2:0 */
#define PU_PHY_XCFGI_39_32   0x16cu   /* HSTX_PE 3:2 */
#define PU_PHY_XCFGI_71_64   0x17cu   /* HSTX_SWING 3:0 */

static void
pu_phy(vm_offset_t p)
{
	const uint32_t c0 = pu_rd(p, PU_PHY_HS_CTRL_COMM0);
	const uint32_t c5 = pu_rd(p, PU_PHY_UTMI_CTRL5);
	const uint32_t c2 = pu_rd(p, PU_PHY_HS_CTRL2);
	const uint32_t cfg0 = pu_rd(p, PU_PHY_CFG0);
	const uint32_t cfg1 = pu_rd(p, PU_PHY_CFG1);

	printf("PANDORA USB: PHY eUSB2 %08x: UTMI_CTRL0 %08x (SLEEPM %u) UTMI_CTRL5 %08x (POR %u) "
	    "HS_CTRL_COMMON0 %08x (PHY_ENABLE %u FSEL %u) CFG0 %08x (OVERRIDE_EN %u)\n", PU_EUSB2_PA,
	    pu_rd(p, PU_PHY_UTMI_CTRL0), pu_rd(p, PU_PHY_UTMI_CTRL0) & 1u, c5, (c5 >> 1) & 1u,
	    c0, c0 & 1u, (c0 >> 4) & 7u, cfg0, (cfg0 >> 1) & 1u);
	printf("PANDORA USB: PHY eUSB2: FSEL_SEL %08x (SEL %u) CFG1 %08x (PLL_EN %u) HS_CTRL2 %08x "
	    "(SUSPEND_N %u SEL %u) XCFGI 7_0 %08x 31_24 %08x 39_32 %08x 71_64 %08x\n",
	    pu_rd(p, PU_PHY_FSEL_SEL), pu_rd(p, PU_PHY_FSEL_SEL) & 1u, cfg1, cfg1 & 1u, c2,
	    (c2 >> 2) & 1u, (c2 >> 3) & 1u, pu_rd(p, PU_PHY_XCFGI_7_0), pu_rd(p, PU_PHY_XCFGI_31_24),
	    pu_rd(p, PU_PHY_XCFGI_39_32), pu_rd(p, PU_PHY_XCFGI_71_64));
	printf("PANDORA USB: PHY eUSB2 raw 0x00-%02x (no in-tree register map for this block):\n", PU_EUSB2_RAW - 1u);
	for (uint32_t off = 0; off < PU_EUSB2_RAW; off += 16u) {
		printf("PANDORA USB:  %02x: %08x %08x %08x %08x\n", off, pu_rd(p, off), pu_rd(p, off + 4u),
		    pu_rd(p, off + 8u), pu_rd(p, off + 12u));
	}
}

/*
 * Trim/calibration, read only. The PHY keeps its HS transmit trim in XCFGI registers (the four above)
 * and its PLL lock target in XCFGI_7_0; the Linux driver only ever writes them (phy-qcom-m31-eusb2.c
 * m31_eusb_phy_override_tbl) and has no way to read a "calibrated" flag back. Writing them from here
 * would replace values the bootloader set against the actual board, and a wrong HS trim takes USB
 * down with no way back, so this reports and stops. The "matches the override table" line below is
 * a comparison against that table, not a calibration and not a verdict on the link.
 */
void
usb_phy_calibrate(vm_offset_t p)
{
	const uint32_t x7 = pu_rd(p, PU_PHY_XCFGI_7_0);
	const uint32_t x31 = pu_rd(p, PU_PHY_XCFGI_31_24);
	const uint32_t x39 = pu_rd(p, PU_PHY_XCFGI_39_32);
	const uint32_t x71 = pu_rd(p, PU_PHY_XCFGI_71_64);
	const uint32_t pe = (x39 >> 2) & 3u, slew = x31 & 7u, swing = x71 & 0xfu, lock = x7 & 3u;
	const bool as_override = pe == 0u && slew == 0u && swing == 7u && lock == 0u;

	printf("PANDORA USB: PHY trim: HSTX_PE %u HSTX_SLEW %u HSTX_SWING %u PLL_LOCK_TIME %u -> %s\n",
	    pe, slew, swing, lock, as_override ? "matches the m31 override table" : "board values, keep them");
	printf("PANDORA USB: PHY trim: no calibration-done bit exists in this block; nothing written, result unknown\n");
}

/*
 * PD and BC1.2. Both live on the PMIC, behind the pmic-glink UCSI channel: DT pmic-glink
 * (canoe_base.dts:8569-8572, qcom,pmic-glink-channel = "PMIC_RTR_SOCCP_APPS", qcom,subsys-name =
 * "soccp") holds ucsi-glink (canoe_base.dts:8584-8586, compatible "qcom,ucsi-glink", alias ucsi at
 * canoe_base.dts:23150). That channel is served by the soccp firmware over GLINK; this tree has the
 * GLINK transport (bsd/dev/qcom/QcomGlink.c) but no UCSI client, no altmode client and no
 * typec/PD stack anywhere (grep -ri ucsi over bsd/ finds nothing), so there is no base address to
 * read: the state is not in a register this kernel can reach. Honest placeholder, not a stub that
 * hides a defect.
 */
kern_return_t
usb_pd_state(void)
{
	printf("PANDORA USB: UCSI/PD not implemented (pmic-glink channel has no XNU driver)\n");
	printf("PANDORA USB: PD: ucsi-glink canoe_base.dts:8584 under pmic-glink canoe_base.dts:8569; "
	    "no UCSI client in this tree, so role/power/BC1.2 state is unknown\n");
	return kIOReturnUnsupported;
}

/*
 * The opt-in pass (boot-arg pandora_usb_phy=1, default 0): the eUSB2 PHY, the data role the core
 * is in, and the PD/UCSI placeholder. Read only, everywhere: no register is written by any of it.
 * It keeps its own copy of the GCC gate because it also runs on the console path (pandora_usb=2),
 * which never runs the survey below.
 */
static void pu_extras(void)
{
	vm_offset_t g, d, p;
	uint32_t core, iface, bcr, gdscr;
	bool on;

	g = ml_io_map_unmappable(PU_GCC_PA, 0x1000, VM_WIMG_IO);
	if (!g) {
		printf("PANDORA USB: PHY pass: GCC not mapped\n");
		return;
	}
	core = pu_rd(g, 0x018);
	iface = pu_rd(g, 0x090);
	bcr = pu_rd(g, 0x000);
	gdscr = pu_rd(g, 0x004);
	ml_io_unmap(g, 0x1000);
	on = !(core & PU_CLK_OFF) && !(iface & PU_CLK_OFF) && (gdscr & PU_PWR_ON) && !(bcr & 1u);
	printf("PANDORA USB: PHY pass: clocks core %08x iface %08x BCR %08x GDSCR %08x -> controller %s\n",
	    core, iface, bcr, gdscr, on ? "up" : "down");
	if (!on) {
		printf("PANDORA USB: PHY pass skipped: clocks or power domain are off, a read of the DWC3 "
		    "or PHY block would hang\n");
		return;
	}
	d = ml_io_map_unmappable(PU_DWC3_PA, 0x1000, VM_WIMG_IO);
	p = ml_io_map_unmappable(PU_EUSB2_PA, PU_EUSB2_MAP, VM_WIMG_IO);
	if (!d || !p) {
		printf("PANDORA USB: PHY pass: DWC3 %s eUSB2 %s not mapped\n", d ? "ok" : "not", p ? "ok" : "not");
		if (d) {
			ml_io_unmap(d, 0x1000);
		}
		if (p) {
			ml_io_unmap(p, PU_EUSB2_MAP);
		}
		return;
	}
	pu_role(d);
	pu_phy(p);
	usb_phy_calibrate(p);
	(void)usb_pd_state();
	ml_io_unmap(d, 0x1000);
	ml_io_unmap(p, PU_EUSB2_MAP);
}

static void
pu_probe(void)
{
	vm_offset_t g = ml_io_map_unmappable(PU_GCC_PA, 0x1000, VM_WIMG_IO);
	if (!g) {
		printf("PANDORA USB: GCC not mapped\n");
		return;
	}
	uint32_t cbcr[5];
	for (unsigned i = 0; i < 5; i++) {
		cbcr[i] = pu_rd(g, pu_clk[i].off);
	}
	const uint32_t bcr = pu_rd(g, 0x000), gdscr = pu_rd(g, 0x004), cfg = pu_rd(g, 0x008);
	ml_io_unmap(g, 0x1000);
	printf("PANDORA USB: clocks core %08x iface %08x aggr %08x utmi %08x sleep %08x; BCR %08x GDSCR %08x CFG_GDSCR %08x\n",
	    cbcr[0], cbcr[1], cbcr[2], cbcr[3], cbcr[4], bcr, gdscr, cfg);
	const bool on = !(cbcr[0] & PU_CLK_OFF) && !(cbcr[1] & PU_CLK_OFF) && (gdscr & PU_PWR_ON) && !(bcr & 1u);
	if (!on) {
		printf("PANDORA USB: core or iface clock off, power domain off or reset held: the controller is not read\n");
		return;
	}
	vm_offset_t d = ml_io_map_unmappable(PU_DWC3_PA, 0x1000, VM_WIMG_IO);
	vm_offset_t q = ml_io_map_unmappable(PU_QSCR_PA, 0x1000, VM_WIMG_IO);
	if (!d || !q) {
		printf("PANDORA USB: DWC3 not mapped\n");
		return;
	}
	pu_dwc3(d, q, "at start");
	delay(100000);
	pu_dwc3(d, q, "100 ms later");
	ml_io_unmap(d, 0x1000);
	ml_io_unmap(q, 0x1000);
}

#define PU_PERIOD_MS 5u
#define PU_CHUNK     4096u

/* Who works on the console ring's bytes: the serial thread during a pass, pandora_usb_final once the
 * reset is coming (the thread then stops). */
#define PU_IDLE   0u
#define PU_THREAD 1u
#define PU_FINAL  2u

static int pu_level;
static int pu_phy_pass;      /* pandora_usb_phy=1: the read-only PHY / role / PD pass, default off */
static int pu_wait;
static uint32_t pu_state, pu_up;
static char pu_chunk[PU_CHUNK];
static uint64_t pu_pos;
static uint32_t pu_held, pu_sent;    /* chunk bytes not yet taken by the module; of them already taken */

static void
pu_sleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&pu_wait, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
pu_status(const char *when)
{
	struct qcom_usb_debug_status s;
	qcom_usb_debug_get_status(&s);
	printf("PANDORA USB: serial %s: phase %u error %u core %d qcom %u restore %u dma mode %u lease %u at %#llx "
	    "width %u hwparams0 %#x; domain result %u stage %u bank %u slot %u; vbus %u/%#x phy %u/%u; polls %u, "
	    "dropped %llu, io failed %u\n", when, s.phase, s.error, s.core_result, s.qcom_result, s.restore_result,
	    s.dma_mode, s.dma_lease_active, s.dma_pa, s.dma_address_width, s.hwparams0, s.domain_result,
	    s.domain_stage, s.domain_bank, s.domain_slot, s.vbus_owned, s.vbus_saved, s.phy_owned, s.phy_applied,
	    s.poll_calls, s.dropped, s.io_failed);
}

/* The serial console: started and served from the boot CPU. */
static void
pu_serial(void)
{
	(void)thread_bind(cpu_to_processor(master_cpu));
	thread_block(THREAD_CONTINUE_NULL);
	if (cpu_number() != master_cpu) {
		printf("PANDORA USB: serial not started: not on the boot CPU\n");
		return;
	}
	/* the module masks interrupts while it works; its windows are mapped first, here */
	if (!qcom_usb_debug_map()) {
		printf("PANDORA USB: serial not started: register windows not mapped\n");
		return;
	}
	const int up = qcom_usb_debug_init();
	pu_status(up ? "started" : "refused");
	if (!up) {
		return;
	}
	__atomic_store_n(&pu_up, 1u, __ATOMIC_RELEASE);
	for (unsigned n = 1;; n++) {
		uint32_t idle = PU_IDLE;
		if (!__atomic_compare_exchange_n(&pu_state, &idle, PU_THREAD, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
			for (;;) {    /* pandora_usb_final took over; the reset follows */
				pu_sleep(1000);
			}
		}
		if (pu_held == pu_sent) {
			pu_held = sweet_log_copy(&pu_pos, pu_chunk, PU_CHUNK);
			pu_sent = 0;
		}
		if (pu_held > pu_sent) {
			pu_sent += (uint32_t)qcom_usb_debug_write(pu_chunk + pu_sent, pu_held - pu_sent);
		}
		qcom_usb_debug_poll();
		__atomic_store_n(&pu_state, PU_IDLE, __ATOMIC_RELEASE);
		if (n == 2000 || n % 60000 == 0) {    /* 10 s after the start, then every 5 min */
			pu_status("running");
		}
		pu_sleep(PU_PERIOD_MS);
	}
}

/*
 * Right before the phone resets (sweet_ufs.c pl_psci_reset: a panic, a restart, pandora_reboot_after),
 * the bytes the serial thread has not handed over yet: usb4 lost the last two lines, written 1 ms before
 * the reset. The thread is stopped once its current pass ends (up to 20 ms; a panic can have stopped it
 * inside one, and then only what the module already holds goes out), the rest of the console ring goes
 * into the module, and the module sends it from this CPU (qcom_usb_debug_final_drain), for at most 2 s:
 * as long as a reset waits when nothing on the PC reads the port. The rest is every byte taken, stored
 * or not (sweet_log_copy_any): a CPU a panic stopped inside sweet_log_put must not hold back the lines
 * after it.
 */
void pandora_usb_final(void);
void
pandora_usb_final(void)
{
	if (!__atomic_load_n(&pu_up, __ATOMIC_ACQUIRE)) {
		return;
	}
	bool owned = false;
	for (unsigned i = 0; i < 200 && !owned; i++) {
		uint32_t idle = PU_IDLE;
		owned = __atomic_compare_exchange_n(&pu_state, &idle, PU_FINAL, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
		if (!owned) {
			delay(100);
		}
	}
	if (owned) {
		for (unsigned k = 0; k < 2048; k++) {    /* 8 MiB at most, twice the ring */
			if (pu_held == pu_sent) {
				pu_held = sweet_log_copy_any(&pu_pos, pu_chunk, PU_CHUNK);
				pu_sent = 0;
				if (!pu_held) {
					break;
				}
			}
			const size_t took = qcom_usb_debug_write(pu_chunk + pu_sent, pu_held - pu_sent);
			if (!took) {
				break;
			}
			pu_sent += (uint32_t)took;
		}
	}
	(void)qcom_usb_debug_final_drain(2000);
}

static void
pu_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	/* Before the console path, which never returns from pu_serial. Read only, so it cannot
	 * disturb the state the serial module goes on to take over. */
	if (pu_phy_pass) {
		pu_extras();
	}
	if (pu_level >= 2) {
		pu_serial();
	} else {
		pu_probe();
	}
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
pandora_usb_init(void)
{
	(void)PE_parse_boot_argn("pandora_usb", &pu_level, sizeof(pu_level));
	(void)PE_parse_boot_argn("pandora_usb_phy", &pu_phy_pass, sizeof(pu_phy_pass));
	if (pu_phy_pass && pu_level < 1) {
		/* the pass needs the survey's core it is run from: pandora_usb=1 or 2 as well */
		pu_level = 1;
	}
	if (!pu_level) {
		return;
	}
	thread_t thread;
	if (kernel_thread_start(pu_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* PANDORA */
