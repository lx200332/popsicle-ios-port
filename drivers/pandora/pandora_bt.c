/*
 * PANDORA Bluetooth: QUPv3 wrapper 3 SE5 UART (0x01994000, Android hsuart0) to the
 * Ganges BTFM combo. Probe-first: every register this driver touches is printed at
 * least once, so a single boot answers what static evidence cannot.
 *
 * Every hardware fact below is cited to the phone's own extraction
 * (<BACKUP>\01-手机提取), file by file.
 *
 * DT (02-devicetree/live-fdt.dtb, node /soc/qcom,qupv3_3_geni_se@19c0000/qcom,qup_uart@1994000):
 *   compatible          = "qcom,msm-geni-serial-hs"
 *   reg                 = <0x01994000 0x4000>              (reg-names "se_phys")
 *   interrupts-extended = <&intc 0 0x24b 4 &tlmm 0x1b 4>   -> GIC SPI 0x24b = 587, GPIO 27
 *   qcom,wakeup-byte    = 0xfd                              (BT WAKE byte for H4)
 *   status              = "ok"
 *   aliases/hsuart0     -> this node
 *   pinctrl-0..3        = qupv3_se18_default_{cts,rts,tx,rx}  (gpio24-27, func "gpio")
 *                         qupv3_se18_{cts,rts,tx,rx}_active   (gpio24-27, func qup3_se5_l0..l3)
 *   .../qupv3_se18_tx/mux          { pins="gpio26" function="qup3_se5_l2" }
 *   .../qupv3_se18_rx_active/mux   { pins="gpio27" function="qup3_se5_l3" }
 *   SE5 = wrapper 3 base 0x19c0000 ... child at 0x1994000, i.e. inside wrapper 3.
 *
 * DT (/soc/bt_peach, compatible "qcom,peach-bt"):
 *   qcom,bt-sw-ctrl-gpio = <&tlmm 18 0>
 *   qcom,bt-fmd-clk-gpio = <&pmh0104_gpios 6 0>   (pmh0104@9/pinctrl@8800/bt_fmd_clk_en, pins "gpio6")
 *   qcom,wl-reset-gpio   = <&tlmm 16 0>
 *   pinctrl-0            = pmh0104@9/pinctrl@8800/bt_uwb_en   (pins "gpio5")
 * DT (/soc/qcom,cnss-peach@b0000000):
 *   qcom,bt-en-gpio      = <&pmh0104_gpios 5 0>   -> PMIC GPIO 5, not a TLMM pin
 *
 * Android dmesg (10-props-logs/dmesg-full.txt) shows how BT is powered there:
 *   "BTON:Turn Bt OFF asserting BT_EN to low" / "BTON: WLAN ON Asserting BT_EN to high"
 *   "bt_configure_gpios: bt_gpio= 834 on: 1"      (that PMIC gpio's Linux global number)
 *   "bt_ioctl: unified Current SOC Version : 40210200"
 * props (10-props-logs/getprop-all.txt):
 *   persist.vendor.qcom.bluetooth.soc = ganges   -> firmware is gngbtfw20.*
 *   ro.hardware.wlan.chip            = qca_cld3_peach_v2
 *
 * Firmware (11-other-partitions/bluetooth_a.img, FAT12 8 MiB, directory /image):
 *   gngbtfw20.ver = BTFW.GANGES.2.0.1-00363-PATCHZ-1.164416.13_DEF
 *   gngbtfw20.tlv = 318,736 B   (TLV container)
 *   gngbtfw20.mbn = 331,876 B   (ELF32 ARM, entry 0x3001c3a0; same image, unpacked)
 *   gngbtnv20.bin = NVM template, gngbcscal20.bin = calibration
 *
 * Boot-arg pandora_bt (bit mask, default 0 = off):
 *   bit 0  probe: mux the four UART pins, vote wrapper 3 clocks, dump GCC + SE registers
 *   bit 1  also move sw_ctrl and report the BT_EN path (PMIC, not driven from here)
 *   bit 2  also bring the UART up in FIFO mode and read the firmware's HCI version
 *   bit 3  reserved: upload gngbtfw20 and open H4 (needs the blob inside the pack)
 * pandora_btslow=1 paces the register steps by 20 ms (default 0).
 * pandora_btse=N sets the per-SE GCC stride (default 0x40).
 *
 * THE ONE UNKNOWN, stated plainly: the GCC offset of wrapper 3 SE5's branch clock and
 * RCG is not in the extraction. What this tree already drives successfully on the same
 * board pins the pattern down to the wrapper level:
 *   pandora_touch.c: wrapper 4 SE0  branch 0xa9004  RCG 0xa9014  vote bits 22-26
 *   pandora_amp.c  : wrapper 3 SE0  branch 0xa8004  RCG 0xa8014  vote bits 8-12
 * so the wrapper stride is 0x1000 and SE0 sits at +0x4 (branch) / +0x14 (RCG). The SE
 * stride inside a wrapper is not derivable from that, so it is a boot-arg (default
 * 0x40, the value every recent Qualcomm GCC uses) and bit 0 scans the whole window and
 * prints every non-zero word, so one boot settles it.
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

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern int pandora_fw_read(uint64_t off, void *dst, uint64_t len);   /* bsd/dev/sweet_ufs.c */
void pandora_bt_init(void);

