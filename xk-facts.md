# xk-facts：小米官方 opensource 硬件事实摘录（popsicle / SM8850 / canoe）

> 数据源：MiCode 官方开源仓库（2026-03-09 提交，标题「Kernel: Xiaomi kernel changes for Xiaomi 17, Xiaomi 17 Pro AND Xiaomi 17 Pro Max for Android W」）
> 用途：hackios 移植对照与硬件事实参考。所有行号以本文所述提交为准。

## 0. 仓库结论（先读）

- **MiCode/Xiaomi_Kernel_OpenSource@popsicle-w-oss 不是完整内核树**，是 GKI soc 模块仓库（soc-repo）：
  - `Makefile` VERSION=6 PATCHLEVEL=11（android16-6.12 基线），但 `scripts/basic/fixdep.c` 缺失 → 无法 make defconfig（报错在 `Makefile:635 scripts_basic`）。
  - 无 `drivers/Kconfig`、无 `drivers/input/touchscreen`、全树无 dts（`arch/arm64/boot/dts/vendor` 是指向 `../../../../../qcom/opensource/devicetree` 的断裂符号链接）。
  - git 仅 1 个 squashed commit、无 .gitmodules、无 README。
  - `android/ACK_SHA` = f1bdb13583da85a47fcf1632a78ef52d6e6da651（android16-6.12-2025-06_r8）。
  - `build.config.constants`：CLANG_VERSION=r536225、RUSTC_VERSION=1.81.0.u1、NDK triple aarch64-linux-android31。
  - `build.config.msm.popsicle` 仅一行 `. ${ROOT_DIR}/soc-repo/build.config.msm.canoe`；build 链条 msm.common→msm.perf→aarch64，DEFCONFIG=gki_defconfig + fragment `arch/arm64/configs/vendor/${MSM_ARCH}_perf.config`（vendor/ 目录在本仓库缺失）→ bazel/kleaf 目标 popsicle.bzl 需要 android common 源码环境。
- **dts 事实必须从 MiCode/kernel_devicetree@popices-w-oss 取**（本仓 qcom/popsicle-sm8850-touch.dtsi 等）。
- **面板 dtsi 必须从 MiCode/vendor_qcom_opensource_display-devicetree@popsicle-w-oss 取**（kernel_devicetree 里没有 dsi-panel）。

## 1. 触控（xk-dt <BUILD>/xk-dt/qcom/popsicle-sm8850-touch.dtsi，497 行）

### 供电（固定 regulator）
- `touch_avdd_vreg`（第一屏 synaptics AVDD）：GPIO **tlmm 117**，enable-active-high，行 9-14。
- `touch_avdd_vreg_second`（第二屏 goodix AVDD）：GPIO **tlmm 82**，enable-active-high，行 16-21。
- `&L4I`（syna iovdd）固定 **1.8V**，行 24-28。
- `&L2F`（goodix iovdd）固定 **1.2V**，行 30-34。

### 第一屏 synaptics TCM（&qupv3_se19_spi，行 298-440）
- SPI pinctrl（行 93-143）：GPIO48=MISO/qup4_se0_l0、GPIO49=MOSI/qup4_se0_l1、GPIO50=CLK/qup4_se0_l2、GPIO51=CS/qup4_se0_l3；sleep 态（行 146-172）CS 保持、48/49/50 回 GPIO、drive 2mA。
- synaptics,tcm-spi@0（行 305-439）：spi-max 15MHz；IRQ GPIO158（0x2008）；RST GPIO66；avdd=<&touch_avdd_vreg>；iovdd=<&L4I>；fod-lx=597 fod-ly=2406、fod 246×246（行 328-331）；max-x=1200 max-y=2608（行 333-334）；rx=18 tx=39（行 335-338）；panel-display-resolution 1440×3200（行 410）；thp 配置 popsicle_syna_thp_config.ini、固件 synaptics_spi_popsicle_00.img（cfg_0 ddic-id=1）/ synaptics_spi_popsicle_01.img（cfg_1 ddic-id=2）、test-limit popsicle_test_limits_S3910P.csv（行 421-437）；qts trusted-touch vm_mode/tvm、spi-irq=647、io-base 0x1a80000 size 0x1000、vm-gpio-list 48/49/50/51/66/158（行 411-420）。
- 两个 SPI 节点都标 `qcom,la-vm; qcom,rt;`（行 303-304、447-448）。

### 第二屏 goodix GT9615v（&qupv3_se15_spi，行 442-497）
- compatible "goodix,gt9615"；SPI GPIO12/13/14/15 = qup3_se2_l0..l3（行 223-269）；IRQ GPIO201；RST GPIO185；iovdd=<&L2F>；avdd=<&touch_avdd_vreg_second>；panel-max-x=97600 panel-max-y=59600（行 462-463，×100 为物理坐标 976×596）；firmware goodix_firmware_popsicle_csot；config goodix_cfg_group_popsicle_csot（行 467-468）。

