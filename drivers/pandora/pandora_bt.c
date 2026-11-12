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
/* The bluetooth partition carries the ganges firmware as real files and the shared
 * FatVolume reader now accepts its FAT12 layout, so read it from there: the firmware is
 * already on the device, whereas the pinned pack has to be rebuilt and pushed by hand.
 * pandora_bt_fw=pack goes back to the pack. */
extern int pandora_fw_volume_size(int part, const char *path, uint64_t *size);
extern int pandora_fw_volume_read(int part, const char *path, uint64_t off, void *dst, uint64_t len);
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
/*
 * Bits 8.. of GCC_VOTE 0x52018 are one branch per SE. Measured: writing 0x1f00
 * (bits 8..12) started SE0 only -- SE0 CBCR went 0x88004000 -> 0x8004000 while
 * SE5 stayed at 0x88040000. SE5 is bit 13, so the mask has to follow BT_SE_INDEX.
 * The Android side agrees the branch is the only missing piece: debugfs shows
 * gcc_qupv3_wrap3_s5_clk_src already at 64 MHz, /dev/ttyHS0 exists, and the
 * branch enable_count is 0.
 */
#define GCC_VOTE_BITS   (((1u << (BT_SE_INDEX + 1u)) - 1u) << 8)
#define BT_SE_INDEX     5u             /* qup_uart@1994000 is wrapper 3 SE5 */
#define BT_SE0_BRANCH   0xa8004u
#define BT_SE0_RCG      0xa8014u
/*
 * Measured on the board (boot X3, pandora_btslow=1 dumped 0xa8000..0xa8c00 and every
 * CBCR read back off bit31): the wrapper spaces its SEs 0x13c apart.
 *   SE0 CBCR 0xa8004   SE1 0xa8140   SE2 0xa827c   SE3 0xa83b8   SE4 0xa84f4   SE5 0xa8630
 * 0xa8630 - 0xa8004 = 0x62c and 0x62c / 5 = 0x13c. SE5 therefore owns CBCR 0xa8630 and
 * RCG 0xa8640 -- not the 0xa8144/0xa8154 the old 0x40 stride produced (that pair is
 * SE1's), and not 0xa8644/0xa8654 either (0xa8644 is SE5's RCG-prep word, whose bit31
 * is already clear, so the branch probe used to pass on the wrong register and the RCG
 * write landed outside any clock).
 */
#define BT_SE_STRIDE    0x13cu

/* ---- TLMM pins (DT pinctrl groups quoted in the header) ----------------------- */
#define BT_PIN_CTS      24u
#define BT_PIN_RTS      25u
#define BT_PIN_TX       26u
#define BT_PIN_RX       27u
#define BT_PIN_SWCTRL   18u            /* bt_peach qcom,bt-sw-ctrl-gpio */
/* Per-pin funcs[] indices, not SoC function numbers. The phone's own
 * pinmux-functions table, walked in order, gives:
 *   gpio24 [gpio, qup3_se5_l0]                 -> 1
 *   gpio25 [gpio, qup3_se5_l1]                 -> 1
 *   gpio26 [gpio, qup3_se5_l2]                 -> 1
 *   gpio27 [gpio, qdss_cti, qup3_se5_l3]       -> 2, because qdss_cti
 *            (function 134) is listed before qup3_se5_l3 (function 246)
 * Index 1 on gpio27 is qdss_cti: the RX line would land in the trace unit and
 * the radio would look dead while TX and the SE loopback both worked. */
#define BT_FUNC_UART    1u             /* qup3_se5_l0/l1/l2 on gpio24/25/26 */
/* gpio18's funcs[] is [gpio, qup3_se3_l2, wcn_sw_ctrl] -- index 2 is the
 * WCN switch control that the cnss_wlan_sw_ctrl state selects. */
#define BT_FUNC_SWCTRL  2u
#define BT_FUNC_UART_RX 2u             /* qup3_se5_l3 on gpio27; index 1 is qdss_cti */
#define BT_FUNC_GPIO    0u
/* 8N1: one start bit + eight data + one stop = ten bit times per frame.
 * qcom_geni_serial.c writes tty_get_char_size() here, not the data width. */
#define BT_UART_BITS_PER_CHAR 10u

/* ---- GENI SE registers (Linux qcom-geni-se.h, msm_geni_serial.c) --------------- */
#define SE_GENI_STATUS           0x040u
#define SE_GENI_M_CMD0           0x600u
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
#define SE_GENI_BYTE_GRAN        0x254u   /* packing: words per FIFO entry */
#define SE_GENI_TX_PACKING_CFG0  0x260u
#define SE_GENI_TX_PACKING_CFG1  0x264u
#define SE_GENI_RX_PACKING_CFG0  0x284u
#define SE_GENI_RX_PACKING_CFG1  0x288u
/* UART framing and sequencer registers (Linux qcom_geni_serial.c, geni-se.h).
 * SE_GENI_CLK_SEL is an index into the SE clock table, not a divider: the divider
 * lives in SER_M/S_CLK_CFG bits 15:4 with SER_CLK_EN in bit 0.  See p_bt_uart.py. */
/* No SE_GENI_CFG at 0x000: geni-se.h has no register below 0x010, and this
 * part mirrors word_len there (0xa). Never read or write offset 0x0. */
#define SE_GENI_SER_M_CLK_CFG    0x048u
#define SE_GENI_SER_S_CLK_CFG    0x04cu
#define SE_UART_TX_TRANS_CFG     0x25cu
#define SE_UART_TX_WORD_LEN      0x268u
#define SE_UART_TX_STOP_BIT_LEN  0x26cu
#define SE_UART_TX_TRANS_LEN     0x270u
#define SE_UART_RX_TRANS_CFG     0x280u
#define SE_UART_RX_WORD_LEN      0x28cu
#define SE_UART_RX_STALE_CNT     0x294u
#define SE_GENI_TX_WATERMARK_REG 0x80cu
#define SE_GENI_RX_WATERMARK_REG 0x810u
#define SE_GENI_RX_RFR_WM_REG    0x814u
/* Linux qcom_geni_se (geni-se.h + geni-se.c).  Everything below is written by
 * geni_se_irq_clear(), geni_se_io_init() and geni_se_io_set_mode(), which
 * qcom_geni_serial_port_setup() calls through geni_se_init().  None of them was
 * ever programmed here, so the SE was left with the bootloader's mix of GSI and
 * DMA enables and with the FIFO interface's clocks gated. */
#define SE_DMA_GENERAL_CFG       0xe30u
#define SE_GSI_EVENT_EN          0xe18u
#define SE_DMA_TX_IRQ_CLEAR      0xc44u
#define SE_DMA_RX_IRQ_CLEAR      0xd44u
#define SE_UART_MANUAL_RFR       0x2acu
#define SE_UART_TX_PARITY_CFG    0x2a4u
#define SE_UART_RX_PARITY_CFG    0x2a8u
#define SE_UART_LOOPBACK_CFG     0x22cu
#define SE_UART_IO_MACRO_CTRL    0x240u
#define SE_GENI_IOS              0x908u
#define SE_GENI_M_GP_LENGTH      0x910u
#define SE_GENI_S_GP_LENGTH      0x914u
#define PAR_CALC_EN              0x00000001u
#define UART_TX_PAR_EN           0x00000001u
#define UART_RX_PAR_EN           0x00000010u
#define M_CMD_DONE_EN            0x00000001u
#define SE_GENI_M_CMD_CTRL_REG   0x604u      /* geni-se.h:98  */
#define M_GENI_CMD_CANCEL        0x00000004u /* geni-se.h:198 BIT(2) */
#define M_CMD_CANCEL_EN          0x00000010u /* geni-se.h:217 BIT(4) */
#define M_TX_FIFO_WATERMARK_EN   0x40000000u /* geni-se.h:237 BIT(30) */
#define M_TX_FIFO_RD_ERR_EN      0x10000000u /* geni-se.h:235 BIT(28) */
#define M_RX_FIFO_WATERMARK_EN   0x04000000u
#define M_RX_FIFO_LAST_EN        0x08000000u
#define DEFAULT_CGC_EN           0x0000007fu   /* geni-se.c DEFAULT_CGC_EN  GENMASK(6,0) */
#define DMA_CGC_BITS             0x0000000fu   /* AHB_SEC|DMA_AHB|DMA_TX|DMA_RX      */
#define DEFAULT_IO_OUTPUT_CTRL   0x0000007fu   /* DEFAULT_IO_OUTPUT_CTRL_MSK         */
#define SE_IRQ_MODE_BITS         0x0000000fu   /* DMA_RX|DMA_TX|GENI_M|GENI_S        */
#define FORCE_DEFAULT_BIT        0x00000001u   /* geni-se.h FORCE_DEFAULT  BIT(0)    */
#define S_RX_FIFO_WATERMARK_EN   0x04000000u   /* GENI_S_IRQ_EN  BIT(26)             */
#define S_RX_FIFO_LAST_EN        0x08000000u   /* GENI_S_IRQ_EN  BIT(27)             */
#define RX_LAST                  0x80000000u   /* GENI_RX_FIFO_STATUS  BIT(31)       */
#define RX_LAST_BYTE_VALID_MSK   0x70000000u   /* GENMASK(30,28)                     */
#define RX_LAST_BYTE_VALID_SHFT  28u
#define BYTES_PER_FIFO_WORD      4u
#define UART_MANUAL_RFR_EN       0x80000000u   /* GENI_UART_MANUAL_RFR  BIT(31)      */

#define M_OPCODE_SHFT            27u      /* GENI_M_CMD0 fields */
#define S_OPCODE_SHFT            27u      /* GENI_S_CMD0 fields */
#define SER_CLK_EN               0x00000001u
#define UART_CTS_MASK            0x00000002u
#define TX_FIFO_WC               0x0fffffffu   /* GENMASK(27, 0) */
#define RX_FIFO_WC               0x01ffffffu   /* GENMASK(24, 0) */
#define M_GENI_CMD_ACTIVE        0x00000001u
#define S_GENI_CMD_ACTIVE        0x00001000u
#define SE_GENI_IRQ_EN           0xe1cu
#define SE_FORCE_DEFAULT         0x020u
/* 0x040 is GENI_STATUS. The protocol register sits right after it: Linux has
 * STATUS 0x40 / DEFAULT_PROTOCOL 0x44 / SER_M_CLK_CFG 0x48 / SER_S_CLK_CFG 0x4C.
 * Declaring this as 0x040 (as an earlier revision did) made the probe read
 * STATUS, always 0, and write UART into STATUS -- boot XC2. */
#define SE_GENI_DEFAULT_PROTOCOL 0x044u   /* GENI_DEFAULT_PROTOCOL */
#define SE_OUTPUT_CTRL           0x024u
#define SE_CGC_CTRL              0x028u
#define SE_IF_DISABLE            0x064u
#define SE_FW_REV                0x068u
#define SE_CLK_SEL               0x07cu
#define SE_DMA_MODE              0x258u
#define SE_HW_PARAM0             0xe24u
#define SE_HW_PARAM1             0xe28u

#define GENI_PROTO_UART    0x3u
#define BT_BAUD            115200u   /* ROM/banner speed (hci_qca QCA_INIT_SPEED) */
#define BT_BAUD_OPER        4000000u  /* Android stty -F /dev/ttyHS0: speed 4000000 */
/* clk_summary on the live device: gcc_qupv3_wrap3_s5_clk_src = 64000000. The
 * 19.2 MHz value belongs to SE0 (I2C); using it here makes the divider 3.3x
 * too small and the HCI stream unreadable. */
#define BT_CLK_HZ          64000000u

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

/* The line rate bt_se_probe() programmed; bt_se_write() needs it to size its
 * retire budget against the real frame time instead of a guess. */
static uint32_t bt_baud = BT_BAUD;

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

