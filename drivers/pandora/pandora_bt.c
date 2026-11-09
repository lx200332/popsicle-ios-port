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
 *   bit 3  also load gngbtfw20.tlv out of the pack, print its TLV table, open H4
 *          and run HCI_Reset / Read_Local_Version / Read_BD_ADDR. The vendor
 *          download is bt_fw_download(), which does not guess an opcode: it says
 *          what evidence is missing and returns ENOTSUP.
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
#include <vm/pmap.h>	/* VM_WIMG_IO for ml_io_map_unmappable */
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/kalloc.h>	/* kalloc_data/kfree_data for the bit 3 firmware blob */
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, uint32_t);
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
#define H4_ACL             0x02u
#define H4_SCO             0x03u
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

/* ---- bit 3: the firmware blob and H4 -----------------------------------------
 *
 * The pack carries gngbtfw20.tlv, and the blob was measured rather than assumed
 * from a Linux header: the container is ONE TLV -- a u32 little-endian at offset
 * 0, type in the low byte (1, patch), length in the upper 24 bits (320,724), and
 * 4 + 320,724 == the file's 320,728 bytes. There is no chain to walk, so the
 * table printed below is that one row plus the segment plan a download would cut
 * it into.
 *
 * Everything here that needs no vendor opcode is real: the blob is read through
 * pandora_fw_read(), its header decoded and printed, and H4 is spoken with core
 * spec commands. The vendor download is bt_fw_download(), which returns ENOTSUP
 * and names the missing evidence. Nothing in this file invents an opcode.
 */
#define BT_FW_MAX          0x100000u   /* 1 MiB, the cap pandora_amp.c:379 applies */
#define BT_TLV_HDR         4u          /* u32 LE: type [7:0], length [31:8]        */
#define BT_TLV_TYPE_PATCH  0x01u
#define BT_TLV_PATCH_HDR   36u         /* payload bytes this header really occupies */
#define BT_TLV_SEG_DATA    243u        /* btqca.h MAX_SIZE_PER_TLV_SEGMENT          */
#define BT_TLV_SEG_HDR     2u          /* LE16 segment id in front of each chunk    */

/* One file out of the pinned pack into a kalloc_data buffer; NULL and *out_len 0
 * when the pack or the entry is missing. Same directory walk as pandora_amp.c:363. */
static uint8_t *
bt_fw_load(const char *name, uint32_t *out_len)
{
	static uint8_t dir[4096];
	uint8_t *buf;
	uint32_t count = 0;
	uint32_t i;
	uint64_t off = 0;
	uint64_t len = 0;
	int e;

	*out_len = 0;
	e = pandora_fw_read(0, dir, sizeof(dir));
	if (e) {
		printf("PANDORA BT: fw: pack read failed %d\n", e);
		return NULL;
	}
	if (memcmp(dir, "PANDFW01", 8)) {
		printf("PANDORA BT: fw: pack has no header\n");
		return NULL;
	}
	memcpy(&count, dir + 8, sizeof(count));
	for (i = 0; i < count && i < 63u; i++) {
		const uint8_t *ent = dir + 16 + i * 64;
		char ename[49];

		memcpy(ename, ent, 48);
		ename[48] = 0;
		if (strcmp(ename, name)) {
			continue;
		}
		memcpy(&off, ent + 48, sizeof(off));
		memcpy(&len, ent + 56, sizeof(len));
		if (!len || len > BT_FW_MAX) {
			printf("PANDORA BT: fw: %s has unusable size %llu\n", name, len);
			return NULL;
		}
		buf = kalloc_data((vm_size_t)len, Z_WAITOK);
		if (!buf) {
			printf("PANDORA BT: fw: cannot allocate %llu bytes\n", len);
			return NULL;
		}
		e = pandora_fw_read(off, buf, len);
		if (e) {
			printf("PANDORA BT: fw: %s at %#llx read failed %d\n", name, off, e);
			kfree_data(buf, (vm_size_t)len);
			return NULL;
		}
		printf("PANDORA BT: fw: %s %llu bytes at %#llx\n", name, len, off);
		*out_len = (uint32_t)len;
		return buf;
	}
	printf("PANDORA BT: fw: %s not in the pack\n", name);
	return NULL;
}

/* The blob carries its own version string; search for it instead of hard-coding
 * the offset this one sample happens to use (+0x710 of 320,728). */