## 2. 面板（xk-disp <BUILD>/xk-disp/display/）

- popsicle-sde-display.dtsi（270 行）定义 9 个第一屏候选 + 1 个第二屏：
  - **第一屏默认** = `dsi_p2_42_02_0a_prim_dsc_cmd`（&sde_dsi，行 186）；候选 p2_42_0d_0b（prim/p12/p2/mp 四个变体）、p2_42_02_0a（p11/p12/p2 变体）、nt37801 fhd+ cmd。
  - **第二屏**（&sde_dsi1，行 194-204）= `dsi_p2_42_06_0a_sec_dsc_cmd` + nt37801 video（chargetime 副屏）。
  - 主屏供电（行 185-192）：vddd=<&display_panel_vddd_p2>（GPIO 90，1.06V，行 13-21）、vddio=<&L12B>（1.8V）、vci=<&display_panel_vci_p2>（GPIO 108，3.0V，行 22-30）。
  - 副屏供电（行 194-204）：sec-vddio=<&L10B>、sec-vddd=<&display_panel_vddd_p2_sec>（GPIO 214，1.2V，行 97-105）、sec-vci=<&display_panel_vci_p2_sec>（GPIO 215，3.0V，行 106-114）。
  - 触控↔面板绑定（行 215-234）：syna 触摸的 panel 列表 = 全部 9 个主屏候选；goodix 的 panel = 副屏 06_0a + nt37801 video。
  - nt37801 video 节点重写（行 148-158）：bl_ctrl_dcs、bl-min 10 / max 4095、brightness-max 8191、inverted-dbv、sec-reset GPIO30。
- **主屏 p2_42_02_0a**（dsi-panel-p2-42-02-0a-prim-dsc-cmd-common.dtsi 行 168-201）：width=1200 height=2608；H 前/后 64/64、H 脉宽 32；V 前/后 64/64、V 脉宽 32；**panel-clockrate = 1198000000**（行 181，与实机已知值一致）；phy-timings `[00 27 0A 0A 1B 19 0A 0B 0A 02 04 00 20 0F]`（行 186）；topology `<2 2 1>`（行 187）；**DSC：version 0x12、slice-h 16、slice-w 600、slice-per-pkt 2、bpc 10、bpp 8、block-prediction-enable**（行 190-198）；4 lane（lane-0..3-state，主文件行 13-16）。
- **主屏 p2_42_0d_0b p12**（dsi-panel-p2-42-0d-0b-p12-prim-dsc-cmd-common.dtsi 行 61-95）：同 1200×2608、同 porches、**同 1198000000**（行 75）、同 phy-timings/topology/DSC（slice 600×16×2、10bpc 8bpp）；4 lane。
- **副屏 p2_42_06_0a**（dsi-panel-p2-42-06-0a-sec-dsc-cmd-common.dtsi）：**width=596 height=976**（横排副屏）；H 前/后 64/32、脉宽 32；V 前/后 32/32、脉宽 32；**clockrate=553000000**；topology `<1 1 1>`；DSC slice 596×16×1、10bpc 8bpp；**2 lane**（lane-0/1-state）。
- canoe-sde-display-mtp.dtsi:298 默认面板 `dsi_p2_42_02_0a_dsc_cmd`（canoe 通用板）。

## 3. 供电 regulator 定义（xk-dt <BUILD>/xk-dt/qcom/canoe-regulators.dtsi）

- `L2F`（行 924-936）：`pmh0110_f_l2`，rpmh-vrm-regulator、resource `L2F_E0`、pmic7-ldo、固定 **1.2V**（行 931-933）。→ goodix iovdd。
- `L4I`（行 1125-1137）：`pmh0110_i_l4`，resource `L4I_E0`、pmic7-ldo、固定 **1.8V**。→ syna iovdd。**注意：主控之前记忆的「pmh0110-i-l4 = syna iovdd」成立；「pmh0110-f-l2 = goodix iovdd」也成立。**
- `L9B`（行 499-506）：pmh0101_l9，2.96-3.3V，init 2.96V HPM → fp_3v3（popsicle-sm8850.dtsi 行 33-37 将其固定到 3.3V）。
- `L10B`（行 518-526）：pmh0101_l10，固定 1.8V，proxy-consumer-enable → 副屏 sec-vddio。
- `L12B`（行 558-566）：pmh0101_l12，1.2-1.8V，init 1.8V，proxy-consumer-enable → 主屏 vddio。

## 4. pKVM / gunyah / mitee

