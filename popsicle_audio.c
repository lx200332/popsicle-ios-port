/*
 * popsicle_audio.c -- Xiaomi 17 Pro Max (popsicle, SM8850 "canoe") audio support.
 *
 * Two halves, both gated by boot-args and both default-off, exactly like
 * popsicle_wifi.c:
 *
 *   1. audio_probe=1   READ-ONLY survey (unchanged since the first revision):
 *      TLMM pin states left by XBL/ABL, the GENI serial-engine status of the
 *      Cirrus I2C bus, and the GCC clock state. No bus transactions are issued
 *      and no pin is written -- the I2C register map of the CS35L43 (closed
 *      firmware handshake) and the AudioReach GPR client are the real driver's
 *      job.
 *
 *   2. audio_pinmux / audio_geniprobe / audio_pipe  (new, WRITE path):
 *      audio_pinmux=1   mux the four TDM0 pads to their I2S function and pulse
 *                       the WCD9395 reset on GPIO162; every write is read back
 *                       and printed with " OK" or " MISMATCH" -- the
 *                       popsicle_wifi.c:146-168 shape.
 *      audio_geniprobe=1 read the GENI SE identity words of i2c@a8c000 and
 *                       decide whether the bootloader already left that engine
 *                       usable. Read-only; no I2C controller driver here.
 *      audio_pipe=1     call audio_dma_setup()/audio_irq_register(), which are
 *                       honest REFUSALS: they print why and return ENOTSUP.
 *                       No DMA channel and no interrupt is faked.
 *
 * Hardware facts (audio-facts.md, DTB + real dmesg):
 *   - WCD9395 split codec: control I2C i2c@a8c000 @0xe (RST GPIO162), RX/TX over
 *     SoundWire (masters at 0x6ad0000 / 0x7630000).
 *   - CS35L43 x2 + CS40L26A on i2c-13 = i2c@1980000 (qupv3_3 GENI engine, the same
 *     engine whose SPI shape carries the GT9615 second-panel touch):
 *       cs35l43@40  addr 0x28 (reg 0x40), RST GPIO10,  GIC SPI 11,  prefix "B"
 *       cs35l43@42  addr 0x2a (reg 0x42), RST GPIO54,  GIC SPI 47,  prefix "T",
 *                   plus GPIO174 earpiece switch
 *       cs40l26@43  addr 0x43,            RST GPIO9 (active high), GIC SPI 80
 *   - All three Cirrus share TDM0 (48kHz/32bit/4slot, dmesg hw_params channels=3):
 *       SCK GPIO122, WS GPIO125, SD0 GPIO123, SD1 GPIO124 (base-tree pinctrl).
 *   - The ADSP speaks AudioReach GPR, NOT redwood's APR (ASM/ADM/AFE): that part of
 *     redwood_sound.c cannot be reused. QEMU has no audio hardware here; the virtio
 *     sound driver stays the QEMU path.
 *
 * Survey steps (all reads, no writes):
 *   step 1 - TLMM config + level of every audio GPIO above;
 *   step 2 - GENI engine status words of i2c@1980000 (0x1980000);
 *   step 3 - the GCC wrap3 CBCR at 0x52018 (which bits XBL left voted).
 */

#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>	/* ENOTSUP for the honest stubs */
#include <sys/systm.h>
#include <vm/pmap.h>	/* VM_WIMG_IO for ml_io_map_unmappable */

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, uint32_t wimg);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern kern_return_t thread_terminate(thread_t thread);

void popsicle_audio_init(void);	/* called from sweet_ufs.c; prototype keeps -Wmissing-prototypes quiet */
int audio_dma_setup(void);	/* honest stub -- returns ENOTSUP, see the body */
int audio_irq_register(void);	/* honest stub -- returns ENOTSUP, see the body */

/* ------------------------------------------------------------------ board facts */