static int bt_chan;     /* assert_wait_deadline's event, same shape pandora_touch.c:162 uses */

/* ---- base addresses (DT, see header) ------------------------------------------- */
#define BT_SE_PA        0x01994000u    /* qup_uart@1994000            */
#define BT_SE_SIZE      0x4000u
#define BT_GCC_PA       0x00100000u    /* clock-controller@100000     */
#define BT_GCC_SIZE     0x1f0000u
#define BT_TLMM_PA      0x0f100000u    /* pinctrl@f000000             */
#define BT_GPIO(n)      ((uint32_t)(n) * 0x1000u)

/* ---- GCC, wrapper 3 (pandora_amp.c:8 and :279) -------------------------------- */
#define GCC_VOTE        0x52018u
#define GCC_VOTE_BITS   (0x1fu << 8)
#define BT_SE0_BRANCH   0xa8004u
#define BT_SE0_RCG      0xa8014u
#define BT_SE_INDEX     5u             /* qup_uart@1994000 is wrapper 3 SE5 */

/* ---- TLMM pins (DT pinctrl groups quoted in the header) ----------------------- */
#define BT_PIN_CTS      24u
#define BT_PIN_RTS      25u
#define BT_PIN_TX       26u
#define BT_PIN_RX       27u
#define BT_PIN_SWCTRL   18u            /* bt_peach qcom,bt-sw-ctrl-gpio */
#define BT_FUNC_UART    1u             /* qup3_se5_l0..l3 */
#define BT_FUNC_GPIO    0u

/* ---- GENI SE registers (Linux qcom-geni-se.h, msm_geni_serial.c) --------------- */
#define SE_GENI_STATUS           0x040u
#define SE_GENI_M_CMD0           0x600u
#define SE_GENI_M_CMD_CTRL       0x604u
#define SE_GENI_M_IRQ_STATUS     0x610u
#define SE_GENI_M_IRQ_EN         0x614u
#define SE_GENI_M_IRQ_CLEAR      0x618u
#define SE_GENI_S_CMD0           0x630u
#define SE_GENI_S_IRQ_STATUS     0x640u
#define SE_GENI_S_IRQ_EN         0x644u
#define SE_GENI_S_IRQ_CLEAR      0x648u
#define SE_GENI_TX_FIFO          0x700u
#define SE_GENI_RX_FIFO          0x780u
#define SE_GENI_TX_FIFO_STATUS   0x800u
#define SE_GENI_RX_FIFO_STATUS   0x804u
#define SE_GENI_IRQ_EN           0xe1cu
#define SE_FORCE_DEFAULT         0x020u
#define SE_OUTPUT_CTRL           0x024u
#define SE_CGC_CTRL              0x028u
#define SE_IF_DISABLE            0x064u
#define SE_FW_REV                0x068u
#define SE_CLK_SEL               0x07cu
#define SE_DMA_MODE              0x258u
#define SE_HW_PARAM0             0xe24u
#define SE_HW_PARAM1             0xe28u

#define GENI_PROTO_UART    0x3u
#define BT_BAUD            115200u
#define BT_CLK_HZ          19200000u