static void
bt_fw_version(const uint8_t *buf, uint32_t len)
{
	uint32_t at = len;
	uint32_t n = 0;
	uint32_t i;

	for (i = 0; i + 5u <= len; i++) {
		if (!memcmp(buf + i, "BTFW.", 5)) {
			at = i;
			break;
		}
	}
	if (at == len) {
		printf("PANDORA BT: fw: no BTFW. string in %u bytes\n", len);
		return;
	}
	while (at + n < len && n < 64u && buf[at + n] >= 0x20u && buf[at + n] < 0x7fu) {
		n++;
	}
	printf("PANDORA BT: fw version at +%u: %.*s\n", at, (int)n, (const char *)(buf + at));
}

/* Patch payload: the fields a segment sender would need, read straight out of the
 * blob. Where the layout disagrees with Linux's struct tlv_type_patch the
 * disagreement is reported rather than papered over. */
static void
bt_fw_patch(const uint8_t *pay, uint32_t paylen)
{
	uint32_t total_size = 0;
	uint32_t data = 0;
	uint32_t entry = 0;
	uint16_t product = 0;
	uint16_t rom = 0;
	uint16_t patch = 0;
	unsigned segs;
	unsigned i;

	if (paylen < BT_TLV_PATCH_HDR) {
		printf("PANDORA BT: fw: patch payload %u B is shorter than the %u B header\n",
		    paylen, BT_TLV_PATCH_HDR);
		return;
	}
	memcpy(&total_size, pay + 0, sizeof(total_size));
	memcpy(&data, pay + 4, sizeof(data));
	memcpy(&product, pay + 12, sizeof(product));
	memcpy(&rom, pay + 14, sizeof(rom));
	memcpy(&patch, pay + 16, sizeof(patch));
	memcpy(&entry, pay + 32, sizeof(entry));
	printf("PANDORA BT: fw: patch total %u data %u format %u signature %u download_mode %u "
	    "product %#x rom %#x patch %#x entry %#x\n", total_size, data, (unsigned)pay[8],
	    (unsigned)pay[9], (unsigned)pay[10], (unsigned)product, (unsigned)rom,
	    (unsigned)patch, entry);
	printf("PANDORA BT: fw: entry sits at payload+32; Linux's tlv_type_patch ends at +24, "
	    "so this header is 12 B wider -- vendor layout, no struct for it in the repo\n");
	if (data > paylen) {
		printf("PANDORA BT: fw: data_length %u exceeds the %u B payload\n", data, paylen);
		return;
	}
	segs = (data + BT_TLV_SEG_DATA - 1u) / BT_TLV_SEG_DATA;
	printf("PANDORA BT: fw: plan %u segments of %u B data + %u B id = %u B per command\n",
	    segs, BT_TLV_SEG_DATA, BT_TLV_SEG_HDR, BT_TLV_SEG_DATA + BT_TLV_SEG_HDR);
	for (i = 0; i < segs; i++) {
		uint32_t at = i * BT_TLV_SEG_DATA;
		uint32_t n = data - at;

		if (n > BT_TLV_SEG_DATA) {
			n = BT_TLV_SEG_DATA;
		}
		if (i < 3u || i + 1u == segs) {
			printf("PANDORA BT: fw:   seg %u at payload+%u length %u\n", i, at, n);
		} else if (i == 3u) {
			printf("PANDORA BT: fw:   ... %u rows elided ...\n", segs - 4u);
		}
	}
}

/* Walk the TLV chain and print tag / length / offset per row. For this blob that
 * is one row; the loop is a loop because the container is one, and a second TLV
 * showing up later must not be silently dropped. */
static int
bt_fw_parse(const uint8_t *buf, uint32_t len)
{
	uint32_t off = 0;
	unsigned rows = 0;

	printf("PANDORA BT: fw: TLV table over %u B (type = byte 0, length = u32 >> 8)\n", len);
	while (off + BT_TLV_HDR <= len) {
		uint32_t tl = 0;
		uint32_t type;
		uint32_t plen;

		memcpy(&tl, buf + off, sizeof(tl));
		type = tl & 0xffu;
		plen = tl >> 8;
		printf("PANDORA BT: fw:   row %u at +%u tag %u length %u\n", rows, off, type, plen);
		rows++;
		if (plen > len - off - BT_TLV_HDR) {
			printf("PANDORA BT: fw:   row %u runs past the blob; stop\n", rows);
			return EINVAL;
		}
		if (type == BT_TLV_TYPE_PATCH) {
			bt_fw_patch(buf + off + BT_TLV_HDR, plen);
		} else {
			printf("PANDORA BT: fw:   tag %u is not a patch; not decoded\n", type);
		}
		off += BT_TLV_HDR + plen;
	}
	if (!rows) {
		printf("PANDORA BT: fw: no TLV at all in %u B\n", len);
		return EINVAL;
	}
	printf("PANDORA BT: fw: %u TLV row(s), %u B left after the last one\n", rows, len - off);
	return 0;
}