#define TLMM_PHYS               0x0f100000UL    /* TRUE base: pinctrl-canoe.ko pin table = 0x100000 + 0x1000*n (wsl/pandora_touch.c:8-10); DT /soc/pinctrl@f000000 reg=<0x0f000000 0x202000> is the pre-offset base. 0x0f0xxxxx hangs the bus. */
#define TLMM_SIZE               0x00210000UL    /* >= DT 0x202000, covers 132 pins at 0x1000 stride */
#define GENI_I2C_PHYS           0x01980000UL    /* i2c@1980000, the qupv3_3 engine (Cirrus trio) */
#define GENI_I2C_SIZE           0x00004000UL
#define GCC_PHYS                0x00100000UL    /* /soc/clock-controller@100000 */
#define GCC_SIZE                0x001f4200UL    /* DT /soc/clock-controller@100000 reg size (fdtget 0x1f4200); 0x10000/0x60000 was smaller than the 0x52018 / 0xa90a8 offsets actually read */

/* i2c@a8c000 -- the WCD9395 control bus, and a DIFFERENT GENI engine from the
 * 0x1980000 one step 2 surveys: canoe_base.dts:16858-16883 (qupv3_1, alias i2c3,
 * status "ok", child wcd939x_i2c@e) versus canoe_base.dts:17352-17370
 * (i2c@1980000, alias i2c13, status "disabled"). Same 0x4000 window. */
#define GENI_I2C_AUDIO_PHYS     0x00a8c000UL
#define GENI_I2C_AUDIO_SIZE     0x00004000UL

#define GPIO_CIRRUS_B_RST       10
#define GPIO_CIRRUS_T_RST       54
#define GPIO_CS40L26_RST        9
#define GPIO_WCD9395_RST        162
#define GPIO_TDM_SCK            122
#define GPIO_TDM_WS             125
#define GPIO_TDM_SD0            123
#define GPIO_TDM_SD1            124
#define GPIO_EARPIECE_SW        174

#define TLMM_CFG(pin)           ((pin) * 0x1000)
#define TLMM_IO(pin)            ((pin) * 0x1000 + 4)

/* GCC wrap3 branch-enable CBCR shared by the qupv3_3 engine (from the touch work:
 * gcc-kaanapali.c; this is the same engine family as the GT9615 SPI) */
#define GCC_CBCR_WRAP3          0x52018UL

/* TLMM access, popsicle_wifi.c:100-109. Written as integer offsets off a mapped
 * window so no pointer arithmetic over a byte array is involved -- an indexed
 * walk trips -Wunsafe-buffer-usage (popsicle_wifi.c:143-145). */
#define TLMM_RD32(base, off)    (*(volatile uint32_t *)((base) + (vm_offset_t)(off)))
#define TLMM_WR32(base, off, value) \
	(*(volatile uint32_t *)((base) + (vm_offset_t)(off)) = (uint32_t)(value))

#define TLMM_CFG_MASK           0x3ffu
#define TLMM_PULL_NONE          0u
#define TLMM_FUNC_GPIO          0u
#define TLMM_CTL(pull, func, ma, oe) \
	((uint32_t)(pull) | ((uint32_t)(func) << 2) | \
	    ((uint32_t)((ma) / 2u - 1u) << 6) | ((oe) ? (1u << 9) : 0u))
#define TLMM_IO_LOW             0u
#define TLMM_IO_HIGH            2u
/* A pad whose config bit 11 reads back set is an eGPIO owned by the always-on
 * block; pinctrl-msm claims it for the TLMM by also setting bit 12, otherwise
 * the mux write lands and the pad still belongs to the other block.
 * pandora_adsp.c:1659-1662 does exactly this for the same four TDM0 pads. */
#define TLMM_EOGPIO             0x800u
#define TLMM_EOGPIO_CLAIM       0x1000u

