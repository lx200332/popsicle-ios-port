# popsicle (Xiaomi 17 Pro Max / SM8850) 固件分析报告

分析日期：2026-10-23
工作区：`C:\dsh-对话\popsicle-port\`
分析对象：`D:\Backup\Downloads\4.0.4.0\`（payload 解包产物）

---

## 0. 结论摘要

| 项 | 结论 |
|---|---|
| 设备平台 | Xiaomi 17 Pro Max，codename **popsicle**，SoC **SM8850** |
| SoC 平台代号 | **canoe**（DT）/ **kaanapali**（XBL 平台名）— 二者同一芯片 |
| popsicle 设备树 | `dtbo.img` 第 2 条目，`model = "Qualcomm Technologies, Inc. Popsicle based on SM8850"` |
| Mu-Silicium 平台层 | **已支持**（`KaanapaliPkg`，OnePlus 15 在用） |
| 缺失层 | `popsiclePkg`（设备包，12 个文件） |
| UEFI 内存映射 | **已完整取得**（28 个区域，来自 `xbl_config.img` 的 `/soc/memorymap`） |
| 与 OnePlus 15 重合度 | `Kernel` / `DXE_Heap` / `Display_Demura` 三项**逐字节相同** |
| 串口调试 | **0xA9C000**，`qcom,geni-debug-uart`，115200n8，IRQ 835 |
| 最大未知 | DRAM 容量（固件里不存在，运行时 DDR training 决定） |

**核心判断**：popsicle 的 Mu-Silicium 移植，可以以 OnePlus 15（infiniti）的设备包为模板直接改，平台层和内存映射都不用重做。

---

## 1. 输入材料

| 路径 | 内容 |
|---|---|
| `popsicle-ota_full-OS4.0.4.0.XPBCNXM-user-17.0-7d13a10bbc` | 卡刷包，含 `payload.bin` 8.75 GB |
| `666\popsicle_FullBackup_20261023004711` | 手机端提取的完整备份（A/B 分区） |
| `4.0.4.0\` | payload 解包后的全部分区镜像（43 个 .img） |

本次分析使用 `4.0.4.0\` 下的：
- `vendor_boot.img`（96 MB）→ 内含 base SoC DTB
- `dtbo.img`（22 MB）→ 内含 popsicle 设备 overlay
- `boot.img`（96 MB）→ GKI kernel 42 MB
- `xbl_config.img`（388 KB）→ 内含 **UEFI 内存映射配置**
- `uefi.img`（2.73 MB）→ 原厂 UEFI 固件（ELF64，entry 0xC6800000）

---

## 2. DTB 分析

### 2.1 vendor_boot 的 base DTB

`vendor_boot.img` 的 `dtb` 段 = 6,557,351 字节，内部是 **12 个拼接的 FDT**（每个 500–580 KB）：

| # | model | msm-id |
|---|---|---|
| v00 | Canoe Alt. Thermal Profile v2 SoC | `0x1000294 0x20000` |
| v01 | Canoe Alt. Thermal Profile SoC | `0x1000294 0x10000` |
| **v02** | **Canoe v2 SoC** | `0x294 0x20000` |
| **v03** | **Canoe SoC** | `0x294 0x10000` |
| v04 | CanoeP SG v2 SoC | `0x2e7 0x20000` |
| v05 | CanoeP SG SoC | `0x2e7 0x10000` |
| v06 | CanoeP Alt. Thermal Profile v2 SoC | `0x1010295 0x20000` |
| v07 | CanoeP Alt. Thermal Profile SoC | `0x1010295 0x10000` |
| v08 | CanoeP v2 SoC | `0x295 0x20000` |
| v09 | CanoeP SoC | `0x295 0x10000` |
| v10 | Whale SoC | `0x2d2 0x30000` |
| v11 | WhaleP SoC | `0x2d3 0x30000` |

**canoe = 0x294，canoeP = 0x295**。同一份 vendor_boot 同时服务 Canoe / CanoeP / Whale / WhaleP 四个 SoC 家族。

popsicle 是 SM8850（非 P），因此候选为 **v02 / v03**（以及 Alt. Thermal Profile 的 v00 / v01）。具体选哪个由 XBL 运行时上报的 board rev 决定。

已导出：`dts/canoe_base.dts`（v03，720 KB）

### 2.2 popsicle 设备 overlay

`dtbo.img` 结构：`dt_table`，2 个条目，均 `id=0 rev=0`。

| 条目 | 大小 | msm-id 覆盖 | model |
|---|---|---|---|
| `dtbo_00` | 145,475 B | `0x2d3` `0x2d2` `0x2e7` | Popsicle based on SM8850 |
| **`dtbo_01`** | **1,450,875 B** | `0x295` `0x294` `0x1000294` `0x1000295` | **Popsicle based on SM8850** |

**`dtbo_01_id0_rev0.dtb` 就是 popsicle 在 SM8850 上的设备树 overlay**，共 **151 个 fragment**，全部 target 通过 `__fixups__` 绑定到符号。

关键 fragment：

| fragment | target 符号 | 内容 |
|---|---|---|
| @0 / @1 | `spmi0_bus` / `spmi1_bus` | PMIC：pmk8850, pmh0101, pmh0110, pmd802x, pmih010x, pmh0104, pmr735d, pm8010 |
| @5 | `soc` | `gpio_keys` |
| @10 / @11 | `ufsphy_mem` / `ufshc_mem` | UFS 使能（status=ok） |
| @13 / @14 | `usb0` / `ucsi` | USB role-switch + Type-C connector |
| @18 | `soc` | `xiaomi_fingerprint`, `mi_t1_chip` |
| @41 / @45 / @46 | `soc` / `qupv3_se19_spi` / `qupv3_se15_spi` | 触摸：`synaptics_tcm@0`、`goodix_gt9615v@0` |
| @40 | `qupv3_se21_i2c` | NFC `nq@28` |
| @51 / @53 | `soc` / `pcie_rp` | WLAN `qcom,cnss-peach@b0000000` |
| @57–@73 | `mdss_mdp` | 屏幕：`nt37801` wqhd_plus（cmd / vid / SL / ddicspr / cphy_spr） |
| @137 | `reserved_memory` | `splash_region` 0xFC800000+43 MiB，`demura_region_0/1` |
| @141–@157 | `mdss_mdp` | 实际面板：`p2_42_02_0a_prim_dsc_cmd` 等 7 个变体 |
| @159 | `soc` | `dsi_panel_pwr_supply_p2` 等供电 |
| @199–@205 | `soc` | 摄像头：eeprom0-3, actuator0-3, cam-sensor0-3, ois3 |
| @206–@226 | — | 音频：lpass-cdc, wsa/wsa2/rx/tx-macro, SWR pinctrl |

已导出：`dts/popsicle_overlay.dts`（1.4 MB）

### 2.3 移植相关的硬件地址

从 base DTB（v03）提取：

| 功能 | 节点 | compatible | 基址 |
|---|---|---|---|
| **串口调试** | `qcom,qup_uart@a9c000` | `qcom,geni-debug-uart` | **0xA9C000**（16 KB，IRQ 835） |
| GENI SE 父节点 | `qcom,qupv3_1_geni_se@ac0000` | `qcom,geni-se-qup` | 0xAC0000 |
| **USB3 控制器** | `ssusb@a600000` | `qcom,dwc-usb3-msm` | **0xA600000**（+0x1FC6000 TCSR） |
| DWC3 核 | `dwc3@a600000` | `snps,dwc3` | 0xA600000（0xD93C） |
| USB2 PHY | `hsphy@88e3000` | `qcom,usb-m31-eusb2-phy` | 0x88E3000 |
| USB3/DP PHY | `ssphy@88e8000` | `qcom,usb-ssphy-qmp-dp-combo` | 0x88E8000 |
| UFS | `ufshc@1d84000` | `qcom,ufshc` | 0x1D84000（base 中 disabled，overlay 打开） |
| RNG | `rng@10c3000` | `qcom,trng` | 0x10C3000 |
| AOSS QMP | `power-controller@c300000` | `qcom,aoss-qmp` | 0xC300000 |

`chosen/bootargs` 关键项：
```
arm64.nosme cpufreq.default_governor=performance kpti=0 swiotlb=0
irqaffinity=0-1 fw_devlink.strict=1 pcie_ports=compat kasan=off
rcu_nocbs=0-7 initcall_blacklist=ufshcd_core_init
```
`chosen/stdout-path = "/soc/qcom,qupv3_1_geni_se@ac0000/qcom,qup_uart@a9c000:115200n8"`

`__symbols__` 有 **1936 个标签**，`soc` 下有 **551 个节点** —— 移植时可以直接用标签引用。

---

## 3. 权威 UEFI 内存映射

### 3.1 来源

原厂 UEFI 固件（`uefi.img`）的字符串里有：

```
/soc/memorymap/
GetMemoryMapOfNode Failed
Failed ParseMemoryMapEntriesFromDT
UEFI Memory Map configuration not found
ERROR: UEFI FD base(0x%x) not match with mem addr(0x%x)!
```

→ **UEFI 的内存映射不是硬编码，而是从设备树的 `/soc/memorymap/` 节点读取**。

该节点不在 base DTB、也不在 dtbo overlay 里，而是由 XBL 注入。它位于 `xbl_config.img` 的 CFGL 配置块中，同时也存在于 `post-ddr.dtbs.bin`（文件偏移 0x1116C，大小 201,944）里。

### 3.2 完整映射表（28 个区域）

| base | size | MiB | 区域名 |
|---|---|---|---|
| `0x0080000000` | `0x0001960000` | 25.38 | NOMAP |
| `0x0081960000` | `0x00000A0000` | 0.63 | RSRV0 |
| `0x0081A00000` | `0x0000060000` | 0.38 | XBL_DT |
| `0x0081C00000` | `0x00000A0000` | 0.63 | AOP |
| `0x0081CE4000` | `0x0000010000` | 0.06 | UEFI_Log |
| `0x0081D00000` | `0x0000200000` | 2.00 | SMEM |
| `0x0081F00000` | `0x0000100000` | 1.00 | PDP_NS |
| `0x00824A0000` | `0x0000100000` | 1.00 | PvmFw |
| `0x008AA00000` | `0x001D300000` | 467.00 | PIL_Reserved |
| `0x00A7D00000` | `0x0002C80000` | 44.50 | **Display_Demura** |
| `0x00AA980000` | `0x0005770000` | 87.44 | DBI_Dump |
| `0x00B00F0000` | `0x00131A0000` | 305.63 | **DXE_Heap** |
| `0x00C4800000` | `0x0002000000` | 32.00 | HLOS1 |
| `0x00C6800000` | `0x0000400000` | 4.00 | **UEFI_FD** |
| `0x00C6C00000` | `0x0000200000` | 2.00 | UEFI_FD_Reserved |
| `0x00C6E00000` | `0x0000001000` | 0.004 | CPU_Vectors |
| `0x00C6E01000` | `0x0000001000` | 0.004 | Info_Blk |
| `0x00C6E02000` | `0x0000003000` | 0.01 | MMU_PageTables |
| `0x00C6E05000` | `0x0000008000` | 0.03 | Log_Buffer |
| `0x00C6E0D000` | `0x0000040000` | 0.25 | UEFI_Stack |
| `0x00C6E4D000` | `0x000008C000` | 0.55 | SEC_Heap |
| `0x00C6ED9000` | `0x0000400000` | 4.00 | Sched_Heap |
| `0x00C72D9000` | `0x0000400000` | 4.00 | FV_Region |
| `0x00C76D9000` | `0x0000127000` | 1.15 | UEFI_RESV |
| `0x00C7800000` | `0x0010000000` | 256.00 | **Kernel** |
| `0x00D7800000` | `0x00001C0000` | 1.75 | XBL_Ramdump |
| `0x00D7A00000` | `0x0000200000` | 2.00 | Global_SYNC |
| `0x00FC800000` | `0x0002B00000` | 43.00 | Display_Reserved |

原始数据：`extract/uefi_memmap.json`

### 3.3 布局连续性验证

`0xC4800000` 起，各区域首尾相接，无空洞：

```
0xC4800000  HLOS1              32.00 MiB  → 0xC6800000
0xC6800000  UEFI_FD             4.00 MiB  → 0xC6C00000
0xC6C00000  UEFI_FD_Reserved    2.00 MiB  → 0xC6E00000
0xC6E00000  CPU_Vectors         4 KiB     → 0xC6E01000
0xC6E01000  Info_Blk            4 KiB     → 0xC6E02000
0xC6E02000  MMU_PageTables     12 KiB     → 0xC6E05000
0xC6E05000  Log_Buffer         32 KiB     → 0xC6E0D000
0xC6E0D000  UEFI_Stack        256 KiB     → 0xC6E4D000
0xC6E4D000  SEC_Heap          560 KiB     → 0xC6ED9000
0xC6ED9000  Sched_Heap          4.00 MiB  → 0xC72D9000
0xC72D9000  FV_Region           4.00 MiB  → 0xC76D9000
0xC76D9000  UEFI_RESV           1.15 MiB  → 0xC7800000
0xC7800000  Kernel            256.00 MiB  → 0xD7800000
```

`Kernel` 恰好终止于 `XBL_Ramdump` 起点 `0xD7800000`。UEFI 整体占用 `0xC4800000..0xD7800000` = **304 MiB**。

### 3.4 与 OnePlus 15（infiniti）逐项对比

| 区域 | popsicle（本次实测） | OnePlus 15 | 差异 |
|---|---|---|---|
| `Kernel` | `0xC7800000` + `0x10000000` | `0xC7800000` + `0x10000000` | **完全相同** |
| `DXE_Heap` | `0xB00F0000` + `0x131A0000` | `0xB00F0000` + `0x131A0000` | **完全相同** |
| `Display_Demura` | `0xA7D00000` + `0x02C80000` | `0xA7D00000` + `0x02C80000` | **完全相同** |
| `UEFI_FD` | `0xC6800000` + `0x400000` | `0xC6900000` + `0x300000` | 起点差 1 MiB，大小差 1 MiB |

结论：**popsicle 的 `MemoryMapLib.c` 只需把 `UEFI_FD` 改成 `0xC6800000 / 0x400000`，其余四项照抄 OnePlus 15。**

### 3.5 原厂 UEFI 固件本身

`uefi.img` = ELF64（`e_machine = 0x28`），**入口地址 `0xC6800000`** —— 与 `UEFI_FD` 区域起点完全一致。

字符串证据（节选）：
```
UEFI DXE
UefiPlatCfg.c
UEFI_FD / UEFI_FD_Reserved / DXE_Heap / UEFI_Stack / UEFI_LOG
ERROR: UEFI FD region not present in cfg file!
Failed to match Uefi FD addr/size !
Could not locate "DXE Heap" in CFG
```

→ Mu-Silicium 的 `MemoryMapLib` 区域命名就是从高通这套抄的，语义可直接对应。

`xbl_config.img` 内的 FDT（共 10 个）：

| 偏移 | 大小 | 内容 |
|---|---|---|
| `0x31C` | 67,840 | `pre-ddr.dtbs.bin` — model `qcom,kaanapali` |
| `0x1116C` | 201,944 | `post-ddr.dtbs.bin` — model `qcom,kaanapali`，含 `/soc/memorymap` |
| `0x10C1C` / `0x10DA4` | 392 / 968 | 小配置块 |
| `0x42644` … `0x43A14` | 408–1904 | 6 个小配置块 |

**关键证据**：XBL 的平台名是 **`qcom,kaanapali`**，而 Linux DT 的 compatible 是 **`qcom,canoe`**。两者指同一颗 SM8850。这从厂商固件层面证实了 Mu-Silicium 的 `KaanapaliPkg` 就是这套平台。

---

## 4. reserved-memory（Linux 侧，用于交叉校验）

base DTB 的 `reserved-memory` 有 58 个子节点，与 UEFI 内存映射互相印证：

- 起始锚点：`gunyah_hyp_region@80000000`（DRAM base = **0x80000000**）
- 4 GiB 下方最高：`llcc_lpi_region@ff800000` → `0x100000000`
- 4 GiB 以上：`lcp_dare_region` @ `0x880000000`（34 GiB）+ 96 MiB
  → 说明这份 base DTB 是 **canoe 通用最大配置**，实际 DRAM 容量由 XBL 运行时回填，静态固件里不存在

overlay 追加的保留区：

| 节点 | 范围 | 说明 |
|---|---|---|
| `shm@fc200000` | `0xFC200000` + 4 MiB | |
| `splash_region` | `0xFC800000` + 43 MiB | 与 UEFI 的 `Display_Reserved` 一致 |
| `llm_mem` / `cnss_wlan_region` | 无 reg | 运行时动态分配 |
| `demura_region_0/1` | reg=0 | 占位 |

`/soc/ddr` 节点（post-ddr DT）只有训练参数，无容量：
```
compatible = "qcom,ddr"
ddr_boot_freq = <0x30a200>
print_severity = <0x2012>
```

---

## 5. 对 Mu-Silicium 移植的直接结论

### 5.1 需要新建的包

`Platforms/Xiaomi/popsiclePkg/`，以 `Platforms/OnePlus/infinitiPkg/` 为模板。

12 个文件：

```
popsiclePkg.dec
popsiclePkg.dsc
popsiclePkg.fdf
DeviceBuild.py
Include/ACPI.inc
Include/Library/DeviceMemoryMapLib.h   (如需)
Library/MemoryMapLib/MemoryMapLib.c
Library/PlatformPeiLib/PlatformPeiLib.c
Library/PlatformBootManagerLib/...
Library/DeviceMemoryMapLib/...
```

### 5.2 `MemoryMapLib.c` 直接可用的数值

```c
// popsicle (Xiaomi 17 Pro Max, SM8850/canoe) — 取自 xbl_config.img 的 /soc/memorymap
UEFI_FD          0x00C6800000  0x00400000
UEFI_FD_Reserved 0x00C6C00000  0x00200000
DXE_Heap         0x00B00F0000  0x131A0000
UEFI_Stack       0x00C6E0D000  0x00040000
Display_Demura   0x00A7D00000  0x02C80000
Kernel           0x00C7800000  0x10000000
```

### 5.3 `DeviceBuild.py` 需改的字段

- `PackagesSupported` → `["popsicle"]`
- `GetName()` → `"Xiaomi 17 Pro Max"`
- `PRODUCT_NAME` → `"popsicle"`
- `ACTIVE_PLATFORM` → `KaanapaliPkg/KaanapaliPkg.dsc`
- `Scopes` → 加入 `Platforms/Xiaomi`

### 5.4 编译环境

Windows 本机可完成（Mu-Silicium 官方支持）：

```
Git
Python 3.11+
LLVM（含 clang / lld-link / llvm-objcopy）
Visual Studio Build Tools（MSVC 库）
GnuMake32（Silicon 目录用 GNU make）
```

命令：`python build_uefi.py -d popsicle`

---

## 6. 剩余未知与下一步

| # | 未知项 | 影响 | 解决路径 |
|---|---|---|---|
| 1 | **DRAM 容量** | 决定 Kernel 之后给 Linux 的内存范围 | 设备上 `adb shell cat /proc/meminfo`；或 `dmesg \| grep -i memory` |
| 2 | popsicle 用哪个 base DTB 变体（v00/v01/v02/v03） | 影响 panel/PMIC 初始化细节 | 读 `/proc/device-tree/model` 或 XBL 日志 |
| 3 | 双屏副屏（976×596） | Mu-Silicium 无副屏支持 | 先按单屏移植，副屏后续 |
| 4 | UEFI_FD 大小上限 4 MiB | Mu-Silicium FD 通常 1–2 MiB，够用 | 编译后核对 `FD` 体积 |
| 5 | 屏幕点亮（SDE + nt37801 + DSI） | 无显示则只能靠串口 | 先串口，后 display |

### 建议的推进顺序

1. **先串口**：确认 0xA9C000 / 115200 能出字。这一步成功，后面才有调试手段。
2. **拷 infinitiPkg → popsiclePkg**，改 5.2 / 5.3 的字段，编译出 FD。
3. **核对 FD 体积 ≤ 4 MiB**，超出则裁组件。
4. **刷 boot 分区**（Mu-Silicium 走 Android boot image v4，不替换 XBL，可逆）。
5. 串口起来后再攻 UFS → USB → 显示。
6. 内核用 `FantomTchi7/kaanapali-mainline-linux`（OnePlus 15 在用的那套）。

---

## 附：生成的产物

| 文件 | 说明 |
|---|---|
| `dts/canoe_base.dts` | base SoC DTS（v03，Canoe，720 KB） |
| `dts/popsicle_overlay.dts` | popsicle 设备 overlay（151 fragment，1.4 MB） |
| `dts/whale_overlay.dts` | Whale/CanoeP 用的另一个 overlay（179 KB） |
| `extract/uefi_memmap.json` | 28 个 UEFI 内存区域（结构化） |
| `extract/uefi.strings.txt` | 原厂 UEFI 字符串（3,163 条） |
| `extract/xblcfg.strings.txt` | xbl_config 字符串（4,902 条） |
| `extract/dtb_variants/` | 12 个 base DTB |
| `extract/dtbo/` | 2 个 overlay DTB |
| `extract/vendor_boot/` | vendor_ramdisk / dtb / bootconfig / table |
| `extract/boot/kernel.bin` | GKI kernel 42 MB |
| `tools/*.mjs` | 自写的纯 Node 解析工具（FDT / boot / dtbo / CFGL） |

工具说明：全部是纯 Node ESM，不依赖 `dtc`、不依赖 Python，可直接用 DSH 内置 node 运行。