/* The vendor download. A skeleton on purpose -- see the note. */
static int
bt_fw_download(const uint8_t *tlv, uint32_t len)
{
	/* A vendor download is a run of commands in the 0xFC00..0xFCFF range whose
	 * opcode, parameter layout and completion-event contract belong to the chip
	 * vendor. This repo has none of that for ganges, and each missing piece is
	 * load-bearing:
	 *
	 *   1. The only published transcription is Linux's QCA path (btqca.h:
	 *      EDL_PATCH_CMD_OPCODE 0xFC00, EDL_PATCH_TLV_REQ_CMD 0x1E,
	 *      MAX_SIZE_PER_TLV_SEGMENT 243), and it is a different SoC family:
	 *      nothing in this repo binds it to ganges. It is cited here, not sent.
	 *   2. This blob's download_mode is 3 and its entry word sits 12 B further
	 *      into the header than that struct describes, so the pack was not cut by
	 *      that tool either. Replaying that tool's sequence is a coin flip.
	 *   3. Whether this boot ROM takes H4 at all is unmeasured: the reset probe
	 *      above is the only data point there is, and it is one boot.
	 *
	 * A wrong vendor opcode can leave the part wedged until the PMIC cycles it,
	 * so the honest return is ENOTSUP plus the shape of the missing piece, not a
	 * guessed constant. */
	printf("PANDORA BT: fw download: not attempted; %u B staged, first words "
	    "%02x %02x %02x %02x\n", len, (unsigned)tlv[0], (unsigned)tlv[1],
	    (unsigned)tlv[2], (unsigned)tlv[3]);
	printf("PANDORA BT: fw download: missing 1/3 - the btsoc vendor opcode table for "
	    "ganges; no file in this repo names one\n");
	printf("PANDORA BT: fw download: missing 2/3 - what download_mode 3 means on this part "
	    "and which event closes a segment\n");
	printf("PANDORA BT: fw download: missing 3/3 - a second boot of the reset probe above, "
	    "to tell a ROM that speaks H4 from one that does not\n");
	return ENOTSUP;
}

/* H4 send: one type byte, then the packet. 0x01 command, 0x02 ACL, 0x04 event.
 * Kept apart from bt_hci_cmd() so bit 2's path stays byte-for-byte unchanged. */
static int
bt_h4_send(uint8_t type, const void *pkt, size_t len)
{
	int e = bt_se_putc(type);

	if (e) {
		return e;
	}
	return bt_se_write((const uint8_t *)pkt, len);
}

/* H4 receive: pull the RX FIFO one byte at a time until the packet the header
 * announces is complete, or the FIFO stays quiet for timeout_ms. Framing is the
 * core spec one: event 3 + plen, ACL 5 + handle/length, SCO 2 + length. */
static int
bt_h4_poll(uint8_t *buf, size_t cap, unsigned timeout_ms)
{
	size_t got = 0;
	size_t want = 0;
	unsigned idle = 0;

	while (got < cap && (!want || got < want)) {
		if (!(bt_rd(bt.se, SE_GENI_RX_FIFO_STATUS) & 0xffffu)) {
			if (idle++ >= timeout_ms) {
				break;
			}
			delay(1);
			continue;
		}
		buf[got++] = (uint8_t)(bt_rd(bt.se, SE_GENI_RX_FIFO) & 0xffu);
		idle = 0;
		if (got == 3 && buf[0] == H4_EVT) {
			want = 3u + (size_t)buf[2];
		} else if (got == 5 && buf[0] == H4_ACL) {
			want = 5u + (size_t)buf[3] + ((size_t)buf[4] << 8);
		} else if (got == 2 && buf[0] == H4_SCO) {
			want = 2u + (size_t)buf[1];
		}
		if (want > cap) {
			printf("PANDORA BT: h4: packet wants %llu B, buffer is %llu\n",
			    (unsigned long long)want, (unsigned long long)cap);
			return -EMSGSIZE;
		}
	}
	if (!got) {
		return -ETIMEDOUT;
	}
	if (want && got < want) {
		printf("PANDORA BT: h4: short packet %llu of %llu B\n",
		    (unsigned long long)got, (unsigned long long)want);
		return -ETIMEDOUT;
	}
	return (int)got;
}

/* One core spec command over H4, then the command complete it must answer with.
 * Returns the HCI status byte (0 = success), or a local errno. */