/* TDM0 mux values. The DT carries the pin *function name* only -- tdm0_clk_active
 * says function = "i2s0_sck" (canoe_base.dts:4336-4347), tdm0_ws_active
 * "i2s0_ws" (:4363-4374), tdm0_sd0_active "i2s0_data0" (:4390-4401),
 * tdm0_sd1_active "i2s0_data1" (:4417-4428) -- and never a number; the number
 * lives in Linux's pinctrl-kaanapali.c, which is not on this machine. So
 * TLMM_FUNC_I2S0 = 1 is an UNDECOMPILED-DT PLACEHOLDER, taken from the one
 * implementation that really muxed these four pads on this board:
 * pandora_adsp.c:1412 ("function 1", 8 mA, no pull) and pandora_adsp.c:1656
 * ((1u << 2) | ((8u / 2u - 1u) << 6)). It is confirmed only by readback below,
 * never by a datasheet. drive-strength and bias DO come from the DT: every
 * tdm0_*_active config has drive-strength = <0x8> (8 mA) and bias-disable. */
#define TLMM_FUNC_I2S0          1u
#define TDM0_DRIVE_MA           8u
/* GPIO162 reset: popsicle_overlay.dts:23821-23843 -- active = output-high,
 * sleep = output-low, function "gpio", drive-strength = <0x10> (16 mA). */
#define WCD9395_RST_DRIVE_MA    16u

/* GENI serial-engine words, pandora_bt.c:106-131 (Linux qcom-geni-se.h). */
#define SE_GENI_STATUS          0x040u
#define SE_IF_DISABLE           0x064u
#define SE_FW_REV               0x068u
#define SE_CLK_SEL              0x07cu
#define SE_DMA_MODE             0x258u
#define SE_GENI_M_CMD0          0x600u
#define SE_GENI_M_CMD_CTRL      0x604u
#define SE_GENI_M_IRQ_STATUS    0x610u
#define SE_HW_PARAM0            0xe24u
#define SE_HW_PARAM1            0xe28u

#define GENI_PROTO_SPI          0x1u
#define GENI_PROTO_I2C          0x2u	/* pandora_bt.c:318 */

static vm_offset_t tlmm, geni, gcc;

/* The write path maps its own windows: the survey thread's globals are left
 * exactly as they were, so audio_probe=1 keeps behaving byte for byte. */
static vm_offset_t audio_tlmm, audio_geni_i2c;
static int audio_gate_pinmux, audio_gate_geniprobe, audio_gate_pipe;

static void
audio_survey_print(const char *what, uint32_t value)
{
	printf("popsicle_audio: %s = 0x%x\n", what, value);
}

static uint32_t
tlmm_read_cfg(unsigned pin)
{
	return *(volatile uint32_t *)(tlmm + TLMM_CFG(pin));
}

static uint32_t
tlmm_read_io(unsigned pin)
{
	return *(volatile uint32_t *)(tlmm + TLMM_IO(pin));
}

/* Step 1: the pins, exactly as XBL/ABL left them. */
static void
audio_step1_pins(void)
{
	audio_survey_print("cs35l43 B rst cfg/level", tlmm_read_cfg(GPIO_CIRRUS_B_RST));
	audio_survey_print("cs35l43 B rst level", tlmm_read_io(GPIO_CIRRUS_B_RST));
	audio_survey_print("cs35l43 T rst cfg", tlmm_read_cfg(GPIO_CIRRUS_T_RST));
	audio_survey_print("cs35l43 T rst level", tlmm_read_io(GPIO_CIRRUS_T_RST));
	audio_survey_print("cs40l26 rst cfg/level", tlmm_read_cfg(GPIO_CS40L26_RST));
	audio_survey_print("wcd9395 rst cfg/level", tlmm_read_cfg(GPIO_WCD9395_RST));
	audio_survey_print("tdm sck cfg (gpio122)", tlmm_read_cfg(GPIO_TDM_SCK));
	audio_survey_print("tdm ws cfg (gpio125)", tlmm_read_cfg(GPIO_TDM_WS));
	audio_survey_print("tdm sd0 cfg (gpio123)", tlmm_read_cfg(GPIO_TDM_SD0));
	audio_survey_print("tdm sd1 cfg (gpio124)", tlmm_read_cfg(GPIO_TDM_SD1));
	audio_survey_print("earpiece sw cfg (gpio174)", tlmm_read_cfg(GPIO_EARPIECE_SW));
}