#define H4_CMD             0x01u
#define H4_EVT             0x04u
#define OGF_INFO           0x04u
#define OCF_READ_VERSION   0x0001u
#define OCF_READ_BDADDR    0x0009u

static struct {
	vm_offset_t gcc, se, tlmm;
	unsigned mask, mode;
	uint32_t se_stride;
} bt;

static void
bt_sleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&bt_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
bt_step(const char *what, uint32_t pa)
{
	if (bt.mode) {
		printf("PANDORA BT: next: first access to %s at %#x\n", what, pa);
		bt_sleep(20);
	}
}

static uint32_t
bt_rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static void
bt_wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* TLMM CTL: pull [1:0] (1 down, 3 up), func [5:2], drive [8:6] = mA/2-1, OE [9],
 * EGPIO_PRESENT [11], EGPIO_ENABLE [12]; same encoding as pandora_touch.c:220+. */
static void
bt_gpio(unsigned n, unsigned func, unsigned pull, unsigned ma, int out)
{
	if (out >= 0) {
		bt_wr(bt.tlmm, BT_GPIO(n) + 4, out ? 2u : 0u);
	}
	uint32_t ctl = bt_rd(bt.tlmm, BT_GPIO(n)) & ~0x3ffu;
	ctl |= pull | (func << 2) | ((ma / 2 - 1) << 6) | (out >= 0 ? 0x200u : 0u);
	if (ctl & (1u << 11)) {
		ctl |= 1u << 12;
	}
	bt_wr(bt.tlmm, BT_GPIO(n), ctl);
}

static void
bt_pins_dump(const char *when)
{
	static const uint8_t pins[] = {BT_PIN_CTS, BT_PIN_RTS, BT_PIN_TX, BT_PIN_RX, BT_PIN_SWCTRL};
	printf("PANDORA BT: pins %s:", when);
	for (unsigned i = 0; i < sizeof(pins); i++) {
		printf(" %u=%#x/%u", pins[i], bt_rd(bt.tlmm, BT_GPIO(pins[i])),
		    bt_rd(bt.tlmm, BT_GPIO(pins[i]) + 4) & 3u);
	}
	printf("\n");
}

static uint32_t
bt_branch(void)
{
	return BT_SE0_BRANCH + bt.se_stride * BT_SE_INDEX;
}

static uint32_t
bt_rcg(void)
{
	return BT_SE0_RCG + bt.se_stride * BT_SE_INDEX;
}

static bool
bt_on(uint32_t cbcr)
{
	return !(bt_rd(bt.gcc, cbcr) & 0x80000000u);
}

static void
bt_clock_dump(const char *when)
{
	printf("PANDORA BT: gcc %s: vote=%#x agg0=%#x agg8=%#x | se0 branch=%#x/%#x rcg=%#x/%#x "
	    "| se%u branch %#x=%#x/%#x rcg %#x=%#x/%#x (stride %#x)\n", when,
	    bt_rd(bt.gcc, GCC_VOTE), bt_rd(bt.gcc, 0x52000u), bt_rd(bt.gcc, 0x52008u),
	    bt_rd(bt.gcc, BT_SE0_BRANCH), bt_rd(bt.gcc, BT_SE0_BRANCH + 4),
	    bt_rd(bt.gcc, BT_SE0_RCG), bt_rd(bt.gcc, BT_SE0_RCG + 4),
	    BT_SE_INDEX, bt_branch(), bt_rd(bt.gcc, bt_branch()), bt_rd(bt.gcc, bt_branch() + 4),
	    bt_rcg(), bt_rd(bt.gcc, bt_rcg()), bt_rd(bt.gcc, bt_rcg() + 4),
	    bt.se_stride);
}

/* The SE stride is the single unknown; print every live word of the wrapper window. */
static void
bt_clock_scan(void)
{
	unsigned shown = 0;
	printf("PANDORA BT: gcc scan 0xa8000..0xa8400 (bit31 set = clock off):\n");
	for (uint32_t o = 0xa8000; o < 0xa8400; o += 0x4) {
		uint32_t v = bt_rd(bt.gcc, o);
		if (v && v != 0xffffffffu) {
			printf("   %#06x = %#010x%s\n", o, v, (v & 0x80000000u) ? "  off" : "  ON");
			if (++shown >= 48) {
				printf("   ... (48 words shown)\n");
				break;
			}
		}
	}
	if (!shown) {
		printf("   (all words zero)\n");
	}
}