static int
bt_hci_one(uint16_t op, const uint8_t *par, uint8_t plen, const char *what,
    uint8_t *ev, size_t evcap)
{
	uint8_t cmd[3 + 255];
	unsigned back;
	int e;
	int n;
	int i;

	cmd[0] = (uint8_t)(op & 0xffu);
	cmd[1] = (uint8_t)(op >> 8);
	cmd[2] = plen;
	if (plen) {
		memcpy(cmd + 3, par, plen);
	}
	e = bt_h4_send(H4_CMD, cmd, 3u + (size_t)plen);
	if (e) {
		printf("PANDORA BT: h4 %s: send failed %d\n", what, e);
		return e;
	}
	n = bt_h4_poll(ev, evcap, 2000);
	if (n <= 0) {
		printf("PANDORA BT: h4 %s: no answer within 2000 ms\n", what);
		return ETIMEDOUT;
	}
	printf("PANDORA BT: h4 %s: op %#06x ->", what, op);
	for (i = 0; i < n && i < 24; i++) {
		printf(" %02x", (unsigned)ev[i]);
	}
	printf("\n");
	if (n < 7 || ev[0] != H4_EVT || ev[1] != 0x0eu) {
		printf("PANDORA BT: h4 %s: not a command complete\n", what);
		return EPROTO;
	}
	back = (unsigned)ev[4] | ((unsigned)ev[5] << 8);
	if (back != op) {
		printf("PANDORA BT: h4 %s: complete is for op %#06x\n", what, back);
		return EPROTO;
	}
	return (int)ev[6];
}

/* HCI_Reset, then the two reads the Android stack opens with. All three are core
 * spec opcodes: OGF 0x03 / OCF 0x0003 for the reset, and the same OGF 0x04 pair
 * bit 2 already uses. */
static int
bt_hci_init(void)
{
	uint8_t ev[80];
	int e;

	e = bt_hci_one(0x0c03u, NULL, 0, "reset", ev, sizeof(ev));
	printf("PANDORA BT: hci reset -> %d\n", e);
	if (e) {
		return e;
	}
	e = bt_hci_one((uint16_t)((OGF_INFO << 10) | OCF_READ_VERSION), NULL, 0,
	    "read local version", ev, sizeof(ev));
	if (e) {
		printf("PANDORA BT: read local version failed %d\n", e);
	} else if (ev[2] >= 9u) {
		printf("PANDORA BT: local hci %u lmp %#x manufacturer %#x subversion %#x\n",
		    (unsigned)ev[7], (unsigned)ev[10],
		    (unsigned)ev[11] | ((unsigned)ev[12] << 8),
		    (unsigned)ev[13] | ((unsigned)ev[14] << 8));
	}
	e = bt_hci_one((uint16_t)((OGF_INFO << 10) | OCF_READ_BDADDR), NULL, 0,
	    "read bd addr", ev, sizeof(ev));
	if (e) {
		printf("PANDORA BT: read bd addr failed %d\n", e);
	} else if (ev[2] >= 7u) {
		printf("PANDORA BT: bd addr %02x:%02x:%02x:%02x:%02x:%02x\n",
		    (unsigned)ev[12], (unsigned)ev[11], (unsigned)ev[10],
		    (unsigned)ev[9], (unsigned)ev[8], (unsigned)ev[7]);
	}
	return e;
}

/* bit 3's whole path, in the order the hardware wants it: the blob first, so a
 * missing blob is one line instead of a timeout, then the H4 probe, then the
 * download that cannot run yet. */
static int
bt_bit3(void)
{
	uint32_t len = 0;
	uint8_t *buf;
	int e;

	buf = bt_fw_load("gngbtfw20.tlv", &len);
	if (!buf) {
		printf("PANDORA BT: bit3: no gngbtfw20.tlv in the pack; firmware path stops\n");
		return ENOENT;
	}
	bt_fw_version(buf, len);
	e = bt_fw_parse(buf, len);
	(void)bt_hci_init();
	if (!e) {
		e = bt_fw_download(buf, len);
	}
	kfree_data(buf, (vm_size_t)len);
	return e;
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
				uint64_t off, size;

				memcpy(&off, ent + 48, sizeof(off));
				memcpy(&size, ent + 56, sizeof(size));
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
	bt.gcc = ml_io_map_unmappable(BT_GCC_PA, BT_GCC_SIZE, VM_WIMG_IO);
	bt.tlmm = ml_io_map_unmappable(BT_TLMM_PA, 0x300000, VM_WIMG_IO);
	bt.se = ml_io_map_unmappable(BT_SE_PA, BT_SE_SIZE, VM_WIMG_IO);
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
	if (bt.mask & 8u) {
		(void)bt_bit3();
	}
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