/* Step 2: the GENI engine status. These are controller-side registers only; no
 * transaction is issued, the I2C slaves are not touched. */
static void
audio_step2_geni(void)
{
	audio_survey_print("geni status (0x40)", *(volatile uint32_t *)(geni + 0x40));
	audio_survey_print("geni irq status (0x50)", *(volatile uint32_t *)(geni + 0x50));
	audio_survey_print("geni m cmd ctrl (0x608)", *(volatile uint32_t *)(geni + 0x608));
}

/* Step 3: which wrap3 branch clocks XBL left voted on. */
static void
audio_step3_gcc(void)
{
	audio_survey_print("gcc wrap3 cbcr (0x52018)", *(volatile uint32_t *)(gcc + GCC_CBCR_WRAP3));
}

/* ---- the write path -----------------------------------------------------------------------------
 * Every helper below is reached only through audio_pinmux / audio_geniprobe /
 * audio_pipe, all default-off, so the survey above stays read-only. */

/* Program one TLMM config window and report the readback. Same shape as
 * popsicle_wifi.c:146-168. The output latch is written BEFORE the pad is
 * switched to GPIO output, so the pad never drives the stale latch value. */
static void
audio_pin_mux(unsigned pin, uint32_t func, uint32_t ma, uint32_t oe,
    uint32_t level, const char *what)
{
	uint32_t cfg_before = TLMM_RD32(audio_tlmm, TLMM_CFG(pin));
	uint32_t io_before = TLMM_RD32(audio_tlmm, TLMM_IO(pin));
	uint32_t cfg_value = (cfg_before & ~TLMM_CFG_MASK) |
	    TLMM_CTL(TLMM_PULL_NONE, func, ma, oe);
	uint32_t cfg_after;
	uint32_t io_after;
	int ok;

	/* Claim an always-on pad for the TLMM before the mux, pandora_adsp.c:1659-1662. */
	if ((cfg_value & TLMM_EOGPIO) != 0) {
		cfg_value |= TLMM_EOGPIO_CLAIM;
	}

	printf("popsicle_audio: %s: before cfg=0x%x io=0x%x, writing func=%u drive=%umA oe=%u\n",
	    what, cfg_before, io_before, func, ma, oe);
	if (oe != 0) {
		TLMM_WR32(audio_tlmm, TLMM_IO(pin), level);
	}
	TLMM_WR32(audio_tlmm, TLMM_CFG(pin), cfg_value);
	cfg_after = TLMM_RD32(audio_tlmm, TLMM_CFG(pin));
	io_after = TLMM_RD32(audio_tlmm, TLMM_IO(pin));
	ok = (cfg_after & TLMM_CFG_MASK) == (cfg_value & TLMM_CFG_MASK);
	if ((cfg_value & TLMM_EOGPIO_CLAIM) != 0) {
		/* The claim bit is outside TLMM_CFG_MASK, so check it separately:
		 * a pad that stayed an eGPIO would otherwise read back "OK" while
		 * the other block still owns it. */
		ok = ok && (cfg_after & TLMM_EOGPIO_CLAIM) != 0;
	}
	if (oe != 0) {
		ok = ok && (io_after & TLMM_IO_HIGH) == (level & TLMM_IO_HIGH);
	}
	printf("popsicle_audio: %s: after cfg=0x%x io=0x%x%s\n", what, cfg_after,
	    io_after, ok ? " OK" : " MISMATCH");
}