static int
bt_clocks(void)
{
	bt_wr(bt.gcc, GCC_VOTE, bt_rd(bt.gcc, GCC_VOTE) | GCC_VOTE_BITS);
	uint32_t b = bt_branch();
	for (unsigned t = 0; t < 10000 && !bt_on(b); t++) {
		delay(1);
	}
	if (!bt_on(b)) {
		printf("PANDORA BT: wrapper 3 SE%u branch %#x is still off\n", BT_SE_INDEX, b);
		return EIO;
	}
	/* SE5 source: XO undivided, same recipe pandora_touch.c:283-300 applies to SE0. */
	uint32_t r = bt_rcg();
	unsigned level = 0;
	if (bt_rd(bt.gcc, r + 0x14) & 1u) {
		while (level < 8 && (bt_rd(bt.gcc, r + 0x1c + 4 * level) & 0x371fu) != 1u) {
			level++;
		}
		if (level == 8) {
			printf("PANDORA BT: no XO DFS level in %#x\n", r);
			return EIO;
		}
	} else {
		uint32_t cfg = (bt_rd(bt.gcc, r + 4) & ~(0x371fu | (1u << 20))) | 1u;
		bt_wr(bt.gcc, r + 4, cfg);
		bt_wr(bt.gcc, r, bt_rd(bt.gcc, r) | 1u);
		for (unsigned t = 0; t < 5000 && (bt_rd(bt.gcc, r) & 1u); t++) {
			delay(1);
		}
		if (bt_rd(bt.gcc, r) & 1u) {
			printf("PANDORA BT: RCG %#x update timeout\n", r);
			return EIO;
		}
	}
	return 0;
}

static void
bt_se_dump(const char *when)
{
	printf("PANDORA BT: se %s: fw=%#x if_disable=%#x status=%#x hw0=%#x hw1=%#x "
	    "clk_sel=%#x dma=%#x irq_en=%#x m_irq=%#x m_irq_en=%#x s_irq=%#x s_irq_en=%#x "
	    "tx_fifo=%#x rx_fifo=%#x\n", when,
	    bt_rd(bt.se, SE_FW_REV), bt_rd(bt.se, SE_IF_DISABLE), bt_rd(bt.se, SE_GENI_STATUS),
	    bt_rd(bt.se, SE_HW_PARAM0), bt_rd(bt.se, SE_HW_PARAM1), bt_rd(bt.se, SE_CLK_SEL),
	    bt_rd(bt.se, SE_DMA_MODE), bt_rd(bt.se, SE_GENI_IRQ_EN),
	    bt_rd(bt.se, SE_GENI_M_IRQ_STATUS), bt_rd(bt.se, SE_GENI_M_IRQ_EN),
	    bt_rd(bt.se, SE_GENI_S_IRQ_STATUS), bt_rd(bt.se, SE_GENI_S_IRQ_EN),
	    bt_rd(bt.se, SE_GENI_TX_FIFO_STATUS), bt_rd(bt.se, SE_GENI_RX_FIFO_STATUS));
}