/* TLMM CTL word, now checked against the driver that actually programs it rather
 * than inherited from pandora_touch.c:
 *
 *   pinctrl-msm.h  struct msm_pingroup   ->  mux_bit / pull_bit / drv_bit / oe_bit
 *   pinctrl-sm8550.c PINGROUP()          ->  .ctl_reg = REG_SIZE*id, .io_reg = 4 + REG_SIZE*id,
 *                                            .mux_bit = 2, .pull_bit = 0, .drv_bit = 6,
 *                                            .i2c_pull_bit = 13, .egpio_present = 11,
 *                                            .egpio_enable = 12, .oe_bit = 9,
 *                                            .in_bit = 0, .out_bit = 1
 *   pinctrl-sm8650.c PINGROUP()          ->  identical values
 *
 * So for one pin at CTL = id*0x1000 and IO = id*0x1000 + 4:
 *   [1:0]  pull   MSM_PULL_DOWN 1, MSM_PULL_UP_NO_KEEPER 2, MSM_PULL_UP 3, 0 = disable
 *   [5:2]  func   index into g->funcs[], where index 0 is msm_mux_gpio
 *   [8:6]  drive  (mA / 2) - 1, exactly as msm_config_group_set() converts it
 *   [9]    oe     output enable
 *   [11]   egpio_present, [12] egpio_enable (claim the pin when present)
 *   IO [0] in     input value, IO [1] out  output value
 *
 * BT_FUNC_UART is an index into that pin's own funcs[] array, not a SoC-wide
 * function number: msm_pinmux_set_mux() programs the match index. The index is
 * per pin, so the four UART pins do not share one value (see BT_FUNC_* above).
 * The dump below therefore prints the index it wrote and the index it reads back. */