- 实机「protected KVM」对应 gunyah 生态，不是标准 pKVM：canoe_perf.bzl（<BUILD>/xk-popsicle/configs/canoe_perf.bzl）行 52-70：`CONFIG_GH_ARM64_DRV/CTRL/DBL/VCPU_MGR/IRQ_LEND/MEM_NOTIFIER/MSGQ/PANIC_NOTIFIER/RM_BOOSTER/RM_DRV/RM_HEAP_MANAGER/TLMM_VM_MEM_ACCESS =m`、`CONFIG_QTVM_WITH_AVF=y`、`CONFIG_GUNYAH_DRIVERS=y`、`CONFIG_GUNYAH_TRACE=m`、`CONFIG_HVC_GUNYAH=m`；行 278 `CONFIG_QRTR_GUNYAH=m`。generic_vm_defconfig 行 134-136：`CONFIG_GUNYAH=y`、`CONFIG_GUNYAH_GUEST=y`、`# CONFIG_GUNYAH_QCOM_PLATFORM is not set`。全树无 `kvm-arm.mode=protected` 字样。
- mitee（popsicle-sm8850.dtsi 行 42-68）：oem_vm_shm@fc200000 4MB no-map、gunyah-label 0x3；mitee 节点 binding-core 4 5 6、tui-supported；mitee-hlos-ipc gunyah-shm-label 0x3 peer-name 0x4。
- popsicle_perf.bzl 实际模块（12 项）：CONFIG_FINGERPRINT_MI_FP/CONFIG_INPUT_CS40L26(_I2C)/CONFIG_SND_SOC_CS40L26/CONFIG_CL_DSP/CONFIG_DEVICE_MODULES_DIO_DMABUF_MAPPER/CONFIG_IR_SPI/CONFIG_MI_KERNEL_MONITOR/CONFIG_POWERINSIGHT(+GPU_OPP=m/GPU_OPP_STATS=y/GPU_FREQ_STATS=m)。

## 5. 指纹 / 其他（popsicle-sm8850.dtsi）

- xiaomi,xiaomi-fp：irq/ipc tlmm 99、vendor "P2_n2_132_431"、fp_3v3=<&L9B> 3.3V、fp_1v8=fingerprint_1v8_vdd（固定 1.8V，GPIO 56，enable-active-high）、sensor-loc 480 1906 719 2165（行 4-36）。
- IR spi：qupv3_se0_spi ir-spi@0 19.2MHz（行 70-79）。

## 6. 编译现状（✅ 2026-10-24 完成）

- **GKI Image.gz 编译成功**：`<BUILD>/out-common/arch/arm64/boot/Image.gz` 13,692,603 B，sha256 `47093bd463b53c96ca682afe9288fa42a1f79c62c4cdd9bbc1123af7e934f11e`；Image 32,537,088 B，magic `ARMd`✓，内核版本 6.12.23-4k-gf1bdb13583da。
- 配方：TUNA 镜像浅克隆 ACK android16-6.12-2025-06_r8（xk-common，HEAD f1bdb135…=ACK_SHA）→ `make O=out-common ARCH=arm64 LLVM=1 gki_defconfig` → 关 BTF（pahole 1.25 太旧）→ `make Image modules -j10`（clang 18.1.3 + ld.lld + aarch64-linux-gnu-gcc 13.3.0，LLVM=1）。
- 依赖补齐记录：libdw-dev（gendwarfksyms dwarf.h）、cpio（kheaders tar）、bc/bison/flex/libelf-dev/pahole/dtc。
- vendor 模块 5 个 .ko 全部编译成功：dio_dma_mapper.ko（35,080 B）、powerinsight common.ko（151,088 B）+ gpu_stats.ko（20,440 B）+ gpu_freq_stats.ko（23,464 B）、mi_kernel_monitor.ko（700,432 B，需 <BUILD>/common→xk-common 符号链接满足 kleaf 相对 include）。
- dtb/dtbo 编译成功：canoe.dtb 444,248 B（sha256 fe772458…）+ popsicle-sm8850-overlay.dtbo 137,067 B（sha256 e29d45dc…），cpp 预处理+dtc -@ 两段式。
- **边界（如实）**：这是 GKI 通用 Image，非 popsicle vendor 引导映像——缺 CONFIG_ARCH_CANOE（Kconfig.ext 手工 flatten 撞 61 个与 common 重复的 source，kleaf 才能正确合成）、缺 vendor fragment、dtbo 未封装 dtbo.img、未打 boot 头。作为**硬件事实参考源 100% 可用**；作为**可引导内核**还需 kleaf 环境或后续 Kconfig.ext 去重。
- 工具链：clang 18.1.3 / gcc 13.3.0 / aarch64-linux-gnu-gcc 13.3.0 / ld.lld 18.1.3 齐备；pahole v1.25 + libelf-dev + libdw-dev + cpio 已装。
- 仓库布局（<BUILD>/）：xk-popsicle（soc-repo 42M）、xk-common（ACK 1.9G）、xk-dt（devicetree 37M）、xk-disp（display devicetree 7.5M）、xk-work（kleaf 布局尝试，Kconfig.ext 撞车记录见 build/kconf*.log）、out-common（成功构建输出）。