/* TDM0 active mux: the three Cirrus amps hang off this bus (48kHz/32bit/4slot,
 * dmesg hw_params channels=3). DT tdm0_*_active wants function i2s0_* at 8 mA
 * with bias disabled and no input-enable, i.e. the SoC drives all four pads. */
static void
audio_tdm_pins_active(void)
{
	printf("popsicle_audio: TDM0 active mux (48kHz/32bit/4slot, func=%u drive=%umA)\n",
	    TLMM_FUNC_I2S0, TDM0_DRIVE_MA);
	audio_pin_mux(GPIO_TDM_SCK, TLMM_FUNC_I2S0, TDM0_DRIVE_MA, 0, 0,
	    "tdm0 sck gpio122 -> i2s0_sck");
	audio_pin_mux(GPIO_TDM_WS, TLMM_FUNC_I2S0, TDM0_DRIVE_MA, 0, 0,
	    "tdm0 ws gpio125 -> i2s0_ws");
	audio_pin_mux(GPIO_TDM_SD0, TLMM_FUNC_I2S0, TDM0_DRIVE_MA, 0, 0,
	    "tdm0 sd0 gpio123 -> i2s0_data0");
	audio_pin_mux(GPIO_TDM_SD1, TLMM_FUNC_I2S0, TDM0_DRIVE_MA, 0, 0,
	    "tdm0 sd1 gpio124 -> i2s0_data1");
}

/* WCD9395 reset pulse on GPIO162 (popsicle_overlay.dts:23821-23843, active
 * high, 16 mA). Assert low, hold, release, hold: the codec answers on
 * i2c@a8c000 @0xe only after this edge, and even then nothing here talks to it. */
static void
audio_wcd9395_reset(void)
{
	printf("popsicle_audio: WCD9395 reset on gpio162 (gpio func, %umA, active high)\n",
	    WCD9395_RST_DRIVE_MA);
	audio_pin_mux(GPIO_WCD9395_RST, TLMM_FUNC_GPIO, WCD9395_RST_DRIVE_MA, 1,
	    TLMM_IO_LOW, "wcd9395 rst assert low");
	delay_for_interval(1000, NSEC_PER_USEC);
	audio_pin_mux(GPIO_WCD9395_RST, TLMM_FUNC_GPIO, WCD9395_RST_DRIVE_MA, 1,
	    TLMM_IO_HIGH, "wcd9395 rst release high");
	delay_for_interval(5000, NSEC_PER_USEC);
}

/* Is i2c@a8c000 already up? Decide from the identity words the bootloader left
 * in the serial engine -- no transfer is issued and no I2C controller driver is
 * implemented here (that belongs to a real GENI/i2c driver). pandora_bt.c:307-330
 * does the same probe for the UART engine: proto = fw & 0xff, then bit 0 of
 * SE_IF_DISABLE says the FIFO interface was handed over. */
static int
audio_geni_i2c_probe(void)
{
	uint32_t fw = TLMM_RD32(audio_geni_i2c, SE_FW_REV);
	uint32_t proto = fw & 0xffu;
	uint32_t if_disable = TLMM_RD32(audio_geni_i2c, SE_IF_DISABLE);

	printf("popsicle_audio: i2c@a8c000 se: fw=0x%x proto=%u (%s) if_disable=0x%x "
	    "status=0x%x hw0=0x%x hw1=0x%x clk_sel=0x%x dma=0x%x m_cmd=0x%x "
	    "m_cmd_ctrl=0x%x m_irq=0x%x\n", fw, proto,
	    proto == GENI_PROTO_I2C ? "I2C" : proto == GENI_PROTO_SPI ? "SPI" : "?",
	    if_disable, TLMM_RD32(audio_geni_i2c, SE_GENI_STATUS),
	    TLMM_RD32(audio_geni_i2c, SE_HW_PARAM0), TLMM_RD32(audio_geni_i2c, SE_HW_PARAM1),
	    TLMM_RD32(audio_geni_i2c, SE_CLK_SEL), TLMM_RD32(audio_geni_i2c, SE_DMA_MODE),
	    TLMM_RD32(audio_geni_i2c, SE_GENI_M_CMD0),
	    TLMM_RD32(audio_geni_i2c, SE_GENI_M_CMD_CTRL),
	    TLMM_RD32(audio_geni_i2c, SE_GENI_M_IRQ_STATUS));
	if (proto == GENI_PROTO_I2C && (if_disable & 1u) == 0) {
		printf("popsicle_audio: i2c@a8c000 VERDICT: bootloader left the SE in I2C "
		    "FIFO mode -- the controller is usable; nothing here drives it and no "
		    "transfer was issued\n");
		return 0;
	}
	printf("popsicle_audio: i2c@a8c000 VERDICT: SE is not in I2C FIFO mode "
	    "(proto=%u if_disable=0x%x) -- WCD9395 is unreachable until a GENI i2c "
	    "driver (or a bootloader fix) exists\n", proto, if_disable);
	return ENOTSUP;
}