static void
bt_gpio(unsigned n, unsigned func, unsigned pull, unsigned ma, int out)
{
	if (out >= 0) {
		/* IO bit0 is the input readback and bit1 the output latch
		 * (pinctrl-msm.c in_bit 0, out_bit 1); `out < 0` means input, so
		 * -1 must not be read as "drive high". */
		bt_wr(bt.tlmm, BT_GPIO(n) + 4, out > 0 ? 2u : 0u);
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
	printf("PANDORA BT: pins %s (ctl: func pull drv oe | io: in out):", when);
	for (unsigned i = 0; i < sizeof(pins); i++) {
		uint32_t ctl = bt_rd(bt.tlmm, BT_GPIO(pins[i]));
		uint32_t io = bt_rd(bt.tlmm, BT_GPIO(pins[i]) + 4);
		printf(" %u=%#x f%u p%u d%u e%u i%u o%u", pins[i], ctl,
		    (ctl >> 2) & 0xfu, ctl & 3u, (ctl >> 6) & 7u, (ctl >> 9) & 1u,
		    io & 1u, (io >> 1) & 1u);
	}
	printf("\n");
}

static uint32_t
bt_branch(void)
{
	return BT_SE0_BRANCH + bt.se_stride * BT_SE_INDEX;
}

/* The RCG of the SE we care about, derived the same way as its CBCR. */
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
/* Dump the vote block so the SE5 bit can be located from the board, not from a guess. */
static void
bt_vote_scan(void)
{
	uint32_t base = GCC_VOTE & ~0xffu;

	printf("PANDORA BT: vote block %#x..%#x (bit set = requested):\n", base, base + 0x80u);
	for (uint32_t o = base; o < base + 0x80u; o += 4) {
		uint32_t v = bt_rd(bt.gcc, o);
		if (v) {
			printf("   %#06x = %#010x\n", o, v);
		}
	}
}

static void
bt_clock_scan(void)
{
	unsigned shown = 0;
	printf("PANDORA BT: gcc scan 0xa8000..0xa8c00 (bit31 set = clock off, stride 0x13c):\n");
	for (uint32_t o = 0xa8000; o < 0xa8c00; o += 0x4) {
		uint32_t v = bt_rd(bt.gcc, o);
		if (v && v != 0xffffffffu) {
			printf("   %#06x = %#010x%s\n", o, v, (v & 0x80000000u) ? "  off" : "  ON");
			if (++shown >= 200) {
				printf("   ... (200 words shown)\n");
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
	if (bt.mode) {
		bt_vote_scan();
	}
	uint32_t b = bt_branch();

	/* Program the RCG source first. Linux's clk framework always prepares the parent
	 * (which here means programming the RCG and waiting for RCG_UPDATE to clear) before
	 * it touches the branch, and Android's debugfs shows s5_clk_src already at 64 MHz,
	 * so this is the step that has to succeed before HALT can drop. */
	{
		uint32_t r = bt_rcg();
		uint32_t cfg = bt_rd(bt.gcc, r + 4);
		printf("PANDORA BT: RCG %#x before: %#x / %#x\n", r, bt_rd(bt.gcc, r), cfg);
		/* Android's clk_print_regs for gcc_qupv3_wrap3_s5_clk_src shows all
		 * three MND fields set (CMD_RCGR 0x80000000, CFG_RCGR 0x00100001,
		 * M 0x10, N 0xffc4, D 0xffb4 -> 300 MHz * 16 / 75 = 64 MHz).  Writing
		 * only the DIV field leaves the fractional divider at a ratio of zero.
		 * Wiring the divider up did not change the silent-ROM symptom (boot
		 * YN, 2026-11-12), so keep it correct but do not expect it to fix it. */
		bt_wr(bt.gcc, r + 8u, 0x00000010u);   /* M_VAL */
		bt_wr(bt.gcc, r + 12u, 0x0000ffc4u);  /* N_VAL */
		bt_wr(bt.gcc, r + 16u, 0x0000ffb4u);  /* D_VAL */
		bt_wr(bt.gcc, r + 4u, 0x00100001u);   /* CFG_RCGR: MND on, DIV 1, src 1 */
		bt_wr(bt.gcc, r, bt_rd(bt.gcc, r) | 1u);
		for (unsigned t = 0; t < 5000 && (bt_rd(bt.gcc, r) & 1u); t++) {
			delay(1000);
		}
		printf("PANDORA BT: RCG %#x after: %#x / %#x m %#x n %#x d %#x%s\n", r,
		    bt_rd(bt.gcc, r), bt_rd(bt.gcc, r + 4u), bt_rd(bt.gcc, r + 8u),
		    bt_rd(bt.gcc, r + 12u), bt_rd(bt.gcc, r + 16u),
		    (bt_rd(bt.gcc, r) & 1u) ? "  (update did not clear)" : "  ok");
	}

	for (unsigned t = 0; t < 10000 && !bt_on(b); t++) {
		delay(1000);
	}
	if (!bt_on(b)) {
		/*
		 * Boot XA settled this: GCC_VOTE 0x52018 has to be saturated, not masked to
		 * bits 8..13. 0x1f00 started SE0 only, 0x3f00 still left SE5 halted, and
		 * 0xffffff70 turned SE5 CBCR 0x88004000 -> 0x8004000. The extra bits are the
		 * wrapper's core / core_2x enables, which SE0..SE4 evidently do not need but
		 * SE5's branch does. The word reads back as written, so nothing here is
		 * write-protected.
		 */
		uint32_t before = bt_rd(bt.gcc, GCC_VOTE);
		bt_wr(bt.gcc, GCC_VOTE, before | 0xffffffffu);
		for (unsigned t = 0; t < 10000 && !bt_on(b); t++) {
			delay(1000);
		}
		if (!bt_on(b)) {
			/* clk_print_regs on the live device reports CBCR = 0x88004000 for this
			 * same branch with enable_count 0, so HALT set is this clock's normal
			 * idle state, not a fault. The SE registers read fine either way (boot
			 * XA), so warn and continue instead of aborting the whole BT bring-up. */
			printf("PANDORA BT: SE%u CBCR %#x still shows HALT; "
			    "Android reports the same value, continuing\n",
			    BT_SE_INDEX, bt_rd(bt.gcc, b));
		} else {
			printf("PANDORA BT: SE%u branch %#x up (%#x -> %#x)\n", BT_SE_INDEX, b,
			    before, bt_rd(bt.gcc, b));
		}
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
		uint32_t cfg = (bt_rd(bt.gcc, r + 4) & ~0x371fu) | 1u;   /* keep MND, see above */
		bt_wr(bt.gcc, r + 4, cfg);
		bt_wr(bt.gcc, r, bt_rd(bt.gcc, r) | 1u);
		for (unsigned t = 0; t < 5000 && (bt_rd(bt.gcc, r) & 1u); t++) {
			delay(1000);
		}
		if (bt_rd(bt.gcc, r) & 1u) {
			printf("PANDORA BT: RCG %#x update timeout\n", r);
			return EIO;
		}
	}
	return 0;
}

/* A full read of the SE5 register block. Android runs its HCI on this very
 * instance, so every field that matters has a known-good value to compare
 * against. Reading only: nothing here writes. */
/* The 12-window / 192-word register census that used to live here is gone: it
 * swept 0x000 (no register there) and the vendor-private tail of 0xe00, and it
 * ran on every cancelled command. Reading undocumented offsets on this SE is
 * what dropped the phone off USB twice. bt_se_dump() covers the same ground
 * with named registers only. */

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

/* BT_EN is GPIO 5 of pmh0104@9.  That is a PMIC pin, so it is reachable only
 * over SPMI: peripheral 0x8c (0x88 + 5 - 1), PPID (SID 9 << 8) | 0x8c.  The
 * register layout comes from Linux drivers/pinctrl/qcom/pinctrl-spmi-gpio.c:
 * pad base 0x8800 + 4 * 0x100, VIN 0x41, PULL 0x42, IN 0x43, OUT_CTL 0x45,
 * MODE_CTL 0x40, EN_CTL 0x46.  pandora_spmi_write() refuses a PPID that is
 * not in the channel map, so a wrong number cannot reach another peripheral. */
#define BT_EN_PPID    0x098cu
#define BT_EN_BUS     1u      /* pmh0104@9 is under spmi@c437000 */
#define BT_EN_VIN     0x41u
#define BT_EN_PULL    0x42u
#define BT_EN_IN      0x43u
#define BT_EN_OUT     0x45u
#define BT_EN_MODE    0x40u
#define BT_EN_ENABLE  0x46u

extern int sweet_rpmh_vreg_enable(const char *res, unsigned mv);
extern int sweet_rpmh_clk_enable(const char *res);
extern int pandora_spmi_write(uint16_t ppid, uint8_t reg, const uint8_t *buf, unsigned n);
extern int pandora_spmi_read(uint16_t ppid, uint8_t reg, uint8_t *buf, unsigned n);

#define BT_SPMI_CHN_PA   0x0c900000u
#define BT_SPMI_OBS_PA   0x0c4c0000u
/* SPMI windows, straight out of canoe_base.dts:7745-7776 (arbiter@c400000):
 * core 0x0c400000, chnl_map 0x0c403000, chnls 0x0c900000, obsrvr 0x0c4c0000,
 * bus0 chnl_owner 0x0c42a000, bus1 chnl_owner 0x0c43b000. */
#define BT_SPMI_CORE_PA  0x0c400000u
#define BT_SPMI_MAP_PA   0x0c403000u
#define BT_SPMI_OWN_PA   0x0c42a000u
#define BT_SPMI_OWN1_PA  0x0c43b000u
#define BT_SPMI_MAXAPID  8192u

/* Locate one PPID on an explicit bus.  The arbiter map is indexed globally:
 * bus 0 first, then bus 1, so the entry index is base + slot. */
static int
bt_spmi_pick(unsigned bus, uint16_t ppid, uint16_t *apid_out, unsigned *owner_out)
{
	vm_offset_t core = ml_io_map(BT_SPMI_CORE_PA, 0x1000);
	vm_offset_t map = ml_io_map(BT_SPMI_MAP_PA, 0x8000);
	if (!core || !map) {
		return -2;
	}
	uint32_t base = 0, count = 0;
	for (unsigned b = 0; b <= bus; b++) {
		base += count;
		count = *(volatile uint32_t *)(core + 4 + 4 * b) & 0x1fffu;
	}
	if (count == 0 || base + count > BT_SPMI_MAXAPID) {
		return -3;
	}
	vm_offset_t own = (bus == 0)
	    ? ml_io_map(BT_SPMI_OWN_PA, 0x8000)
	    : ml_io_map_unmappable(BT_SPMI_OWN1_PA, 0x8000, VM_WIMG_IO);
	if (!own) {
		return -2;
	}
	for (uint32_t i = base; i < base + count; i++) {
		const uint32_t e = *(volatile uint32_t *)(map + 4u * i);
		if (e == 0 || (e & 0x1fffu) != (ppid & 0x1fffu)) {
			continue;
		}
		if (apid_out) {
			*apid_out = (uint16_t)i;
		}
		if (owner_out) {
			*owner_out = *(volatile uint32_t *)(own + 4u * (i - base)) & 7u;
		}
		return 0;
	}
	return -3;
}

/* The observer window is 1 MiB and ml_io_map() carves it out of kmem.  Mapping
 * it once per byte of register dump exhausted the kmem submap and killed the
 * boot at the first BT_EN probe (kmem_alloc 1048576 failed with 3), so both
 * windows are now mapped once and cached. */
static vm_offset_t bt_obs_va;
static vm_offset_t bt_chn_va;
static uint32_t bt_chn_page = 0xffffffffu;

/* One EXT_READL through the EE0 observer window (no ownership needed). */
static int
bt_spmi_rd_apid(uint16_t apid, uint8_t reg, uint8_t *buf, unsigned n)
{
	if (n == 0 || n > 8) {
		return -1;
	}
	if (!bt_obs_va) {
		bt_obs_va = ml_io_map(BT_SPMI_OBS_PA, 0x100000);
		if (!bt_obs_va) {
			return -2;
		}
	}
	const vm_offset_t obs = bt_obs_va;
	const uint32_t off = 0x20u * apid;
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(obs + off) = (1u << 27) | ((uint32_t)reg << 4) | (n - 1u);
	__asm__ volatile("dsb osh" ::: "memory");
	for (unsigned i = 0; i < 1000; i++) {
		const uint32_t st = *(volatile uint32_t *)(obs + off + 0x08u);
		if (st & 1u) {
			if (st & 0x0eu) {
				return -4;
			}
			const uint32_t lo = *(volatile uint32_t *)(obs + off + 0x18u);
			const uint32_t hi = (n > 4) ? *(volatile uint32_t *)(obs + off + 0x1cu) : 0u;
			for (unsigned k = 0; k < n; k++) {
				buf[k] = (uint8_t)((k < 4 ? lo : hi) >> (8 * (k % 4)));
			}
			return 0;
		}
		delay(1000);
	}
	return -5;
}

/* One EXT_WRITEL on the APID's read-write channel.  The arbiter rejects a
 * write whose channel is owned by another execution environment, and the
 * status byte says so, so this reports the raw status rather than guessing. */
static int
bt_spmi_wr_apid(uint16_t apid, uint8_t reg, const uint8_t *buf, unsigned n, uint32_t *status)
{
	if (n == 0 || n > 8) {
		return -1;
	}
	const uint32_t pa = BT_SPMI_CHN_PA + 0x200u * apid;
	const uint32_t page = pa & ~0xfffu;
	if (bt_chn_page != page) {
		vm_offset_t va = ml_io_map_unmappable(page, 0x1000, VM_WIMG_IO);
		if (!va) {
			return -2;
		}
		bt_chn_va = va;
		bt_chn_page = page;
	}
	vm_offset_t ch = bt_chn_va + (pa & 0xfffu);
	uint32_t d[2] = {0, 0};
	for (unsigned k = 0; k < n; k++) {
		d[k / 4] |= (uint32_t)buf[k] << (8 * (k % 4));
	}
	*(volatile uint32_t *)(ch + 0x10u) = d[0];
	if (n > 4) {
		*(volatile uint32_t *)(ch + 0x14u) = d[1];
	}
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(ch + 0x00u) = ((uint32_t)reg << 4) | (n - 1u);
	__asm__ volatile("dsb osh" ::: "memory");
	for (unsigned i = 0; i < 1000; i++) {
		const uint32_t st = *(volatile uint32_t *)(ch + 0x08u);
		if (st & 1u) {
			if (status) {
				*status = st;
			}
			return (st & 0x7eu) ? -4 : 0;
		}
		delay(1000);
	}
	if (status) {
		*status = 0xffffffffu;
	}
	return -5;
}


/* Read-only walk of the arbiter channel map.  ps_apid() answers "is this PPID
 * present"; when it says no, this says what the bus actually carries. */
static void
bt_spmi_scan(unsigned sid)
{
	vm_offset_t core = ml_io_map(BT_SPMI_CORE_PA, 0x1000);
	vm_offset_t map = ml_io_map(BT_SPMI_MAP_PA, 0x8000);
	vm_offset_t own = ml_io_map(BT_SPMI_OWN_PA, 0x8000);
	vm_offset_t own1 = ml_io_map_unmappable(BT_SPMI_OWN1_PA, 0x8000, VM_WIMG_IO);
	if (!core || !map || !own) {
		printf("PANDORA BT: spmi scan cannot map the arbiter\n");
		return;
	}
	uint32_t version = *(volatile uint32_t *)(core + 0);
	uint32_t nbus0 = *(volatile uint32_t *)(core + 4) & 0x1fffu;
	uint32_t nbus1 = *(volatile uint32_t *)(core + 8) & 0x1fffu;
	printf("PANDORA BT: spmi arb %#x bus0 %u bus1 %u (looking for sid %u)\n",
	    version, nbus0, nbus1, sid);

	const uint32_t buses[2] = {nbus0, nbus1};
	const vm_offset_t owns[2] = {own, own1};
	uint32_t base = 0;
	for (unsigned b = 0; b < 2; b++) {
		uint32_t count = buses[b];
		if (base + count > BT_SPMI_MAXAPID || !owns[b]) {
			printf("PANDORA BT: spmi bus %u skipped (%u entries)\n", b, count);
			continue;
		}
		printf("PANDORA BT: spmi bus %u: apids %u..%u\n", b, base, base + count - 1);
		unsigned sid_counts[8] = {0};
		unsigned hits = 0;
		for (uint32_t i = base; i < base + count; i++) {
			const uint32_t e = *(volatile uint32_t *)(map + 4u * i);
			if (e == 0) {
				continue;
			}
			const unsigned s = (e >> 8) & 0xffu;
			if (s < 64) {
				sid_counts[s >> 3]++;
			}
			if (s != sid) {
				continue;
			}
			if (hits < 64) {
				const unsigned o = *(volatile uint32_t *)(owns[b] + 4u * (i - base)) & 7u;
				printf("PANDORA BT: spmi   apid %u entry %#x ppid %#x periph %#x owner ee%u\n",
				    i, e, e & 0x1fffu, e & 0xffu, o);
			}
			hits++;
		}
		printf("PANDORA BT: spmi bus %u: %u entries for sid %u\n", b, hits, sid);
		base += count;
	}
}

static int
bt_pmic_dump(const char *when, uint16_t apid, unsigned level)
{
	static const uint8_t regs[8] = {0x04u, 0x05u, 0x40u, 0x41u, 0x42u, 0x45u, 0x46u, 0x47u};
	uint8_t v[8] = {0};
	int e[8] = {0};
	for (unsigned i = 0; i < 8; i++) {
		e[i] = bt_spmi_rd_apid(apid, regs[i], &v[i], 1);
	}
	printf("PANDORA BT: pmic gpio5 %s (level %u): type=%#x sub=%#x mode=%#x vin=%#x pull=%#x out=%#x en=%#x st=%#x err %d%d%d%d%d%d%d%d\n",
	    when, level, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
	    e[0] < 0, e[1] < 0, e[2] < 0, e[3] < 0, e[4] < 0, e[5] < 0, e[6] < 0, e[7] < 0);
	return e[0] < 0 ? -1 : 0;
}

static int
bt_pmic_gpio5(unsigned level)
{
	uint16_t apid = 0;
	unsigned owner = 7;
	int e = bt_spmi_pick(BT_EN_BUS, BT_EN_PPID, &apid, &owner);
	if (e) {
		printf("PANDORA BT: BT_EN ppid %#x is not on bus %u (%d)\n", BT_EN_PPID, BT_EN_BUS, e);
		bt_spmi_scan(BT_EN_PPID >> 8);
		return e;
	}
	printf("PANDORA BT: BT_EN ppid %#x -> bus %u apid %u owner ee%u\n",
	    BT_EN_PPID, BT_EN_BUS, apid, owner);
	(void)bt_pmic_dump("before", apid, level);
	if (owner != 0) {
		printf("PANDORA BT: BT_EN channel is owned by ee%u, not EE0; a write would be rejected, trying anyway\n",
		    owner);
	}
	uint32_t st = 0;
	struct {
		uint8_t reg;
		uint8_t val;
	} seq[5] = {
		{BT_EN_VIN, 0x01u},
		{BT_EN_PULL, 0x05u},
		{BT_EN_OUT, 0x00u},
		{BT_EN_IN, 0x00u},
		{BT_EN_MODE, (uint8_t)(0x10u | (level & 1u))},
	};
	for (unsigned i = 0; i < 5; i++) {
		e = bt_spmi_wr_apid(apid, seq[i].reg, &seq[i].val, 1, &st);
		printf("PANDORA BT: BT_EN write reg %#x = %#x -> %d (status %#x)\n",
		    seq[i].reg, seq[i].val, e, st);
		if (e) {
			return e;
		}
	}
	e = bt_spmi_wr_apid(apid, BT_EN_ENABLE, (const uint8_t[]){0x80u}, 1, &st);
	printf("PANDORA BT: BT_EN master enable -> %d (status %#x)\n", e, st);
	if (e) {
		return e;
	}
	(void)bt_pmic_dump("after", apid, level);
	return 0;
}

/* geni_se_irq_clear() + geni_se_io_init() + geni_se_io_set_mode(), in that order
 * (Linux geni-se.c:345, :328, :312; called from geni_se_init() at :364).
 *
 * Why this matters here: SE_IRQ_EN 0xe1c gates the four IRQ sources, CGC_CTRL keeps
 * the FIFO interface's clocks alive, DMA_GENERAL_CFG keeps the AHB slave path clocked,
 * and GSI_EVENT_EN has to be zero or the SE routes RX to GSI instead of the FIFO.
 * Writing SE_IRQ_EN = 0 (as an earlier revision did) disabled every source while the
 * GSI/DMA enables stayed as the bootloader left them. */
static void
bt_se_linux_init(void)
{
	/* geni_se_irq_clear(), minus the two things we deliberately do differently:
	 * Linux finishes by writing 0xffffffff to SE_IRQ_EN, which routes all four
	 * sources at the GIC.  Nothing here installs a handler, so this revision sets
	 * only the four mode bits below (geni_se_io_set_mode's contribution) and leaves
	 * the rest of the register alone - a pending-but-masked SPI cannot storm. */
	bt_wr(bt.se, SE_GSI_EVENT_EN, 0);
	bt_wr(bt.se, SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	bt_wr(bt.se, SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
	bt_wr(bt.se, SE_DMA_TX_IRQ_CLEAR, 0xffffffffu);
	bt_wr(bt.se, SE_DMA_RX_IRQ_CLEAR, 0xffffffffu);
	/* ---- geni_se_io_init() ---- */
	bt_wr(bt.se, SE_CGC_CTRL, bt_rd(bt.se, SE_CGC_CTRL) | DEFAULT_CGC_EN);
	bt_wr(bt.se, SE_DMA_GENERAL_CFG, bt_rd(bt.se, SE_DMA_GENERAL_CFG) | DMA_CGC_BITS);
	bt_wr(bt.se, SE_OUTPUT_CTRL, DEFAULT_IO_OUTPUT_CTRL);
	bt_wr(bt.se, SE_FORCE_DEFAULT, FORCE_DEFAULT_BIT);
	/* ---- geni_se_io_set_mode() ---- */
	bt_wr(bt.se, SE_GENI_IRQ_EN, bt_rd(bt.se, SE_GENI_IRQ_EN) | SE_IRQ_MODE_BITS);
	bt_wr(bt.se, SE_DMA_MODE, bt_rd(bt.se, SE_DMA_MODE) & ~1u);
	bt_wr(bt.se, SE_GSI_EVENT_EN, 0);
	/* Status only: one boot then shows whether the SE was left in GSI mode. */
	printf("PANDORA BT: se linux init: cgc %#x dma_cfg %#x out %#x force %#x irq_en %#x dma %#x gsi %#x\n",
	    bt_rd(bt.se, SE_CGC_CTRL), bt_rd(bt.se, SE_DMA_GENERAL_CFG),
	    bt_rd(bt.se, SE_OUTPUT_CTRL), bt_rd(bt.se, SE_FORCE_DEFAULT),
	    bt_rd(bt.se, SE_GENI_IRQ_EN), bt_rd(bt.se, SE_DMA_MODE),
	    bt_rd(bt.se, SE_GSI_EVENT_EN));
}

/* Pull one FIFO's worth of received bytes out, word at a time.
 *
 * Linux handle_rx_fifo() (qcom_geni_serial.c:511) computes the byte count from the
 * word count and the last-word fields:
 *   total = 4 * (words - 1) + (partial && last_byte_cnt ? last_byte_cnt : 4)
 * and then reads whole u32 words.  Reading 0x780 one byte at a time pops one FIFO
 * ENTRY per read, so the previous code consumed four entries to fill four bytes and
 * threw away three bytes of every word. */
/* The RX sequencer parks itself once it reports RX_LAST; qcom_geni_serial.c
 * restarts it from its interrupt handler every time. Without this the first
 * completed read is also the last one the port ever sees. */
static void
bt_rx_restart(void)
{
	/* bt_se_probe() already armed this sequencer, and it keeps running until it
	 * reports RX_LAST; writing S_CMD0 on top of a live command raises
	 * S_CMD_OVERRUN. Wait -- bounded -- for it to park, then re-arm.
	 *
	/* Callers inside the TX push path must use bt_rx_restart_soft(): a wait
	 * here is 1 ms per turn, and the 16-word TX FIFO drains in 5.5 ms, so a
	 * blocking restart from bt_rx_pump() starves the sequencer and earns the
	 * M_TX_FIFO_RD_ERR (bit 28) that killed every 249 B segment. */
	unsigned t = 0;
	for (; t < 50u; t++) {
		if (!(bt_rd(bt.se, SE_GENI_STATUS) & S_GENI_CMD_ACTIVE)) {
			break;
		}
		delay(1000);
	}
	if (bt_rd(bt.se, SE_GENI_STATUS) & S_GENI_CMD_ACTIVE) {
		printf("PANDORA BT: rx sequencer still active after %u ms, not re-arming (avoids S_CMD_OVERRUN)\n", t);
		return;
	}
	bt_wr(bt.se, SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
	bt_wr(bt.se, SE_GENI_S_CMD0,
	    (bt_rd(bt.se, SE_GENI_S_CMD0) & ~(0xf8000000u | 0x07ffffffu)) |
	    (1u << S_OPCODE_SHFT));
}

/* Non-blocking re-arm for the TX push path: one look, no sleep. Parking is
 * rare inside a push, and the next bt_rx_drain() will re-arm it anyway. */
static void
bt_rx_restart_soft(void)
{
	if (bt_rd(bt.se, SE_GENI_STATUS) & S_GENI_CMD_ACTIVE) {
		return;
	}
	bt_wr(bt.se, SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
	bt_wr(bt.se, SE_GENI_S_CMD0,
	    (bt_rd(bt.se, SE_GENI_S_CMD0) & ~(0xf8000000u | 0x07ffffffu)) |
	    (1u << S_OPCODE_SHFT));
}

static unsigned bt_rx_dumps;

/* Received bytes need somewhere to live.
 *
 * The old code drained the RX FIFO into a 64-byte scratch buffer inside
 * bt_se_putword() and threw it away on every turn of the TX wait loop: a 249 B
 * segment is 63 FIFO words, so any byte the radio sent while we were pushing the
 * next word was discarded. There is no interrupt handler here, so nothing else
 * collects it. Linux runs RX in its ISR and never sees this.
 *
 * One thread produces (bt_rx_pump) and the same thread consumes (bt_rxq_pop), so
 * no locking is needed. */
#define BT_RXQ_SIZE 1024u
static uint8_t bt_rxq[BT_RXQ_SIZE];
static unsigned bt_rxq_head, bt_rxq_tail;

static void
bt_rxq_push(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned next = (bt_rxq_tail + 1u) % BT_RXQ_SIZE;
		if (next == bt_rxq_head) {
			return;         /* full: drop the newest byte */
		}
		bt_rxq[bt_rxq_tail] = p[i];
		bt_rxq_tail = next;
	}
}

static size_t
bt_rxq_pop(uint8_t *dst, size_t cap)
{
	size_t n = 0;

	while (n < cap && bt_rxq_head != bt_rxq_tail) {
		dst[n++] = bt_rxq[bt_rxq_head];
		bt_rxq_head = (bt_rxq_head + 1u) % BT_RXQ_SIZE;
	}
	return n;
}

/* Move everything the hardware FIFO holds into the queue. Whole FIFO words:
 * one read of 0x780 pops one entry, and with 4x8 packing that entry carries four
 * characters (boot XD: an empty FIFO reads 0, so the old "& 1" test for room
 * stalled before the first byte ever went out). */
static void
bt_rx_pump(void)
{
	uint32_t st = bt_rd(bt.se, SE_GENI_RX_FIFO_STATUS);
	uint32_t words = st & RX_FIFO_WC;

	if ((bt.mask & 65536u) && bt_rx_dumps < 24u) {
		printf("PANDORA BT: rx drain#%u st %#x words %u last %u lbv %u s_irq %#x s_status %#x\n",
		    bt_rx_dumps, st, words, (st & RX_LAST) ? 1u : 0u,
		    (unsigned)((st & RX_LAST_BYTE_VALID_MSK) >> RX_LAST_BYTE_VALID_SHFT),
		    bt_rd(bt.se, SE_GENI_S_IRQ_STATUS), bt_rd(bt.se, SE_GENI_STATUS));
		bt_rx_dumps++;
	}
	for (uint32_t w = 0; w < words; w++) {
		uint32_t word = bt_rd(bt.se, SE_GENI_RX_FIFO);
		uint8_t bytes[BYTES_PER_FIFO_WORD];

		if ((bt.mask & 65536u) && bt_rx_dumps <= 24u) {
			printf("PANDORA BT: rx word[%u] = %#x\n", w, word);
		}
		for (unsigned k = 0; k < BYTES_PER_FIFO_WORD; k++) {
			bytes[k] = (uint8_t)(word >> (8u * k));
		}
		bt_rxq_push(bytes, BYTES_PER_FIFO_WORD);
	}
	/* RX_LAST means the secondary sequencer has finished this read and parked;
	 * put it back so the rest of an answer is not lost. */
	if (st & RX_LAST) {
		bt_rx_restart_soft();
	}
}

/* Throw away whatever is parked, hardware and queue both. Only used to start an
 * experiment (loopback) from a known-empty FIFO -- never inside a transfer. */
static void
bt_rx_flush(void)
{
	for (unsigned i = 0; i < 64u; i++) {
		uint32_t st = bt_rd(bt.se, SE_GENI_RX_FIFO_STATUS);
		uint32_t words = st & RX_FIFO_WC;

		if (!words) {
			break;
		}
		for (uint32_t w = 0; w < words; w++) {
			(void)bt_rd(bt.se, SE_GENI_RX_FIFO);
		}
	}
	bt_rxq_head = 0;
	bt_rxq_tail = 0;
	bt_wr(bt.se, SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
}

/* Hand the parser whatever has arrived. Callers that only want the FIFO emptied
 * into the queue use bt_rx_pump() directly. */
static size_t
bt_rx_drain(uint8_t *dst, size_t cap)
{
	if (!cap) {
		return 0;
	}
	bt_rx_pump();
	return bt_rxq_pop(dst, cap);
}

/* RPMh rails peach-bt sits on.  btpower.ko enables the qcom,bt-vdd-* supplies
 * before it touches resetb, wl-reset or BT_EN; S2J feeds three of the nine supplies
 * (dig, aon, rfa0p75) so it is voted once.  Voltages are the DT init values from
 * canoe_base.dts in mV, and each one is inside the window of the matching
 * rpmh-regulator node.  sweet_rpmh_vreg_enable() resolves the cmd-db address itself
 * and refuses anything outside that window. */
struct bt_rail {
	const char *res;
	unsigned mv;
};

/* Voltages are the ones Android actually programs, read back from
 * /sys/kernel/debug/regulator/regulator_summary on this phone with
 * Bluetooth toggled off and on (identical both ways):
 *   S7F_E0 1224 (its window starts at 1224, DTS init 1256)
 *   S8F_E0 1800 (window starts at 1800, DTS init 1857)
 *   S1J_E1 / S2J_E1 892 -- 0xd9c60, the floor of peach-bt's
 *   qcom,bt-vdd-{dig,aon,rfa0p75}-config windows, not the 852 init value.
 * Three rails feed the same PMIC buck, so S2J is voted once. */
static const struct bt_rail bt_rails[] = {
	{ "S2J_E1",  892 },
	{ "S7F_E0", 1224 },
	{ "S8F_E0", 1800 },
	{ "L2G_E0", 1800 },
	{ "L3G_E0", 1200 },
	{ "S1J_E1",  892 },
	{ "L6K_E1", 1700 },
};

static void
bt_rails_up(void)
{
	for (unsigned i = 0; i < sizeof(bt_rails) / sizeof(bt_rails[0]); ++i) {
		int e = sweet_rpmh_vreg_enable(bt_rails[i].res, bt_rails[i].mv);
		printf("PANDORA BT: rail %s %u mV -> %d\n", bt_rails[i].res,
		    bt_rails[i].mv, e);
	}

	/* peach-bt's dts names one more resource that is not a regulator:
	 * "clock-names = bt_rf_clk1" with clocks = <rpmhcc 6>
	 * (canoe_base.dts __symbols__: rpmhcc = ".../rsc@18900000/drv@2/
	 * clock-controller", #clock-cells = 1).  clk-rpmh.c maps index 6 of
	 * that provider to the cmd-db resource C1A_E0, and Android's clk_summary
	 * reads "c1a_e0 ... 76800000 ... hardware enable Y" -- a 76.8 MHz
	 * reference the SoC needs before it will drive its UART TX pad.  Without
	 * it the reset handshake completes and the pad stays low, which is what
	 * the pad watch measured (pin27 low 100 high 0). */
	if (bt.mask & 4096u) {
		int e = sweet_rpmh_clk_enable("C1A_E0");
		printf("PANDORA BT: clock C1A_E0 (bt_rf_clk1, 76.8 MHz) -> %d\n", e);
	}
}

/* Watch the four UART pads instead of guessing from one snapshot. TLMM gives
 * the pad level in the IO register (bit 0, input) and SE_GENI_IOS mirrors the
 * same four lines inside the SE block, so a pair of reads per sample separates
 * "the radio drives it" from "the pin is still GPIO and floats". */
static void
bt_pad_watch(const char *when, unsigned samples, unsigned gap_ms)
{
	uint32_t ios_first = bt_rd(bt.se, SE_GENI_IOS) & 0xfu;
	uint32_t ios_mask = 0, io_mask = 0, prev = ios_first;
	unsigned changed = 0, low27 = 0, high27 = 0;

	for (unsigned i = 0; i < samples; ++i) {
		uint32_t ios = bt_rd(bt.se, SE_GENI_IOS) & 0xfu;
		uint32_t io27 = bt_rd(bt.tlmm, BT_GPIO(27u) + 4u) & 1u;
		uint32_t io24 = bt_rd(bt.tlmm, BT_GPIO(24u) + 4u) & 1u;
		ios_mask |= ios;
		io_mask |= (io24 << 1) | io27;
		if (ios != prev) {
			++changed;
			prev = ios;
		}
		if (io27) {
			++high27;
		} else {
			++low27;
		}
		bt_sleep(gap_ms);
	}
	printf("PANDORA BT: pad watch %s over %u x %u ms: ios first %#x or-bits %#x "
	    "changes %u | pin24 high anytime %u | pin27 low %u high %u\n",
	    when, samples, gap_ms, ios_first, ios_mask, changed,
	    (io_mask >> 1) & 1u, low27, high27);
}

static int
bt_se_probe(void)
{
	uint32_t fw = bt_rd(bt.se, SE_FW_REV);
	/* Linux reads the protocol out of GENI_FW_REV with GENI_FW_REV_MAJOR(rev) =
	 * (rev >> 8) & 0xff, so 0x206 means major 2 -- not 6. Reading the low byte is what
	 * made the first probe report "protocol 6" and bail. */
	uint32_t proto = (fw >> 8) & 0xffu;
	if (proto != GENI_PROTO_UART) {
		/* Not in UART mode: clear force-default and ask the SE for UART. The Android
		 * side left this SE on I2C (GENI_DEFAULT_PROTOCOL 0x040 read 0x203029e4, low
		 * nibble 2), which is why the branch had nothing to clock. */
		uint32_t d = bt_rd(bt.se, SE_GENI_DEFAULT_PROTOCOL);
		/* Print the neighbours too: one boot then shows which offset carries the
		 * protocol number instead of us guessing again. */
		printf("PANDORA BT: SE protocol block 0x40=%#x 0x44=%#x 0x48=%#x 0x4c=%#x\n",
		    bt_rd(bt.se, 0x040u), bt_rd(bt.se, 0x044u),
		    bt_rd(bt.se, 0x048u), bt_rd(bt.se, 0x04cu));
		printf("PANDORA BT: SE firmware %#x major %u, default protocol %#x (%u); "
		    "switching to UART\n", fw, proto, d, d & 0x7u);
		bt_wr(bt.se, SE_FORCE_DEFAULT, 0);
		bt_wr(bt.se, SE_GENI_DEFAULT_PROTOCOL, GENI_PROTO_UART);
		delay(10);
		d = bt_rd(bt.se, SE_GENI_DEFAULT_PROTOCOL);
		if ((d & 0x7u) != GENI_PROTO_UART) {
			printf("PANDORA BT: SE stayed on protocol %u after the write\n", d & 0x7u);
			return ENODEV;
		}
		printf("PANDORA BT: SE now on UART (%#x)\n", d);
	}
	if (bt_rd(bt.se, SE_IF_DISABLE) & 1u) {
		printf("PANDORA BT: FIFO interface disabled (if_disable bit 0); "
		    "would need GPI DMA, not attempted\n");
		return ENOTSUP;
	}
	bt_se_linux_init();
	/* Baud divider: Linux geni_serial_set_rate() puts DIV_ROUND_UP(clk, baud *
	 * sampling) into GENI_SER_M_CLK_CFG bits 15:4 with SER_CLK_EN in bit 0, and
	 * writes only the *table index* into SE_GENI_CLK_SEL.  QUP >= 2.5 halves the
	 * 32x oversampling to 16x, so 64 MHz / (16 * 115200) -> 35. */
	uint32_t baud = BT_BAUD;
	(void)PE_parse_boot_argn("pandora_btbaud", &baud, sizeof(baud));
	if (baud < 9600u || baud > 8000000u) {
		baud = BT_BAUD;
	}
	uint32_t div = (BT_CLK_HZ + 16u * baud - 1u) / (16u * baud);
	if (div == 0u) {
		div = 1u;
	}
	bt_baud = baud;
	uint32_t ser = SER_CLK_EN | ((div & 0xfffu) << 4);
	bt_wr(bt.se, SE_GENI_SER_M_CLK_CFG, ser);
	bt_wr(bt.se, SE_GENI_SER_S_CLK_CFG, ser);
	bt_wr(bt.se, SE_CLK_SEL, 0u);
	/* Frame format: 8N1, no parity, CTS ignored (Linux writes UART_CTS_MASK when
	 * CRTSCTS is off).  Never programmed before, so the sequencer had no word
	 * length to shift out. */
	bt_wr(bt.se, SE_UART_TX_TRANS_CFG, UART_CTS_MASK);
	/* 8N1 is ten bit times per frame, not eight: Linux writes
	 * tty_get_char_size(), and the SE samples on that boundary. */
	bt_wr(bt.se, SE_UART_TX_WORD_LEN, BT_UART_BITS_PER_CHAR);
	bt_wr(bt.se, SE_UART_TX_STOP_BIT_LEN, 0u);
	bt_wr(bt.se, SE_UART_RX_TRANS_CFG, 0u);
	bt_wr(bt.se, SE_UART_RX_WORD_LEN, BT_UART_BITS_PER_CHAR);
	bt_wr(bt.se, SE_UART_RX_STALE_CNT, 10u * 16u);
	/* Parity: Linux qcom_geni_serial_set_termios() clears PAR_CALC_EN in both
	 * SE_UART_TX_PARITY_CFG and SE_UART_RX_PARITY_CFG when PARENB is off, and clears
	 * UART_TX_PAR_EN / UART_RX_PAR_EN in the two TRANS_CFG registers. Nothing here
	 * ever touched those four fields, so whatever the bootloader left behind was still
	 * in force: a stray parity bit turns every 8-bit character into nine bits on the
	 * wire, which the chip answers with silence rather than an error. */
	{
		uint32_t tp = bt_rd(bt.se, SE_UART_TX_PARITY_CFG);
		uint32_t rp = bt_rd(bt.se, SE_UART_RX_PARITY_CFG);
		uint32_t tx = bt_rd(bt.se, SE_UART_TX_TRANS_CFG);
		uint32_t rx = bt_rd(bt.se, SE_UART_RX_TRANS_CFG);
		bt_wr(bt.se, SE_UART_TX_PARITY_CFG, tp & ~PAR_CALC_EN);
		bt_wr(bt.se, SE_UART_RX_PARITY_CFG, rp & ~PAR_CALC_EN);
		bt_wr(bt.se, SE_UART_TX_TRANS_CFG, tx & ~UART_TX_PAR_EN);
		bt_wr(bt.se, SE_UART_RX_TRANS_CFG, rx & ~UART_RX_PAR_EN);
		if ((tp | rp) & PAR_CALC_EN) {
			printf("PANDORA BT: parity was armed (tx %#x rx %#x), cleared\n", tp, rp);
		}
	}
	/* Loopback: set_termios() ends with a write of port->loopback (zero unless the
	 * loopback knob is on) to SE_UART_LOOPBACK_CFG. Left set by the bootloader, it
	 * feeds our own TX back into RX and no character from the chip can appear. */
	{
		uint32_t lb = bt_rd(bt.se, SE_UART_LOOPBACK_CFG);
		bt_wr(bt.se, SE_UART_LOOPBACK_CFG, 0u);
		if (lb) {
			printf("PANDORA BT: loopback was %#x, cleared\n", lb);
		}
	}
	printf("PANDORA BT: io macro %#x ios %#x\n",
	    bt_rd(bt.se, SE_UART_IO_MACRO_CTRL), bt_rd(bt.se, SE_GENI_IOS));
	/* Nothing is read or written at offset 0x0 any more.  Linux geni-se.h has no
	 * register there at all (the first one is GENI_GENERAL_CFG 0x010); the two
	 * words read back as 0xa/0xa -- this part mirrors word_len there -- and a
	 * read of an undocumented offset is exactly the class of access that took
	 * the whole phone off USB. */
	/* geni_se_select_fifo_mode() clears GENI_DMA_MODE_EN with a read-modify-write;
	 * a plain store of 0 would also drop whatever reserved bits the SE keeps there. */
	bt_wr(bt.se, SE_DMA_MODE, bt_rd(bt.se, SE_DMA_MODE) & ~1u);
	bt_wr(bt.se, SE_GENI_TX_WATERMARK_REG, 2u);
	bt_wr(bt.se, SE_GENI_RX_WATERMARK_REG, 2u);
	bt_wr(bt.se, SE_GENI_RX_RFR_WM_REG, 14u);
	/* geni_se_config_packing(&se, 8, 4, false, true, true): four 8-bit lanes per FIFO
	 * entry, LSB first, stop bit on the last vector.  Linux computes cfg0 0x4380e /
	 * cfg1 0xc3e0e for exactly these arguments and BYTE_GRAN 0 because bpw/16 is zero
	 * at 8 bits.  Programmed BEFORE the sequencer starts, which is the order
	 * qcom_geni_serial_port_setup() uses. */
	/* bit14 (16384) skips packing entirely: one A/B boot then says whether the
	 * four-lane packing is what turns a5 5a 3c c3 into fe a5. */
	if (bt.mask & 16384u) {
		printf("PANDORA BT: packing left alone (bit14); cfg0 %#x cfg1 %#x rx0 %#x rx1 %#x gran %#x\n",
		    bt_rd(bt.se, SE_GENI_TX_PACKING_CFG0), bt_rd(bt.se, SE_GENI_TX_PACKING_CFG1),
		    bt_rd(bt.se, SE_GENI_RX_PACKING_CFG0), bt_rd(bt.se, SE_GENI_RX_PACKING_CFG1),
		    bt_rd(bt.se, SE_GENI_BYTE_GRAN));
	} else {
		bt_wr(bt.se, SE_GENI_TX_PACKING_CFG0, 0x4380eu);
		bt_wr(bt.se, SE_GENI_TX_PACKING_CFG1, 0xc3e0eu);
		bt_wr(bt.se, SE_GENI_RX_PACKING_CFG0, 0x4380eu);
		bt_wr(bt.se, SE_GENI_RX_PACKING_CFG1, 0xc3e0eu);
		bt_wr(bt.se, SE_GENI_BYTE_GRAN, 0u);
		/* Read them back: the bring-up has only ever written these five words and
		 * never looked at what the SE kept. */
		printf("PANDORA BT: packing written, reads cfg0 %#x cfg1 %#x rx0 %#x rx1 %#x gran %#x\n",
		    bt_rd(bt.se, SE_GENI_TX_PACKING_CFG0), bt_rd(bt.se, SE_GENI_TX_PACKING_CFG1),
		    bt_rd(bt.se, SE_GENI_RX_PACKING_CFG0), bt_rd(bt.se, SE_GENI_RX_PACKING_CFG1),
		    bt_rd(bt.se, SE_GENI_BYTE_GRAN));
	}
	/* SE_UART_MANUAL_RFR 0x2ac: with the EN bit clear the hardware drives RFR from
	 * RX_RFR_WATERMARK, which is what Linux ends up with when CRTSCTS is off and RTS
	 * is asserted (qcom_geni_serial_set_mctrl writes 0).  The bootloader may have
	 * left EN|NOT_READY here, which tells the chip "stop sending" forever; print the
	 * old value so one boot proves which it was. */
	{
		uint32_t rfr = bt_rd(bt.se, SE_UART_MANUAL_RFR);
		bt_wr(bt.se, SE_UART_MANUAL_RFR, 0u);
		printf("PANDORA BT: manual rfr %#x -> %#x%s\n", rfr,
		    bt_rd(bt.se, SE_UART_MANUAL_RFR),
		    (rfr & UART_MANUAL_RFR_EN) ? " (was manual, hardware now drives RFR)" : "");
	}
	/* The secondary sequencer has to be told to read.  Without this the RX FIFO
	 * never fills, whatever the wires do (Linux geni_se_setup_s_cmd, a read-modify-
	 * write of the opcode and parameter fields). */
	bt_wr(bt.se, SE_GENI_S_CMD0,
	    (bt_rd(bt.se, SE_GENI_S_CMD0) & ~(0xf8000000u | 0x07ffffffu)) |
	    (1u << S_OPCODE_SHFT));
	/* qcom_geni_serial_start_rx_fifo() then arms the two RX interrupts.  We poll
	 * instead of taking the IRQ, but the watermark condition is the same one the
	 * FIFO uses to decide it has data, and leaving S_IRQ_EN at zero (as an earlier
	 * revision did) also leaves S_CMD_ACTIVE reporting nothing. */
	/* geni_se_select_fifo_mode() also latches the M side: CMD_DONE, TX
	 * watermark and the two RX conditions.  Nothing here takes the interrupt,
	 * but an unlatched status word reports zero forever, which reads exactly
	 * like a transfer that never happened. */
	bt_wr(bt.se, SE_GENI_M_IRQ_EN, bt_rd(bt.se, SE_GENI_M_IRQ_EN) |
	    M_CMD_DONE_EN | M_TX_FIFO_WATERMARK_EN |
	    M_RX_FIFO_WATERMARK_EN | M_RX_FIFO_LAST_EN);
	bt_wr(bt.se, SE_GENI_S_IRQ_EN,
	    bt_rd(bt.se, SE_GENI_S_IRQ_EN) | S_RX_FIFO_WATERMARK_EN | S_RX_FIFO_LAST_EN);
	printf("PANDORA BT: uart 8N1 at %u baud, m_clk_cfg %#x (div %u), s_cmd0 %#x, "
	    "tx_wm %#x rx_wm %#x\n", baud, ser, div & 0xfffu,
	    bt_rd(bt.se, SE_GENI_S_CMD0), bt_rd(bt.se, SE_GENI_TX_WATERMARK_REG),
	    bt_rd(bt.se, SE_GENI_RX_WATERMARK_REG));
	printf("PANDORA BT: frame tx_cfg %#x word_len %#x stop %#x rx_cfg %#x "
	    "rx_word_len %#x stale %#x\n",
	    bt_rd(bt.se, SE_UART_TX_TRANS_CFG), bt_rd(bt.se, SE_UART_TX_WORD_LEN),
	    bt_rd(bt.se, SE_UART_TX_STOP_BIT_LEN), bt_rd(bt.se, SE_UART_RX_TRANS_CFG),
	    bt_rd(bt.se, SE_UART_RX_WORD_LEN), bt_rd(bt.se, SE_UART_RX_STALE_CNT));
	printf("PANDORA BT: m_irq %#x tx_status %#x rx_status %#x status %#x\n",
	    bt_rd(bt.se, SE_GENI_M_IRQ_STATUS), bt_rd(bt.se, SE_GENI_TX_FIFO_STATUS),
	    bt_rd(bt.se, SE_GENI_RX_FIFO_STATUS), bt_rd(bt.se, SE_GENI_STATUS));
	bt_se_dump("after setup");
	return 0;
}

static int
bt_se_putword(uint32_t c)
{
	uint32_t st = 0;
	unsigned waited = 0;

	/* The FIFO holds 16 words and the sequencer drains one word (4 B) every
	 * ~350 us at 115200, so a full FIFO is empty in ~5.5 ms; 20 ms is 4x that.
	 * The old loop ran `t < 10000` turns of delay(1000) -- ten SECONDS -- which
	 * is why the first long frame wedged for 13.9 s (boot-yb). */
	/* Segment 0 (249 B) went out whole and segment 1 wedged with the TX FIFO
	 * parked at 16 words, while "rx sequencer still active" kept repeating. The
	 * secondary sequencer that owns the RX FIFO is not serviced here -- there is
	 * no interrupt handler -- and while it holds a non-empty FIFO the primary one
	 * stops fetching. Linux runs RX in its ISR concurrently, so it never sees
	 * this. Drain into a scratch buffer on every turn of the wait loop. */
	bt_rx_pump();        /* keep, do not discard, what the radio already sent */
	/* Occupancy, read once and acted on immediately. Do NOT wait on the
	 * M_TX_FIFO_WATERMARK_EN latch before the first word: the sequencer is
	 * already pulling (M_CMD0 went out above), so any delay here lets it read
	 * an empty FIFO and raise M_TX_FIFO_RD_ERR (bit 28) -- measured on boot YU,
	 * where a 20 ms pre-wait turned the 4 B loopback from pass into garbage
	 * (0c 97 6a f1). Only wait when the FIFO is genuinely full. */
	st = bt_rd(bt.se, SE_GENI_TX_FIFO_STATUS);
	while ((st & TX_FIFO_WC) >= 16u && waited < 20u) {
		bt_rx_pump();
		delay(1000);
		waited++;
		st = bt_rd(bt.se, SE_GENI_TX_FIFO_STATUS);
	}
	if ((st & TX_FIFO_WC) >= 16u) {
		static unsigned told;
		if (!told) {
			told = 1;
			printf("PANDORA BT: tx fifo full, status %#x (wc %u)\n",
			    st, st & TX_FIFO_WC);
		}
		return EBUSY;
	}
	bt_wr(bt.se, SE_GENI_TX_FIFO, c);
	/* Linux __qcom_geni_serial_console_write() clears this latch after every
	 * chunk (qgs.c:626): the watermark is a latched condition, and a latched
	 * one this side never services stops the sequencer mid-transfer. Segment 0
	 * (249 B) went out and segment 1 froze at 132 of 249 with wc pinned at 16,
	 * which is exactly that signature. */
	bt_wr(bt.se, SE_GENI_M_IRQ_CLEAR, M_TX_FIFO_WATERMARK_EN);
	return 0;
}

/* One H4 packet == one M command: Linux qcom_geni_serial_setup_tx() writes the
 * length and then the UART_START_TX opcode (bits 31:27) before pushing bytes, so
 * the sequencer knows where the transfer ends.  Splitting a packet across two
 * calls would kick the sequencer twice and truncate the first half. */
static int
bt_se_write(const uint8_t *p, size_t n)
{
	/* Let the previous command retire so its leftovers cannot shift out ahead.
	 *
	 * delay() takes MICROSECONDS and every turn of this loop sleeps 1000 of
	 * them, so the counter below is milliseconds. The budget used to be
	 * `2000 + n*120` read as microseconds but spent as milliseconds, i.e. 20-60
	 * SECONDS per segment -- 1320 segments could never finish a download
	 * (boot-yp). 8N1 is 10 bits per byte, so the wire time is n*10/baud: allow
	 * 4x that plus a 20 ms floor, cap at 500 ms. */
	static size_t rd_err_at;
	rd_err_at = 0;
	unsigned limit_ms = 20u + (unsigned)(((unsigned long long)n * 40000ull) /
	    (unsigned long long)(bt_baud ? bt_baud : BT_BAUD));
	if (limit_ms > 500u) {
		limit_ms = 500u;
	}
	unsigned waited = 0;
	for (; waited < limit_ms; waited++) {
		if (!(bt_rd(bt.se, SE_GENI_STATUS) & M_GENI_CMD_ACTIVE)) {
			break;
		}
		bt_rx_pump();
		delay(1000);
	}
	if (waited >= limit_ms) {
		/* Linux qcom_geni_serial_stop_tx_fifo() -> __qcom_geni_serial_cancel_tx_cmd():
		 * a command that will not retire has to be cancelled, not written over. The
		 * old code sailed on and re-armed M_CMD0 on top of a live command, which
		 * wedges the primary sequencer for good. */
		uint32_t ct = 0;
		bt_wr(bt.se, SE_GENI_M_CMD_CTRL_REG, M_GENI_CMD_CANCEL);
		for (ct = 0; ct < 2000u; ct++) {
			if (bt_rd(bt.se, SE_GENI_M_IRQ_STATUS) & M_CMD_CANCEL_EN) {
				break;
			}
			delay(1000);
		}
		bt_wr(bt.se, SE_GENI_M_IRQ_CLEAR, M_CMD_CANCEL_EN);
		printf("PANDORA BT: m command did not retire; cancel %s after %u ms, m_irq %#x\n",
		    ct < 2000u ? "acked" : "TIMED OUT", ct, bt_rd(bt.se, SE_GENI_M_IRQ_STATUS));
	}
	else if (waited > 20u) {
		/* Retiring normally should take about one frame; report the outliers so a
		 * regression in the retire path is visible instead of inferred. */
		printf("PANDORA BT: m command took %u ms to retire\n", waited);
	}
	bt_wr(bt.se, SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	/* Pump, never flush: during a download the previous segment's answer can
	 * still be arriving, and bt_rx_flush() would throw it away. */
	bt_rx_pump();
	bt_rx_restart();      /* arm the receiver before the chip can answer */
	/* Fill the FIFO BEFORE starting the command.
	 *
	 * Linux writes TRANS_LEN, starts the command and then pushes a whole
	 * tx_fifo_depth worth of words in one go (qcom_geni_serial_handle_tx_fifo ->
	 * setup_tx -> send_chunk_fifo), because its watermark ISR refills the FIFO
	 * for as long as the transfer runs. There is no ISR here, so with the start
	 * written first the sequencer begins pulling at once and, for the few
	 * instructions before our first word lands, reads an empty FIFO. Hardware
	 * records that as M_TX_FIFO_RD_ERR (bit 28) and stops the command: boot YQ
	 * shows exactly that pair, m_irq 0xd0000001 = CMD_DONE | TX_FIFO_RD_ERR |
	 * TX_FIFO_WATERMARK, on the very segment that never retired.
	 *
	 * So prime the FIFO first, then start. bit19 (524288) restores the old order
	 * for an A/B. */
	/* Linux qcom_geni_serial_handle_tx_fifo() order, verbatim: setup_tx() writes
	 * TRANS_LEN and starts the command, and send_chunk_fifo() pushes the words
	 * afterwards.  Boot YS tried the opposite (fill, then start) and the loopback
	 * regressed from a5 5a 3c c3 to fe 03 00 00, so filling first is wrong: the
	 * block drops whatever the FIFO held when the command starts.
	 *
	 * bit19 (524288) still selects the fill-then-start experiment for an A/B. */
	/* TRANS_LEN is a WORD count on this block, not a byte count.
	 *
	 * Boots YU/YV sent TRANS_LEN = n (bytes) and every transfer ended with
	 * CMD_DONE | M_TX_FIFO_RD_ERR (m_irq 0xd0000001): the sequencer was
	 * told 249 and given 63 words, so it kept asking for more and read an
	 * empty FIFO. bit21 (2097152) restores the byte count for an A/B. */
	bt_wr(bt.se, SE_UART_TX_TRANS_LEN,
	    (bt.mask & 2097152u) ? (uint32_t)n : (uint32_t)((n + 3u) / 4u));
	size_t primed = 0;
	if (bt.mask & 524288u) {
		const size_t words = (n + (size_t)BYTES_PER_FIFO_WORD - 1u) /
		    (size_t)BYTES_PER_FIFO_WORD;
		size_t prime_n = words > 16u ? 16u : words;

		for (size_t i = 0; i < prime_n * (size_t)BYTES_PER_FIFO_WORD && i < n;
		    i += 4) {
			uint32_t word = 0;
			for (size_t k = 0; k < 4 && i + k < n; k++) {
				word |= (uint32_t)p[i + k] << (8 * k);
			}
			bt_wr(bt.se, SE_GENI_TX_FIFO, word);
			primed += 4;
		}
		if (primed > n) {
			primed = n;
		}
		printf("PANDORA BT: tx primed %zu of %zu B before the start" "\n", primed, n);
	}
	bt_wr(bt.se, SE_GENI_M_CMD0, 1u << M_OPCODE_SHFT);
	/* Linux qcom_geni_serial_send_chunk_fifo() pushes BYTES_PER_FIFO_WORD (4)
	 * bytes as ONE iowrite32 to SE_GENI_TX_FIFOn.  Writing single bytes to
	 * 0x700 overwrote the same entry, so only the last byte of every H4
	 * command reached the wire. */
	/* Resume where priming stopped: the words already in the FIFO must not
	 * be pushed a second time. */
	for (size_t i = primed; i < n; i += 4) {
		uint32_t word = 0;
		for (size_t k = 0; k < 4 && i + k < n; k++) {
			word |= (uint32_t)p[i + k] << (8 * k);
		}
		int e = bt_se_putword(word);
		if (e) {
			printf("PANDORA BT: tx fifo stalled after %zu of %zu\n", i, n);
			return e;
		}
		/* bit20: name the byte where the block first records a FIFO read
		 * error. A sweep over frame lengths then separates "starved while
		 * refilling" (error late) from "race with the start" (error at 4). */
		if ((bt.mask & 1048576u) && !rd_err_at &&
		    (bt_rd(bt.se, SE_GENI_M_IRQ_STATUS) & M_TX_FIFO_RD_ERR_EN)) {
			rd_err_at = i + 4u;
			printf("PANDORA BT: tx rd-err first at byte %zu of %zu, m_irq %#x tx_status %#x status %#x\n",
			    rd_err_at, n, bt_rd(bt.se, SE_GENI_M_IRQ_STATUS),
			    bt_rd(bt.se, SE_GENI_TX_FIFO_STATUS), bt_rd(bt.se, SE_GENI_STATUS));
		}
	}
	return 0;
}

/* bit9: prove the SE itself can move bytes before blaming the radio.
 *
 * Every boot so far ends the same way: the TX sequencer drains (tx_status shows an
 * empty FIFO), M_GP_LENGTH counts the bytes out, and the RX FIFO stays at zero. That
 * is two very different faults wearing one face - either the UART inside the SE is
 * misconfigured (wrong packing, wrong frame, wrong sampling rate), or it is perfect
 * and the chip simply never answers. SE_UART_LOOPBACK_CFG (0x22c) separates them:
 * with the internal loopback closed, TX is fed back into RX without any pin or any
 * remote device, so a correct configuration must return the exact bytes sent.
 *
 * Linux only ever writes this register from qcom_geni_serial_set_termios()
 * (writel(port->loopback, SE_UART_LOOPBACK_CFG)), which is why it is the one field
 * the bring-up had never touched.
 *
 * Read-only with respect to the outside world: no pin, no radio, and the register is
 * put back to 0 before returning. */
static int
bt_se_loopback(void)
{
	static const uint8_t pattern[4] = {0xa5u, 0x5au, 0x3cu, 0xc3u};
	uint8_t back[16];
	unsigned got = 0;
	unsigned idle = 0;
	uint32_t was = bt_rd(bt.se, SE_UART_LOOPBACK_CFG);
	int rc = 0;

	printf("PANDORA BT: loopback: cfg was %#x, closing it with 3\n", was);
	printf("PANDORA BT: loopback: stale %#x rx_wm %#x rx_rfr %#x tx_trans_cfg %#x rx_trans_cfg %#x\n",
		bt_rd(bt.se, SE_UART_RX_STALE_CNT), bt_rd(bt.se, SE_GENI_RX_WATERMARK_REG),
		bt_rd(bt.se, SE_GENI_RX_RFR_WM_REG), bt_rd(bt.se, SE_UART_TX_TRANS_CFG),
		bt_rd(bt.se, SE_UART_RX_TRANS_CFG));
	/* Linux only ever writes RX_TX_CTS_RTS_SORTED (3) or 0 here (qgs.c:262-263).
	 * Writing 1 (RX_TX_SORTED alone) is a combination the driver never uses, and it is
	 * the prime suspect for the 2-of-4-byte loopback. bit15 restores 1 for an A/B. */
	uint32_t lbt = (bt.mask & 32768u) ? 1u : 3u;
	bt_wr(bt.se, SE_UART_LOOPBACK_CFG, lbt);
	/* Enabling the loopback makes the block echo one word of its own (0x3fe in
	 * every log) a few microseconds later; flushing before the write only
	 * catches what was already parked.  Settle first, then flush. */
	delay(2000);
	bt_rx_flush();

	rc = bt_se_write(pattern, sizeof(pattern));
	if (rc) {
		printf("PANDORA BT: loopback: write failed %d\n", rc);
		goto restore;
	}
	while (got < sizeof(back) && idle < 2000) {
		size_t n = bt_rx_drain(&back[got], sizeof(back) - got);
		if (n) {
			got += (unsigned)n;
			idle = 0;
			continue;
		}
		delay(1000);
		idle++;
	}
	printf("PANDORA BT: loopback: rx_fifo_status %#x rx_fifo_raw %#x\n",
	    bt_rd(bt.se, SE_GENI_RX_FIFO_STATUS), bt_rd(bt.se, SE_GENI_RX_FIFO));
	printf("PANDORA BT: loopback: sent %u B, got %u B:", (unsigned)sizeof(pattern), got);
	for (unsigned i = 0; i < got && i < 16; i++) {
		printf(" %02x", (unsigned)back[i]);
	}
	unsigned at = 0xffffffffu;
	for (unsigned i = 0; i + sizeof(pattern) <= got; i++) {
		if (memcmp(&back[i], pattern, sizeof(pattern)) == 0) {
			at = i;
			break;
		}
	}
	if (at != 0xffffffffu) {
		printf(" -> PASS: all 4 bytes came back (pattern at offset %u); a silent radio is the chip's fault\n", at);
		rc = 0;
	} else if (got == 0) {
		printf(" -> FAIL: nothing came back; packing, frame format or the sampling "
		    "rate is still wrong, the radio was never asked\n");
		rc = EIO;
	} else {
		printf(" -> FAIL: %u of %u bytes and they differ; the frame format is "
		    "length-mismatched\n", got, (unsigned)sizeof(pattern));
		rc = EIO;
	}
restore:
	bt_wr(bt.se, SE_UART_LOOPBACK_CFG, 0u);
	printf("PANDORA BT: loopback: cfg restored to 0 (was %#x)\n", was);
	return rc;
}

/* Ask the bootloader what it is, the way hci_qca.c does before anything else.
 *
 * qca_setup() -> qca_power_on() -> qca_read_soc_version(): the first command on the
 * wire is vendor opcode 0xFC00 carrying EDL_PATCH_VER_REQ_CMD (0x19), and the answer
 * is the BTFW banner this part reports as its SoC version.  Android prints that same
 * banner ("bt_ioctl: unified Current SOC Version : 40210200").  Opening with a
 * generic HCI command instead assumes an already-patched controller, which a cold
 * boot of this part may not be.
 */
#define BT_EDL_PATCH_OPCODE 0xfc00u
#define BT_EDL_VER_REQ      0x19u
/* btqca.h, the only published transcription of this vendor protocol:
 *   #define EDL_PATCH_CMD_OPCODE   0xFC00
 *   #define EDL_PATCH_CMD_LEN      1
 *   #define EDL_PATCH_VER_REQ_CMD  0x19
 *   #define EDL_PATCH_TLV_REQ_CMD  0x1E
 * The opcode is constant; the sub-command is the FIRST PARAMETER BYTE.
 * btqca.c never ORs it into the opcode:
 *   cmd = EDL_PATCH_VER_REQ_CMD;
 *   __hci_cmd_sync_ev(hdev, EDL_PATCH_CMD_OPCODE, EDL_PATCH_CMD_LEN, &cmd, ...);
 *   cmd[0] = EDL_PATCH_TLV_REQ_CMD; cmd[1] = seg_size; memcpy(cmd + 2, data, seg_size);
 *   __hci_cmd_sync_ev(hdev, EDL_PATCH_CMD_OPCODE, seg_size + 2, cmd, ...);
 * The first build of this probe sent 0xFC19 as the opcode with no parameters,
 * which this part has no reason to answer -- that result is void. */
#define BT_EDL_TLV_REQ      0x1eu

/* bt_h4_poll() and bt_fw_load() are defined further down; the vendor probe and the
 * download gate sit above them, so declare the two here. */
static int bt_h4_poll(uint8_t *buf, size_t cap, unsigned timeout_ms);
static uint8_t *bt_fw_load(const char *name, uint32_t *out_len);

/* The first command hci_qca.c sends after power-on: opcode 0xFC00, one
 * parameter byte 0x19. WCN6750-class parts answer it out of the ROM with a
 * vendor event carrying product id / soc version / rom / patch words, which is
 * also what Android logs as "bt_ioctl: unified Current SOC Version : 40210200".
 * A part that never answers this is not running its ROM console either. */
static void
bt_vendor_soc_version(void)
{
	uint8_t cmd[5];
	uint8_t ev[128];
	/* bt_h4_poll() returns a NEGATIVE errno on timeout; a size_t turned -60
	 * into a gigantic positive count, so the hex dump below printed 32 bytes of
	 * uninitialised stack and the download judged every segment "acked". */
	int got;
	int e;

	cmd[0] = H4_CMD;
	cmd[1] = (uint8_t)(BT_EDL_PATCH_OPCODE & 0xffu);
	cmd[2] = (uint8_t)(BT_EDL_PATCH_OPCODE >> 8);
	cmd[3] = 1u;                         /* EDL_PATCH_CMD_LEN */
	cmd[4] = BT_EDL_VER_REQ;             /* the sub-command, as a parameter */
	e = bt_se_write(cmd, sizeof(cmd));
	if (e) {
		printf("PANDORA BT: vendor %#06x/%#04x write failed %d\n",
		    BT_EDL_PATCH_OPCODE, BT_EDL_VER_REQ, e);
		return;
	}
	got = bt_h4_poll(ev, sizeof(ev), 3000u);
	printf("PANDORA BT: vendor %#06x/%#04x -> %d bytes %s\n",
	    BT_EDL_PATCH_OPCODE, BT_EDL_VER_REQ, got,
	    got > 0 ? ":" : "(the ROM console is silent)");
	for (int i = 0; i < got && i < 32; i++) {
		printf(" %02x", (unsigned)ev[i]);
	}
	if (got) {
		printf("\n");
	}
	if (got >= 7 && ev[0] == H4_EVT) {
		printf("PANDORA BT: vendor banner: ");
		for (int i = 6; i < got; i++) {
			printf("%c", (ev[i] >= 0x20u && ev[i] < 0x7fu) ? ev[i] : 0x2eu);
		}
		printf("\n");
	}
}
static int
bt_hci_cmd(uint16_t op, const uint8_t *par, uint8_t plen, uint8_t *ev, size_t evlen)
{
	/* Header and parameters travel as ONE M command (see bt_se_write). */
	uint8_t h4[4 + 255];
	h4[0] = H4_CMD;
	h4[1] = (uint8_t)(op & 0xffu);
	h4[2] = (uint8_t)(op >> 8);
	h4[3] = plen;
	if (plen) {
		memcpy(&h4[4], par, plen);
	}
	int e = bt_se_write(h4, (size_t)4 + plen);
	if (e) {
		return e;
	}
	printf("PANDORA BT: after tx op %#06x: m_irq %#x tx_status %#x tx_fifo %#x s_irq %#x rx_status %#x\n",
	    op, bt_rd(bt.se, SE_GENI_M_IRQ_STATUS), bt_rd(bt.se, SE_GENI_TX_FIFO_STATUS),
	    bt_rd(bt.se, SE_GENI_TX_FIFO), bt_rd(bt.se, SE_GENI_S_IRQ_STATUS),
	    bt_rd(bt.se, SE_GENI_RX_FIFO_STATUS));
	/* Linux qcom_geni_serial_poll_tx_done() waits for SE_GENI_M_GP_LENGTH to reach
	 * the byte count it queued: a short count means the sequencer stalled mid packet,
	 * a full count with an empty FIFO means the bytes really left the SE and the chip
	 * stayed silent. Report both so the next boot separates the two. */
	printf("PANDORA BT: tx trans_len %#x m_gp_length %#x s_gp_length %#x "
	    "m_cmd_ctrl %#x\n", bt_rd(bt.se, SE_UART_TX_TRANS_LEN),
	    bt_rd(bt.se, SE_GENI_M_GP_LENGTH), bt_rd(bt.se, SE_GENI_S_GP_LENGTH),
	    bt_rd(bt.se, SE_GENI_M_CMD_CTRL_REG));
	size_t got = 0;
	unsigned idle = 0;
	/* The chip's TX pad is readable without the SE, so watching it separates
	 * "the radio never spoke" from "our receiver could not hear it" -- the
	 * two have been indistinguishable while loopback is still lossy. */
	unsigned edges = 0, high = 0, samples = 0;
	/* RX_DATA_IN (SE_GENI_IOS bit 0), not the TLMM input bit: with pin 27 muxed
	 * to qup3_se5_l3 the pad is owned by the SE and the TLMM readback does not
	 * follow the wire (16 boots: two highs, the rest low, same image). */
	uint32_t pad = bt_rd(bt.se, SE_GENI_IOS) & 1u;
	while (got < evlen && idle < 5000) {
		size_t n = bt_rx_drain(&ev[got], evlen - got);
		if (n) {
			got += n;
			idle = 0;
			continue;
		}
		{
			uint32_t now = bt_rd(bt.se, SE_GENI_IOS) & 1u;
			if (now != pad) {
				++edges;
				pad = now;
			}
			if (now) {
				++high;
			}
			++samples;
		}
		delay(1000);
		idle++;
	}
	printf("PANDORA BT: hci rx pad (IOS RX_DATA_IN) edges %u high %u of %u samples, final %u\n",
	    edges, high, samples, pad);
	printf("PANDORA BT: hci op %#06x -> %zu bytes:", op, got);
	for (size_t i = 0; i < got && i < 24; i++) {
		printf(" %02x", ev[i]);
	}
	printf("\n");
	if (!got) {
		return ETIMEDOUT;
	}
	/* "Some bytes came back" is not the same as "the controller answered":
	 * the caller cannot tell an event from line noise otherwise. */
	if (ev[0] != H4_EVT) {
		printf("PANDORA BT: hci op %#06x: answer is not an event (type %#x)\n",
		    op, ev[0]);
		return EPROTO;
	}
	return 0;
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
#define BT_TLV_SEG_HDR     2u          /* 1 B sub-command + 1 B size, per btqca.c   */


/* One file out of the pinned pack into a kalloc_data buffer; NULL and *out_len 0
 * when the pack or the entry is missing. Same directory walk as pandora_amp.c:363. */
/* FW_BT: index of the bluetooth partition in the volume table of pandora_adsp.c. */
#define FW_BT 3

static bool bt_want_pack(void)
{
	char how[8] = "";

	return PE_parse_boot_arg_str("pandora_bt_fw", how, sizeof(how)) && !strcmp(how, "pack");
}

/* One firmware file from the bluetooth partition, or NULL when it is not there. */
static uint8_t *
bt_fw_from_part(const char *name, uint32_t *out_len)
{
	char path[64];
	uint64_t len = 0;
	uint8_t *buf;
	int e;

	snprintf(path, sizeof(path), "image/%s", name);
	e = pandora_fw_volume_size(FW_BT, path, &len);
	if (e) {
		printf("PANDORA BT: fw: %s not in the bluetooth partition (%d)\n", name, e);
		return NULL;
	}
	if (!len || len > BT_FW_MAX) {
		printf("PANDORA BT: fw: %s has unusable size %llu\n", name, len);
		return NULL;
	}
	buf = kalloc_data((vm_size_t)len, Z_WAITOK);
	if (!buf) {
		printf("PANDORA BT: fw: cannot allocate %llu bytes\n", len);
		return NULL;
	}
	e = pandora_fw_volume_read(FW_BT, path, 0, buf, len);
	if (e) {
		printf("PANDORA BT: fw: %s read failed %d\n", name, e);
		kfree_data(buf, (vm_size_t)len);
		return NULL;
	}
	printf("PANDORA BT: fw: %s %llu bytes from the bluetooth partition\n", name, len);
	*out_len = (uint32_t)len;
	return buf;
}

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
	if (!bt_want_pack()) {
		buf = bt_fw_from_part(name, out_len);
		if (buf) {
			return buf;
		}
		printf("PANDORA BT: fw: falling back to the pinned pack\n");
	}
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
/* One TLV segment command: opcode 0xFC00, then [0x1E, size, data...].
 * btqca.c qca_tlv_send_segment() uses exactly this layout and waits for one
 * event per segment unless the blob asks for the "skip event" download mode. */
static int
bt_edl_send_segment(const uint8_t *data, uint32_t n)
{
	uint8_t buf[4u + 2u + BT_TLV_SEG_DATA];
	uint8_t ev[64];
	int got;                   /* negative errno on timeout, see bt_h4_poll() */
	int e;

	if (n > BT_TLV_SEG_DATA) {
		return EINVAL;
	}
	buf[0] = H4_CMD;
	buf[1] = (uint8_t)(BT_EDL_PATCH_OPCODE & 0xffu);
	buf[2] = (uint8_t)(BT_EDL_PATCH_OPCODE >> 8);
	buf[3] = (uint8_t)(n + 2u);          /* plen = sub-command + size + data */
	buf[4] = BT_EDL_TLV_REQ;
	buf[5] = (uint8_t)n;
	memcpy(&buf[6], data, n);
	e = bt_se_write(buf, 6u + n);
	if (e) {
		printf("PANDORA BT: dl: segment write failed %d\n", e);
		return e;
	}
	/* The ROM has never answered (vendor 0xFC00/0x19 -> 0 bytes on every boot),
	 * so waiting a full second per segment only turns 1320 segments into half an
	 * hour of silence. 100 ms is still >> the 0.87 ms a 245 B frame needs at
	 * 115200 8N1, so a real answer cannot be missed. */
	got = bt_h4_poll(ev, sizeof(ev), 100u);
	if (got <= 0) {
		return ETIMEDOUT;
	}
	return 0;
}

/* The vendor download. Faithful to btqca.c qca_download_firmware(): the WHOLE
 * file, TLV container and patch header included, is walked in
 * MAX_SIZE_PER_TLV_SEGMENT (243 B) chunks -- not just the patch payload. For
 * gngbtfw20.tlv that is 320,728 B in 1,320 commands. The blob's download_mode is
 * 3, which matches none of btqca.h's skip-event values, so Linux would wait for
 * one event per segment; so does this. */
static int
bt_fw_download(const uint8_t *tlv, uint32_t len)
{
	uint32_t off = 0;
	unsigned seg = 0;
	unsigned quiet = 0;
	int e = 0;
	/* pandora_btdl=N stops after N segments. A full run is 1320 commands and,
	 * with a silent ROM, nothing but time; the question this experiment has to
	 * answer is whether the sequencer keeps advancing now that the TX watermark
	 * latch is cleared. Eight segments is enough to see that, and it fits in one
	 * auto-reset window. */
	unsigned segmax = 8;
	(void)PE_parse_boot_argn("pandora_btdl", &segmax, sizeof(segmax));

	printf("PANDORA BT: dl: %u B in %u B segments -> %u commands\n",
	    len, BT_TLV_SEG_DATA, (len + BT_TLV_SEG_DATA - 1u) / BT_TLV_SEG_DATA);
	while (off < len && seg < segmax) {
		uint32_t n = len - off;

		if (n > BT_TLV_SEG_DATA) {
			n = BT_TLV_SEG_DATA;
		}
		e = bt_edl_send_segment(tlv + off, n);
		if (e) {
			printf("PANDORA BT: dl: segment %u at +%u (%u B) -> %d; stopping\n",
			    seg, off, n, e);
			return e;
		}
		printf("PANDORA BT: dl: segment %u at +%u (%u B) acked; m_irq %#x tx_status %#x \n"
		    "    rx_status %#x m_gp_length %#x s_irq %#x\n",
		    seg, off, n, bt_rd(bt.se, SE_GENI_M_IRQ_STATUS),
		    bt_rd(bt.se, SE_GENI_TX_FIFO_STATUS), bt_rd(bt.se, SE_GENI_RX_FIFO_STATUS),
		    bt_rd(bt.se, SE_GENI_M_GP_LENGTH), bt_rd(bt.se, SE_GENI_S_IRQ_STATUS));
		quiet = 0;
		off += n;
		seg++;
	}
	printf("PANDORA BT: dl: stopped after %u segments, %u of %u B\n", seg, off, len);
	return e;
}
/* H4 send: one type byte, then the packet. 0x01 command, 0x02 ACL, 0x04 event.
 * Kept apart from bt_hci_cmd() so bit 2's path stays byte-for-byte unchanged. */
static int
bt_h4_send(uint8_t type, const void *pkt, size_t len)
{
	/* One H4 packet == one M command (type byte included), same rule bt_hci_cmd
	 * follows: the sequencer's TX_TRANS_LEN must count the whole frame. */
	uint8_t h4[4 + 255];
	if (len > sizeof(h4) - 1u) {
		return EMSGSIZE;
	}
	h4[0] = type;
	memcpy(&h4[1], pkt, len);
	return bt_se_write(h4, len + 1u);
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
		size_t n = bt_rx_drain(&buf[got], cap - got);
		if (!n) {
			if (idle++ >= timeout_ms) {
				break;
			}
			delay(1000);
			continue;
		}
		got += n;
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
	static const char *want[] = {"gngbtfw20.tlv", "gngbtfw20.mbn", "gngbtnv20.bin",
		"gngbcscal20.bin"};
	int e;

	if (!bt_want_pack()) {
		char path[64];
		uint64_t got = 0;

		for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
			snprintf(path, sizeof(path), "image/%s", want[i]);
			if (pandora_fw_volume_size(FW_BT, path, &got)) {
				printf("PANDORA BT: %s is not in the bluetooth partition\n", want[i]);
			} else {
				printf("PANDORA BT: %s %llu bytes in the bluetooth partition\n",
				    want[i], got);
			}
		}
	}
	e = pandora_fw_read(0, dir, sizeof(dir));
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
	/* The WCN block's BT/WLAN select.  Android's own dmesg on this phone
	 * (btpower.ko, dmesg -t) is explicit, and it repeats it in both halves
	 * of the sequence:
	 *   BTON:Turn Bt OFF bt-sw-ctrl-gpio(530) value(1)
	 *   BTON: Turn BT ON bt-sw-ctrl-gpio(530) value(1)
	 * 530 - 512 (gpiochip0 base, parent f000000.pinctrl) is pin 18, and
	 * "value(1)" is the level it is driven to.  Upstream hci_qca.c only
	 * reads this line because upstream carries no ganges/peach code; the
	 * vendor btpower.ko drives it.  Left low, or muxed away, it selects
	 * WLAN on the shared UART -- which is exactly the signature measured
	 * here: the radio is powered (its TX pad idles high) and never answers
	 * one HCI byte.  Bit 11 keeps the old experiment (mux to wcn_sw_ctrl
	 * index 2, oe 0) so the two can still be compared from cmdline. */
	if (bt.mask & 2048u) {
		bt_gpio(BT_PIN_SWCTRL, BT_FUNC_SWCTRL, 0, 2, -1);
		printf("PANDORA BT: pin 18 muxed to wcn_sw_ctrl (index 2), oe left 0\n");
	} else {
		bt_gpio(BT_PIN_SWCTRL, BT_FUNC_GPIO, 0, 2, 1);
		printf("PANDORA BT: pin 18 driven high as GPIO (bt-sw-ctrl; Android drives 1)\n");
	}
	bt_gpio(BT_PIN_TX, BT_FUNC_UART, 1, 2, 1);
	bt_gpio(BT_PIN_RX, BT_FUNC_UART_RX, 3, 2, -1);
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
	if (bt.mask & 256u) {
		/* bit8: vote the RPMh rails up first.  btpower.ko enables the supplies
		 * before it drives resetb/wl-reset/BT_EN, so a chip that is otherwise
		 * wired correctly still answers nothing when the rails are down.  Off by
		 * default: this writes RPMh TCS slots, which the host firmware also uses. */
		bt_rails_up();
		bt_sleep(20);
	}
	if (bt.mask & 16u) {
		/* bit4: drive BT_EN (PMIC GPIO 5) ourselves instead of assuming
		 * the bootloader left the radio powered.
		 *
		 * hci_qca.c:1812 does not simply assert bt_en: it drives it LOW, waits
		 * 50 ms, drives it HIGH and waits another 50 ms, because the chip needs a
		 * clean edge to leave reset.  A level that was already high produces no
		 * edge at all, which is the state a warm boot leaves behind. */
		if (bt.mask & 32u) {
			e = bt_pmic_gpio5(0u);
			printf("PANDORA BT: BT_EN pulse low -> %d\n", e);
			bt_sleep(50);
		}
		if (bt.mask & 128u) {
			/* bit7: also toggle wl-reset (TLMM 16), the other pin
			 * btpower.ko drives during WLAN/BT shared-rail bring-up.  Off by
			 * default because it is the WLAN reset line. */
			printf("PANDORA BT: wl-reset (TLMM 16) low/high\n");
			bt_gpio(16u, BT_FUNC_GPIO, 1, 2, 0);
			bt_sleep(50);
			bt_gpio(16u, BT_FUNC_GPIO, 1, 2, 1);
			bt_sleep(50);
		}
		e = bt_pmic_gpio5(1u);
		if (e) {
			printf("PANDORA BT: BT_EN write failed %d; leaving the radio off\n", e);
		} else {
			printf("PANDORA BT: BT_EN driven 1 over SPMI (PPID %#x)\n", BT_EN_PPID);
			if (bt.mask & 1024u) {
				bt_pad_watch("just after BT_EN", 100u, 20u);
			}
			if (bt.mask & 1024u) {
				bt_sleep(5000);
				bt_pad_watch("5 s after BT_EN", 100u, 20u);
			}
			bt_sleep(50);
			bt_pins_dump("after BT_EN");
			printf("PANDORA BT: se after BT_EN: status %#x s_irq %#x s_irq_en %#x rx_status %#x\n",
			    bt_rd(bt.se, SE_GENI_STATUS), bt_rd(bt.se, SE_GENI_S_IRQ_STATUS),
			    bt_rd(bt.se, SE_GENI_S_IRQ_EN), bt_rd(bt.se, SE_GENI_RX_FIFO_STATUS));
		}
	} else {
		printf("PANDORA BT: BT_EN is pmh0104@9 GPIO 5, not driven (bit4 clear)\n");
	}
	bt_pins_dump("before UART");
	bt_sleep(80);
	if (!(bt.mask & 4u)) {
		printf("PANDORA BT: bit2 clear, not bringing the UART up\n");
		goto out;
	}
	e = bt_se_probe();
	if (e) {
		printf("PANDORA BT: SE probe failed %d\n", e);
		goto out;
	}
	if (bt.mask & 512u) {
		/* bit9: internal loopback, no pin and no radio involved. Run it before the
		 * first HCI command so a silent chip can be told apart from a deaf SE. */
		(void)bt_se_loopback();
	}
	if (bt.mask & 8192u) {
		/* bit13: the vendor SoC-version probe, which is what hci_qca.c sends first. */
		bt_vendor_soc_version();
	}
	if (bt.mask & 131072u) {
		/* bit17: the ROM download itself. btqca.c sends this BEFORE any generic
		 * HCI command, because a part that has not been patched is still in its
		 * ROM console and does not answer HCI at all. */
		uint32_t flen = 0;
		uint8_t *fw = bt_fw_load("gngbtfw20.tlv", &flen);

		if (fw) {
			(void)bt_fw_download(fw, flen);
			kfree_data(fw, (vm_size_t)flen);
		}
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
	unsigned on = 0, slow = 0, stride = BT_SE_STRIDE;
	if (!PE_parse_boot_argn("pandora_bt", &on, sizeof(on)) || !on) {
		return;
	}
	(void)PE_parse_boot_argn("pandora_btslow", &slow, sizeof(slow));
	if (PE_parse_boot_argn("pandora_btse", &stride, sizeof(stride))) {
		if (stride < 4 || stride > 0x400) {
			stride = BT_SE_STRIDE;
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