static int
bt_se_probe(void)
{
	uint32_t fw = bt_rd(bt.se, SE_FW_REV);
	uint32_t proto = fw & 0xffu;
	printf("PANDORA BT: SE firmware %#x protocol %u (%s), want %u (UART)\n",
	    fw, proto, proto == GENI_PROTO_UART ? "UART" :
	    proto == 1 ? "SPI" : proto == 2 ? "I2C" : "?", GENI_PROTO_UART);
	if (proto != GENI_PROTO_UART) {
		return ENODEV;
	}
	if (bt_rd(bt.se, SE_IF_DISABLE) & 1u) {
		printf("PANDORA BT: FIFO interface disabled (if_disable bit 0); "
		    "would need GPI DMA, not attempted\n");
		return ENOTSUP;
	}
	bt_wr(bt.se, SE_FORCE_DEFAULT, 0);
	bt_wr(bt.se, SE_OUTPUT_CTRL, bt_rd(bt.se, SE_OUTPUT_CTRL) | 1u);
	bt_wr(bt.se, SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	bt_wr(bt.se, SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
	bt_wr(bt.se, SE_GENI_M_IRQ_EN, 0);
	bt_wr(bt.se, SE_GENI_S_IRQ_EN, 0);
	bt_wr(bt.se, SE_GENI_IRQ_EN, 0);
	/* 16x oversampling as in msm_geni_serial_setup: 19.2 MHz / (16 * 115200) = 10 */
	uint32_t div = BT_CLK_HZ / (16u * BT_BAUD);
	bt_wr(bt.se, SE_CLK_SEL, div & 0x3ffu);
	printf("PANDORA BT: clk_sel <- %u (XO/(16*%u))\n", div & 0x3ffu, BT_BAUD);
	bt_se_dump("after setup");
	return 0;
}

static int
bt_se_putc(uint8_t c)
{
	for (unsigned t = 0; t < 10000; t++) {
		if (bt_rd(bt.se, SE_GENI_TX_FIFO_STATUS) & 1u) {
			break;
		}
		delay(1);
	}
	if (!(bt_rd(bt.se, SE_GENI_TX_FIFO_STATUS) & 1u)) {
		return EBUSY;
	}
	bt_wr(bt.se, SE_GENI_TX_FIFO, c);
	return 0;
}

static int
bt_se_write(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		int e = bt_se_putc(p[i]);
		if (e) {
			printf("PANDORA BT: tx fifo stalled after %zu of %zu\n", i, n);
			return e;
		}
	}
	return 0;
}

static int
bt_hci_cmd(uint16_t op, const uint8_t *par, uint8_t plen, uint8_t *ev, size_t evlen)
{
	uint8_t h4[4];
	h4[0] = H4_CMD;
	h4[1] = (uint8_t)(op & 0xffu);
	h4[2] = (uint8_t)(op >> 8);
	h4[3] = plen;
	int e = bt_se_write(h4, 4);
	if (!e && plen) {
		e = bt_se_write(par, plen);
	}
	if (e) {
		return e;
	}
	size_t got = 0;
	unsigned idle = 0;
	while (got < evlen && idle < 5000) {
		if (bt_rd(bt.se, SE_GENI_RX_FIFO_STATUS) & 0xffffu) {
			ev[got++] = (uint8_t)(bt_rd(bt.se, SE_GENI_RX_FIFO) & 0xffu);
			idle = 0;
			continue;
		}
		delay(1);
		idle++;
	}
	printf("PANDORA BT: hci op %#06x -> %zu bytes:", op, got);
	for (size_t i = 0; i < got && i < 24; i++) {
		printf(" %02x", ev[i]);
	}
	printf("\n");
	return got ? 0 : ETIMEDOUT;
}

static int
bt_fw_probe(void)
{
	static uint8_t dir[4096];
	int e = pandora_fw_read(0, dir, sizeof(dir));
	if (e) {
		printf("PANDORA BT: firmware pack read failed %d (no pack?)\n", e);
		return e;
	}
	if (memcmp(dir, "PANDFW01", 8)) {
		printf("PANDORA BT: firmware pack has no header\n");
		return EINVAL;
	}
	uint32_t count = *(uint32_t *)(void *)(dir + 8);
	printf("PANDORA BT: firmware pack: %u files\n", count);
	static const char *want[] = {"gngbtfw20.tlv", "gngbtfw20.mbn", "gngbtnv20.bin",
		"gngbcscal20.bin"};
	unsigned found = 0;
	for (unsigned i = 0; i < count && i < 63u; i++) {
		const uint8_t *ent = dir + 16 + i * 64;
		char name[49];
		memcpy(name, ent, 48);
		name[48] = 0;
		for (unsigned w = 0; w < sizeof(want) / sizeof(want[0]); w++) {
			if (!strcmp(name, want[w])) {
				uint64_t off = *(uint64_t *)(const void *)(ent + 48);
				uint64_t size = *(uint64_t *)(const void *)(ent + 56);
				printf("PANDORA BT: %s at %#llx, %llu bytes\n", name, off, size);
				found++;
			}
		}
	}
	if (!found) {
		printf("PANDORA BT: no ganges firmware in the pack "
		    "(add gngbtfw20.* to files.txt and repack)\n");
	}
	return 0;
}

static void
bt_thread(void *arg __unused, wait_result_t wr __unused)
{
	printf("PANDORA BT: start, mask %#x (bit0 probe, bit1 power, bit2 uart)\n", bt.mask);
	bt_fw_probe();
	bt.gcc = ml_io_map(BT_GCC_PA, BT_GCC_SIZE);
	bt.tlmm = ml_io_map(BT_TLMM_PA, 0x300000);
	bt.se = ml_io_map(BT_SE_PA, BT_SE_SIZE);
	if (!bt.gcc || !bt.tlmm || !bt.se) {
		printf("PANDORA BT: mapping failed gcc %p tlmm %p se %p\n",
		    (void *)bt.gcc, (void *)bt.tlmm, (void *)bt.se);
		goto out;
	}
	bt_step("GCC", BT_GCC_PA + GCC_VOTE);
	bt_pins_dump("before");
	bt_clock_dump("before");
	if (bt.mode) {
		bt_clock_scan();
	}
	bt_gpio(BT_PIN_SWCTRL, BT_FUNC_GPIO, 1, 2, 1);
	bt_gpio(BT_PIN_TX, BT_FUNC_UART, 1, 2, 1);
	bt_gpio(BT_PIN_RX, BT_FUNC_UART, 3, 2, -1);
	bt_gpio(BT_PIN_CTS, BT_FUNC_UART, 1, 2, -1);
	bt_gpio(BT_PIN_RTS, BT_FUNC_UART, 1, 2, 1);
	bt_pins_dump("after mux");
	bt_step("wrapper 3 clocks", BT_GCC_PA + GCC_VOTE);
	int e = bt_clocks();
	bt_clock_dump("after vote");
	if (e) {
		printf("PANDORA BT: clock bring-up failed %d; stop\n", e);
		goto out;
	}
	bt_step("SE5 registers", BT_SE_PA + SE_FW_REV);
	bt_se_dump("before");
	if (!(bt.mask & 2u)) {
		printf("PANDORA BT: bit1 clear, not powering BT\n");
		goto out;
	}
	/* BT_EN is PMIC GPIO 5 on pmh0104@9, reachable over SPMI only; reported, not driven. */
	bt_gpio(BT_PIN_SWCTRL, BT_FUNC_GPIO, 1, 2, 0);
	printf("PANDORA BT: BT_EN is pmh0104@9 GPIO 5 (SPMI, not driven here); sw_ctrl low\n");
	bt_sleep(50);
	if (!(bt.mask & 4u)) {
		printf("PANDORA BT: bit2 clear, not bringing the UART up\n");
		goto out;
	}
	e = bt_se_probe();
	if (e) {
		printf("PANDORA BT: SE probe failed %d\n", e);
		goto out;
	}
	uint8_t ev[64];
	if (!bt_hci_cmd((OGF_INFO << 10) | OCF_READ_VERSION, NULL, 0, ev, 20)) {
		if (ev[0] == H4_EVT && ev[1] == 0x0eu && ev[2] >= 0x0au) {
			printf("PANDORA BT: HCI %u.%u LMP %#x manufacturer %#x\n",
			    ev[6], ev[8], ev[7], ev[10] | (ev[11] << 8));
		}
	}
	(void)bt_hci_cmd((OGF_INFO << 10) | OCF_READ_BDADDR, NULL, 0, ev, 12);
out:
	printf("PANDORA BT: done\n");
	(void)thread_terminate(current_thread());
}

void
pandora_bt_init(void)
{
	unsigned on = 0, slow = 0, stride = 0x40;
	if (!PE_parse_boot_argn("pandora_bt", &on, sizeof(on)) || !on) {
		return;
	}
	(void)PE_parse_boot_argn("pandora_btslow", &slow, sizeof(slow));
	if (PE_parse_boot_argn("pandora_btse", &stride, sizeof(stride))) {
		if (stride < 4 || stride > 0x400) {
			stride = 0x40;
		}
	}
	bt.mask = on;
	bt.mode = slow;
	bt.se_stride = stride;
	thread_t th;
	if (kernel_thread_start(bt_thread, NULL, &th) == KERN_SUCCESS) {
		thread_deallocate(th);
	}
}
#endif /* PANDORA */