/* ---- honest stubs ---------------------------------------------------------------------------------
 * Four things are missing and none of them is faked here: an APM shared-memory
 * region, a TDM0 port started by AudioReach, a DMA channel on that port, and the
 * GIC line of the macro that would raise the completion. pandora_adsp.c:1404-1440
 * has the APM handshake for the speaker graph (pandora_spk=level3 plays a 1 kHz
 * tone) but no port / DMA / IRQ plumbing, and its GPR client is a boot-arg
 * experiment, not a service. So both entry points report the exact missing piece
 * and return ENOTSUP; a caller that ignores the return value will not silently
 * get a half-open path. */

int
audio_dma_setup(void)
{
	printf("popsicle_audio: audio_dma_setup: FAILED ENOTSUP -- ADSP AudioReach GPR path "
	    "is not ready (no APM shared-mem region, no TDM0 port, no LPASS DMA "
	    "channel). See pandora_adsp.c:1404-1440 for what the APM handshake would "
	    "need.\n");
	return ENOTSUP;
}

int
audio_irq_register(void)
{
	printf("popsicle_audio: audio_irq_register: FAILED ENOTSUP -- no interrupt "
	    "registered (TDM0 has no wired IRQ in the DT; GIC SPI 155/171/761/785 "
	    "belong to the rx-macro/va-macro/wsa2-macro SoundWire masters and no driver "
	    "here claims them)\n");
	return ENOTSUP;
}

/* Runs only when at least one of the three new boot-args is set. */
static void
audio_write_path_thread(__unused void *parameter, __unused wait_result_t result)
{
	int rc;

	printf("popsicle_audio: write path (audio_pinmux=%d audio_geniprobe=%d "
	    "audio_pipe=%d)\n", audio_gate_pinmux, audio_gate_geniprobe,
	    audio_gate_pipe);

	audio_tlmm = ml_io_map_unmappable(TLMM_PHYS, TLMM_SIZE, VM_WIMG_IO);
	if (audio_tlmm == 0) {
		printf("popsicle_audio: cannot map TLMM for the write path\n");
		thread_terminate(current_thread());
		return;
	}

	if (audio_gate_pinmux != 0) {
		audio_tdm_pins_active();
		audio_wcd9395_reset();
	}

	if (audio_gate_geniprobe != 0) {
		audio_geni_i2c = ml_io_map_unmappable(GENI_I2C_AUDIO_PHYS,
		    GENI_I2C_AUDIO_SIZE, VM_WIMG_IO);
		if (audio_geni_i2c == 0) {
			printf("popsicle_audio: cannot map the i2c@a8c000 serial engine\n");
		} else {
			rc = audio_geni_i2c_probe();
			printf("popsicle_audio: i2c@a8c000 probe rc=%d\n", rc);
		}
	}

	if (audio_gate_pipe != 0) {
		rc = audio_dma_setup();
		printf("popsicle_audio: audio_dma_setup rc=%d\n", rc);
		rc = audio_irq_register();
		printf("popsicle_audio: audio_irq_register rc=%d\n", rc);
		printf("popsicle_audio: audio chain incomplete -- pins only, no "
		    "clock/port/DMA/IRQ, no codec transaction\n");
	}

	printf("popsicle_audio: write path done (no I2C transaction was issued)\n");
/* PANDORA: ml_io_map_unmappable mappings are permanent; unmap panics (pandora_dpu.c r1b) */
	thread_terminate(current_thread());
}

/* ---- the thread ---------------------------------------------------------------------------------
 * popsicle_wifi.c shape: run once, print, unmap, exit. */
static void
audio_thread(__unused void *parameter, __unused wait_result_t result)
{
	printf("popsicle_audio: WCD9395/Cirrus survey, boot-arg audio_probe=1\n");

	tlmm = ml_io_map_unmappable(TLMM_PHYS, TLMM_SIZE, VM_WIMG_IO);
	geni = ml_io_map_unmappable(GENI_I2C_PHYS, GENI_I2C_SIZE, VM_WIMG_IO);
	gcc = ml_io_map_unmappable(GCC_PHYS, GCC_SIZE, VM_WIMG_IO);
	if (tlmm == 0 || geni == 0 || gcc == 0) {
		printf("popsicle_audio: cannot map TLMM/GENI/GCC\n");
		if (tlmm != 0) {
/* PANDORA: ml_io_map_unmappable mappings are permanent; unmap panics (pandora_dpu.c r1b) */
		}
		if (geni != 0) {
/* PANDORA: ml_io_map_unmappable mappings are permanent; unmap panics (pandora_dpu.c r1b) */
		}
		if (gcc != 0) {
/* PANDORA: ml_io_map_unmappable mappings are permanent; unmap panics (pandora_dpu.c r1b) */
		}
		thread_terminate(current_thread());
		return;
	}

	audio_step1_pins();
	audio_step2_geni();
	audio_step3_gcc();

	printf("popsicle_audio: survey done (pin levels are the boot-loader state; no I2C "
	       "transaction was issued -- no writes)\n");
/* PANDORA: ml_io_map_unmappable mappings are permanent; unmap panics (pandora_dpu.c r1b) */
/* PANDORA: ml_io_map_unmappable mappings are permanent; unmap panics (pandora_dpu.c r1b) */
/* PANDORA: ml_io_map_unmappable mappings are permanent; unmap panics (pandora_dpu.c r1b) */
	thread_terminate(current_thread());
}

/* ---- the entry point ------------------------------------------------------------------------------
 * popsicle_syna.c L1502-1515: PE_parse_boot_argn(...), then the thread.
 * audio_probe keeps its old meaning and its old output; the three new gates are
 * parsed unconditionally but default to zero, so a boot line that names only
 * audio_probe=1 starts exactly one thread and prints exactly the old lines. */
void
popsicle_audio_init(void)
{
	uint32_t enabled = 0;
	uint32_t pinmux = 0;
	uint32_t geniprobe = 0;
	uint32_t pipe = 0;
	thread_t thread;

	if (PE_parse_boot_argn("audio_probe", &enabled, sizeof(enabled)) && enabled &&
	    kernel_thread_start(audio_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}

	audio_gate_pinmux = PE_parse_boot_argn("audio_pinmux", &pinmux,
	    sizeof(pinmux)) && pinmux;
	audio_gate_geniprobe = PE_parse_boot_argn("audio_geniprobe", &geniprobe,
	    sizeof(geniprobe)) && geniprobe;
	audio_gate_pipe = PE_parse_boot_argn("audio_pipe", &pipe, sizeof(pipe)) && pipe;
	if (audio_gate_pinmux == 0 && audio_gate_geniprobe == 0 && audio_gate_pipe == 0) {
		return;
	}
	if (kernel_thread_start(audio_write_path_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
