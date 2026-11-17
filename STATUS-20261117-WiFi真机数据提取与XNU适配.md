# popsicle WiFi 真机数据提取报告

设备：Xiaomi 17 Pro Max（`popsicle` / 2509FPN0BC / SM8850 `canoe`）
提取时间：2026-11-12　出口：`wifi-extract/`
目标：为 `lx200332/popsicle-ios-port` 的 XNU 侧 WiFi 驱动提供逐位可还原的硬件事实

---

## 一、提取物清单

### 已入库（文本类，在 `wifi-extract/`，完整索引见该目录 README）

| 路径 | 内容 | 用途 |
|---|---|---|
| `pci/ep-cfg-parsed.txt` | 端点 4 KiB config space 结构化解析 | BAR / 能力链 / MSI-X 表位置 / AER |
| `pci/pci-all-4k.txt` | 两设备 4 KiB dump（十六进制全量） | 逐字节复核 |
| `pci/pci-config-full.txt` | `lspci -xxx` | 交叉校验 |
| `pci/pci-resource.txt` / `pci-modalias.txt` / `pci-uevent.txt` | BAR、modalias、绑定路径 | 驱动匹配字符串 |
| `state/regulator_summary` | 28 905 B | 8 条 WiFi 电源轨实测电压/模式 |
| `state/clk_summary` | 192 107 B | rf_clk 76.8 MHz、pipe 19.2 MHz |
| `state/interrupts.txt` | 55 909 B | MSI 向量 → CE / DP / MHI 绑定 |
| `state/tlmm-pinmux-pins.txt` / `pinconf-pins.txt` | GPIO 16/18/19/204/205 | 逐 pin 的 func / drive / bias |
| `state/msi-irqs.txt` | 32 个向量号快照 | MSI-X 表规模（**号会变，只记向量序号**） |
| `state/iomem.txt` | BAR 与 CMA 的物理落点 | iATU 映射依据 |
| `state/mhi-detail.txt` / `mhi-list.txt` / `mhi-states.txt` | OEMPKHASH / serial / 设备枚举 | BHI 握手参数 |
| `state/socinfo.txt` / `board-id.txt` / `cmdline.txt` | machine=CANOE、board-id=8 | BDF 变体选择依据 |
| `logs/dmesg-wifi.txt` | 完整 dmesg 中 wifi/pcie/mhi/cnss/qmi 相关的 435 行 | 上电时序来源 |
| `popsicle_wifi_names.h` | 166 个 `#define` | 直接可用的 C 常量表 |
| `tools/` | `parse_cfg.py` / `elfinfo.py` / `gen_header.py` / `decode_cnss.py` | 全部可重跑 |

### 不入库（体积大 / 可复现，取法见 `wifi-extract/README.md`）

| 内容 | 体积 | 取法 |
|---|---|---|
| `firmware/peach/` **100 个文件** | 21 MB | `/vendor/firmware_mnt/image/peach/` 整目录 pull；含 `amss20.bin` 7 319 616 B、`regdb_xiaomi.bin` 24 278 B、**19 个 BDF 变体**（`cnss2.ko` 里有 21 个名字，缺 `bd_p2gl`/`bd_q200gl`）、`qdss_trace_config_v2.cfg` |
| `firmware/peach_v2/WCNSS_qcom_cfg.ini` | 8 645 B / 350 行 | 同目录 `wlan/qca_cld/peach_v2/` |
| `firmware/wlan_mac.bin` | 18 B | `/mnt/vendor/persist/wlan/` |
| `dt/fdt.dtb` | 1 880 491 B（ver 17） | `cat /sys/firmware/fdt` |
| `pci/ep-cfg.bin`、`pci/*.cfg` | 各 4 KiB | `dd .../0000:01:00.0/config` |
| `modules/*.ko`（4 个） | 1.5 MB | `/vendor/lib/modules/` 经 `su -c cp` 中转后 pull |
| 完整 `dmesg.txt` | 531 332 B / 3994 行 | `dmesg`（本仓只留 435 行过滤版） |

---

## 二、硬件事实（XNU 侧要照抄的部分）

### 2.1 PCI 身份

```
端点  0000:01:00.0   17cb:110e   class 0x028000 (network/other)   rev 01
根端口 0000:00:00.0   17cb:0123   class 0x060400 (PCIe RP)
modalias  pci:v000017CBd0000110Esv000017CBsd0000110Ebc02sc80i00
```

- `command = 0x0406` = Memory Space(bit1) + Bus Master(bit2) + **Interrupt Disable(bit10)**　← 注意这一位，Android 平时把中断关着
- `BAR0 = 0x40400000`，**2 MiB，64-bit，non-prefetch** —— 唯一的 MMIO 窗口（MHI doorbell / BHI 寄存器都在里面）
- `BAR2 = 0x40300000`，**32 KiB，64-bit** —— MSI-X 表所在 BAR
- 能力链（`parse_cfg.py` 实解，非猜）：
  - `0x40` Power Management → `0x50` MSI → `0x70` PCIe → `0xb0` MSI-X → 结束
  - **MSI msgctrl = 0x035b**：enable=1、**非 64-bit**、per-vector-mask=1、vectors=32；`addr = 0x18000040`、`data = 0x00441108`
  - **MSI-X msgctrl = 0x001f**：enable=1、mask=0、**table_size = 32**
    - **table = BAR2 + 0x0000**（BIR=2）→ 物理 `0x40300000`，32×16 B = 512 B
    - **PBA = BAR2 + 0x1000**（BIR=2）→ 物理 `0x40301000`
  - PCIe cap：`devcap=0x00008fc1`、`devctl=0x281f`、`devsta=0x0010`
  - `lnkcap=0x00434c23` → 上限 **8.0 GT/s ×2**；`lnksta=0x0011` → 当前 **2.5 GT/s ×1**（另一次采样是 5.0 GT/s ×1，说明链路会随 ASPM/L1 掉速，不是配置错）
- 扩展能力链：`0x100`(MFVS?) → `0x148` **AER** → `0x168` → `0x1f4` → `0x1fc`（AER 有多份副本，`uncorr_sev` 非 0 表示部分错误被当致命）
- `resource2`（MSI-X 表）**从用户态读报 `read error`** —— 表项只能通过内核态映射访问，XNU 侧要自己建 BAR2 映射

### 2.2 中断（**向量→用途是固定的，IRQ 号是动态的**）

`msi_irqs/` 里读到 **32 个连续 IRQ 号**（两次采样分别是 `382–413` 与后来的 `363–394`，因为我做过一次 runtime PM 循环导致重新枚举）——**32 = MSI-X table_size，完全对上**。

真正有用的是 **MSI-X 向量号 → 用途**的映射，这个两次采样完全一致：

| MSI-X 向量 | 用途 | 说明 |
|---|---|---|
| **0** | **`bhi`** | **M2 固件注入专用中断** |
| 1 / 2 | `mhi`, `mhi` | MHI 两条 doorbell |
| 3 | （空） | — |
| 4–11 | `pci0_wlan_ce_1..8` | 8 个 CE 环；其中 **vec 8 未绑（即 `ce_5` 缺失）** |
| 12 | （空） | — |
| 13 | `wlan_wake_irq` | 唤醒 |
| 14–31 | `pci0_wlan_grp_dp_0..17` | 18 条 DP 组；其中 **vec 18 / 22 未绑（`dp_4` / `dp_8` 缺失）** |

**未使用向量：3、12、18、22。**

旁路中断：
- `msmgpio 104` = `msm_pcie_wake`（4 次，CPU1）
- `msmgpio 205` = `wlan_dev_sol_irq`（1 次，CPU0）
- `GICv3 673` = `pcie_crm_sw_drv_0`

- `wlan-txrx-intr-cpumask = <0x3 0x30>` → TX/RX 中断分两组，分别绑 CPU0/1 与 CPU4/5
- **vector 0 是 BHI 专用** —— 这条中断是 M2 阶段「固件握手成功没有」的唯一信号源
- 计数分布很不均（`ce_2` 吃 1580 次、`ce_7` 4389 次，其它 0–560），这是 Android `smp_affinity` 默认值，XNU 侧可重排

> **XNU 侧只需记住向量号，不要记 IRQ 号** —— IRQ 号随枚举变化，向量号是 PCIe 规格里定义的。

### 2.3 内存

| 项 | 值 |
|---|---|
| `cnss_wlan_region` CMA | `base_pfn = 857088` → **物理 0xD1400000**，`count = 8192` 页 = **32 MiB**（`0xD1400000–0xD33FFFFF`），`order_per_bit = 0`，已用 5010 页 / 最大连续 3056 页 |
| CMA 在 iomem 的落点 | `d1400000-d57fffff : reserved`（该 reserved 段 68 MiB，CMA 只占前 32 MiB） |
| 端点 IOVA 窗口（partition 0） | `0x00000000 – 0x18000000`（384 MiB） |
| 端点 IOVA 窗口（partition 1） | `0xb0000000 – 0x100000000`（1.25 GiB） |
| IOMMU 几何 | `qcom,iommu-geometry = <0 0x18000000 0 0x1981>` → 36-bit IOVA，IPA 区 0x1981 页 |
| IOMMU 属性 | `pagetable=coherent`、`dma=fastmap`、`faults=stall-disable HUPCF non-fatal`、`msi-size=0x1000` |
| SMMU | `apps-smmu@15000000`（phandle 0x6b），`sid-base = 0x1400`；`iommu-map = <0x0 0x6b 0x1400 1  0x100 0x6b 0x1401 1>` |
| IPA 寄存器窗 | `qcom,cnss-peach@b0000000`，`reg = <0 0xb0000000 0 0x10000>`，reg-name `smmu_iova_ipa` |
| ramdump 预留 | `qcom,wlan-ramdump-dynamic = 0x600000`（6 MiB），从上面 32 MiB 里切 |

> 32 MiB CMA **必须整体连续**；ring 描述符、BHI 缓冲、fw 下载缓冲、ramdump 全在里面。XNU 侧得用 `IOMallocContiguous` 或直接在设备树保留区里划。
> **注意 `base_pfn` 会随开机变化**（Android 上是 CMA 动态分配），XNU 侧不能硬编码 0xD1400000，要在自己的物理内存布局里固定预留。

### 2.4 固件（**这是最容易踩空的一层**）

#### amss20.bin 的真实内部结构（已解出，不是"一个 7.3 MB 的 blob"）

```
0x000000  ┌─ ELF32 / EM_ARM (0x28)  type=ET_EXEC  entry=0x0180a800  phnum=7
          │   段：0x3000→0x017f5000 (0x15904)  0x19000→0x0180b000 (0xb6dc)
          │       0x25000→0x0181c000 (0x96c8)  0x2f000→0x01471000 (0x2d04)
          │       0x32000→0x01474000 (0x1630)  —— 合计 202 352 B
          │   （另有两个 PT_NULL 头段，0x114 + 0x2b80）
0x080000  ├─ ELF32 / **EM_HEXAGON (0xA4 = 164)**  type=ET_EXEC  entry=0x014ec9d8  phnum=21
          │   19 个 LOAD 段，合计 **6 773 872 B**，最远点到 0x67b040
          │   关键段：0x30 万字节级 0x2e7000→0x017f1000（2.9 MB 代码）
          │           0x10c040→0x016a0000（1.05 MB）
          │           0xf0000000 / 0xf0018000（外设窗口）  0xd4000000（4 B×2.2 K DMA 区）
0x67b040  └─ 512 KiB 尾部（高熵，非零分布均匀）—— 签名/哈希段
0x6fb040  文件结束
```

> **这直接决定了 M2 怎么写**：mhi.ko 是**不解析 ELF** 的，它把整个文件当 blob 往 BHIE 镜像窗口里灌，由 **WCN7850 芯片自己的 boot ROM** 去解析这两层 ELF。
> **但这不等于"整文件一次灌进去"** —— Linux 的 `fbc_download` 路径会按 `sbl_size = 0x80000` 把它切成两段：前 512 KiB 先投，剩下 6.48 MiB 再分 13 段投。`0x80000` 恰好是内层 Hexagon ELF 头的偏移，不是巧合。细节见 [M2](#m2--两段式固件投递这是全篇最容易写错的一步3-5-天级)。
>
> 同时也不要自己解 ELF 段表 —— 芯片 boot ROM 需要那个头。**只按 `0x80000` 这个偏移切一刀，其余原样搬运。**

#### 加载分工

| 文件 | 大小 | 谁加载 | 触发条件 |
|---|---|---|---|
| `peach/amss20.bin` | 7 319 616 B | **MHI/BHI**（`mhi_110e_00.01.00`） | 内核 `request_firmware` 失败 → ueventd 兜底；**XNU 侧必须自己读盘喂进 BHI** |
| `peach/regdb_xiaomi.bin` | 24 278 B | cnss（QMI 通道） | 固定路径，`Downloading REGDB` |
| `peach/bd_p2.elf` | 125 288 B | cnss（QMI 通道） | **BDF 板型文件**，19 个变体择一 |
| `peach/qdss_trace_config_v2.cfg` | 6 245 B | cnss | trace 配置，可省 |
| `wlan/qca_cld/peach_v2/WCNSS_qcom_cfg.ini` | 8 645 B | cnss | 350 行运行参数 |

- 字符串自报 `IMAGE_VARIANT_STRING = 7880.wlanfw.eval_v2_TOQ` → 内核是 **WCN7850（7880）家族**
- `bd_p2.elf` 是 ELF32 单段（`0x400` 起 `0x1e2d0`），内容是 `_binary_tempDir_bdwlan_bin` —— 把 bdwlan.bin 包了一层 ELF
- `phy_ucode20.elf` 3 段，串 `WLAN_UCODE_5.2.0-00000`

#### BDF 变体选择（已从 cnss2.ko 字符串表解出）

`cnss2.ko` 里的 BDF 名字表共 **21 个**：
```
bd_p1.elf  bd_p1a.elf  bd_p1gl.elf  bd_p1in.elf  bd_p2.elf  bd_p2gl.elf
bd_p3.elf  bd_p3gl.elf bd_p3in.elf  bd_p3s.elf   bd_p3sgl.elf  bd_p8.elf
bd_p11u.elf bd_p11ugl.elf bd_p11uin.elf  bd_q11.elf  bd_q11gl.elf
bd_q200.elf bd_q200gl.elf  bd_songyuan.elf  bd_songyuangl.elf
```
本机 `/vendor/firmware_mnt/image/peach/` 里**只有 19 个**（缺 `bd_p2gl.elf` 与 `bd_q200gl.elf`）—— 说明选型是驱动里按 `board_id + chip_id` 查表，`gl` 后缀是另一块板。

实测选中 **`bd_p2.elf`**，但 dmesg 里没有选型日志（prop 里 `ro.boot.hwlevel=MP`、`hwversion=1.9.0`、sku 空、project 空），**只能硬编码**，或用 `qcom,board-id = <0x08 0x00>` 去试映射。

- **CalDB = 0**：`CAL DB file not required as per BDF`、`Host BDF config: HW_XPA: 1 CalDB: 0 Radio OFF: 0` —— 不需要额外 cal 文件，但 `HW_XPA = 1` 说明外置 PA，XPA 相关寄存器必须配对
- 运行时版本自报：`Firmware build version : 2003ffff`、`Board version: 8 0 0 16 1`
- MHI 侧身份：`serial_number = 3216895231`、`OEMPKHASH[0..2] = 0x802240 / 0x20840070 / 0xae87fdfd`（安全启动哈希，XNU 侧如需校验要拿它比）

### 2.5 电源（8 条轨，`regulator_summary` 实测）

| 轨 | 供电对象 | 电压 | 模式 | 备注 |
|---|---|---|---|---|
| `pmh0104_s1` | `vdd-wlan-cx` | 892 mV | unknown | 范围 400–1000 mV，use/open = 1/2（与 BT 共享） |
| `pmh0104_s2` | `vdd-wlan-dig` | 892 mV | unknown | use/open = 4/4（cnss + BT 三路） |
| `pmh0110_f_s7` | `vdd-wlan-rfa2` | 1224 mV | fast | 与 BT rfa1p25、DSI 父轨共享 |
| `pmh0110_f_s8` | `vdd-wlan-rfa1` | 1800 mV | unknown | 与 BT rfa1p8 共享 |
| `pmh0110_g_l2` | `vdd-wlan-aon` | 1800 mV | idle | 常开 |
| `pmh0110_g_l3` | `vdd-wlan-io12` | 1200 mV | idle | 常开 |
| `pmr735d_l3` | `vdd-wlan-ant-share` | 1200 mV | standby | 天线共享开关 |
| `pmk8850_wcal_pbs` | `wcal-pbs` | 0 mV | unknown | **未启用**（use=0），XNU 侧可跳过 |

- `qcom,vreg_pdc_map`：`S1J1=bb  S2J1=rf  S7F0=rf  S8F0=rf`
- `qcom,pmu_vreg_map` 还把 `VDDD_AON_0P9 / VDDA_RFA_0P9 / VDDD_WLMX_0P9 / VDDD_BTCX_0P9 / VDDD_BTCMX_0P9` 全映射到 **S2J1**，`VDDD_WLCX_0P9` 到 **S1J1**，`VDDA_PCIE_0P9 / VDDA_PCIE_1P2` 到 **S7F0** —— **PCIe PHY 的模拟电源和 WiFi rfa2 是同一条 S7F0**，XNU 上电顺序错会打不出链路
- 上电约束表（hex，`min_uV / max_uV / headroom / ? / enable`）：cx/dig `0xd9c60–0xf4240`（892–1000 mV）、rfa1 `0x1ca020–0x1e8480`（1876–2000 mV）、rfa2 `0x144380–0x147260`（1328–1340 mV）、aon `0x1b7740 + 0x7530`、io12 `0x124f80`、ant-share `0x124f80 + 0x19`

### 2.6 时钟

- **`rf_clk` = 76 800 000 Hz**（`clk_summary` 里 `c1a_e0` = 76800000），来源 `rsc@18900000/drv@2/clock-controller` args `['6']`，**WiFi 与 BT（`bt_rf_clk1`）共享**
- `pcie_0_pipe_clk` 父 `gcc_pcie_0_pipe_clk_src` = **19 200 000 Hz**
- `gcc_pcie_0_slv_q2a_axi_clk / slv_axi / mstr_axi / cfg_ahb` 全 **0 Hz / N（deviceless）** —— 说明这些 AXI 时钟在 Android 上根本没被 cnss vote，只在 PCIe 总线访问时才由 RPMh 上电
- `qcom,aux-clk-freq = 0x14`（20 MHz）、`qcom,ep-latency = 0xa`（10）、`qcom,l1-2-th-scale = 2`、`qcom,l1-2-th-value = 150`、`qcom,drv-l1ss-timeout-us = 5000`

### 2.7 GPIO（TLMM 每 pin 一个 **4 KiB** 窗口，步长 `0x1000`，不是老 TLMM 的 `0x10`）

从 `fdt.dtb` 的 `/soc/pinctrl@f000000/cnss_pins` 五组原样解出：

| pin | 组名 | mux function | 电气配置（config 子节点） |
|---|---|---|---|
| 16 | `cnss_wlan_en_active` | `gpio` | `drive-strength = 16`（mA）、`bias-pull-up`、`output-high` |
| 16 | `cnss_wlan_en_sleep` | `gpio` | `drive-strength = 2`、`bias-pull-down`、`output-low` |
| 18 | `cnss_wlan_sw_ctrl` | **`wcn_sw_ctrl`**（专用功能，非 gpio） | 无 config |
| 19 | `cnss_wlan_sw_ctrl_wl_cx` | **`wcn_sw`**（专用功能） | 无 config |
| 204 | `cnss_host_sol_default` | `gpio` | `drive-strength = 4`、`bias-pull-down` |

运行期 `tlmm-pinmux-pins.txt` 实测：
```
pin 16  (GPIO_16):  b0000000.qcom,cnss-peach   function gpio  group gpio16
pin 18  (GPIO_18):  (MUX UNCLAIMED) (GPIO UNCLAIMED)     ← 未 mux，说明当前走默认
pin 19  (GPIO_19):  (MUX UNCLAIMED) (GPIO UNCLAIMED)
pin 102 (GPIO_102): 1c00000.pcie  function gpio  group gpio102   ← PERST#
pin 103 (GPIO_103): 1c00000.pcie  function pcie0_clk_req_n  group gpio103  ← CLKREQ#
pin 104 (GPIO_104): 1c00000.pcie  function gpio  group gpio104   ← WAKE#
pin 204 (GPIO_204): (MUX UNCLAIMED) f000000.pinctrl:716
pin 205 (GPIO_205): (MUX UNCLAIMED) f000000.pinctrl:717
```

`pinconf` 全部为空 —— 说明 pinconf 寄存器没有被 sysfs 暴露，配置写在 pinctrl 驱动里。

> **三个坑**：
> 1. **GPIO 18/19 必须 mux 到 `wcn_sw_ctrl` / `wcn_sw` 专用功能，不能当普通 gpio 写** —— 写成 gpio 会让 SW_CTRL 握手失效，固件以为主机没响应
> 2. **GPIO 18 是 WiFi 与 BT 共用的线**（`/soc/bt_peach` 的 `qcom,sw-ctrl-gpio` 与 `qcom,cnss-peach` 的 `qcom,sw-ctrl-gpio` 都是 18），XNU 侧必须做互斥
> 3. `pcie_clkreq` 是**硬件专用功能** `pcie0_clk_req_n`，且 `pcie-clkreq-offset = 0x2c48` 是 TLMM 内一个独立寄存器，不是普通 pin 配置

### 2.8 PCIe 寄存器窗（六段，全在 DT）

| 名字 | 物理基址 | 大小 | 用途 |
|---|---|---|---|
| `parf` | `0x01c00000` | `0x3000` | 中断掩码 / 状态机 / testbus |
| `phy` | `0x01c06000` | `0x2000` | PHY 状态（`0x414` bit7 = ready） |
| `dm_core` | `0x400000000` | `0x10000000` | DBI（只用前 4 KiB） |
| `elbi` | `0x40f200000` | `0xa8` | ELBI（**单独访问会死机**） |
| `iatu` | `0x410000000` | `0x1000` | **iATU —— 出站映射，XNU 侧必须自己配** |
| `pcie_sm` | `0x01d07000` | `0x7000` | 链路状态机调试字 |

- `ranges`：io `0x01000000@0x40200000`(16 MiB)、mem `0x02000000@0x40300000`(0x3d00000)、prefetch `0x43000000@0x1010000000`(0x2eff00000)
- `qcom,phy-sequence` = **2388 B = 199 个 (offset, mask, value) 三元组** —— 这是 PHY 初始化表，必须原样搬
- `interrupts` 5 组 SPI：140（int_global）、149/150/151/152（int_a..d）
- `resets`：`pcie_0_core_reset`、`pcie_0_phy_reset`（GDSC 在 `gcc 0x6b004` / `0x6c000`）

---

## 三、Android 侧的完整启动时序（dmesg 原文，可逐步复现）

```
 9.376  cnss: QMI DMS service connected, state: 0x2f0600f          ← MAC 来源通道先起
14.258  cnss: Calibration took 8448 ms                             ← 校准窗口，早于 PCIe 上电
14.560  cnss: CAL DB file not required as per BDF                   ← CalDB=0
14.560  cnss: QMI WLFW service disconnected, state: 0x32fa6000      ← 先断，为复位固件
14.560  cnss: Device id: 0x110e
        msm_pcie_disable: Assert the reset of endpoint of RC0
        msm_pcie_config_perst: RC0: assert PERST
        msm_pcie_enable: PCIe: Assert the reset of endpoint of RC0
        msm_pcie_config_perst: RC0: assert PERST
        msm_pcie_enable_link: PCIE20_PARF_INT_ALL_MASK: 0x7f80d202
        msm_pcie_enable_link: RC:0 PARF_INT_ALL_2_TYPE: 0x410
        pcie_phy_init: PCIe RC0 PHY is ready!
        msm_pcie_enable_link: Release the reset of endpoint of RC0
        msm_pcie_config_perst: RC0: de-assert PERST
        msm_pcie_link_train: PCIe RC0 link initialized              ← 链路通
14.596  mhi mhi_110e_00.01.00: Direct firmware load for peach/amss20.bin failed (-2)
14.598  ueventd: loading ... peach/amss20.bin took 1ms              ← 固件进 BHI
15.203  cnss: QMI WLFW service connected, state: 0x46f26011         ← 固件活了
15.217  cnss: tgt cap response direct cx data pin mode status: 0
15.217  cnss: serial id 0x0 0x0
15.220  cnss: Downloading REGDB: peach/regdb_xiaomi.bin, size: 24278
15.242  cnss: Downloading BDF: peach/bd_p2.elf, size: 125288
15.291  cnss: Host BDF config: HW_XPA: 1 CalDB: 0 Radio OFF: 0
15.317  cnss: Received DMS MAC: [a4:ff:9f:96:67:15]
15.444  cnss: Unable to get ipa iommu group phandle                 ← IPA 卸载放弃（可不管）
15.454  cnss: CNSS and direct link share IOMMU group                ← 共用 SMMU
15.609  peach_v2: [WMI] extract_hw_bdf_status: FW and BDF ver check skipped
15.858  cnss_utils: WLAN MAC address is not set, type 0
15.889  cnss: Failed to get cooling device node                     ← 温控可选
```

**注意三件事**：
1. **校准（8448 ms）在 PCIe 上电之前就结束了** —— WCN7850 的 cal 是固件内置（CalDB=0），主机只是等 QMI 回报，不做校准算法
2. **WLFW 先 disconnect 再 connect** —— 复位固件必须走这条「断-复位-重连」，不能省
3. 从 **PERST de-assert 到 QMI 连上只有 640 ms**，其中 amss20.bin（7.3 MB）注入只用 1 ms（走 DMA，不是 MMIO 逐字写）

---

## 四、对照 `popsicle_wifi.c` 的现状

你现有代码（619 行）做到的是：

| 阶段 | 状态 |
|---|---|
| TLMM 4 KiB 步长读写、gpio16/18 上电 | ✅ 有（`wifi_step0/1`） |
| PARF / PHY / ELBI / SM 映射与 dump | ✅ 有（`wifi_step2/2b/2c/2d`） |
| GDSC 上电（`gcc 0x6b004` / `0x6c000`） | ✅ 有（`wifi_step4`） |
| **PERST 时序（assert → PHY ready → de-assert）** | ❌ 缺 |
| SMMU sid 0x1400 建立 | ❌ 缺 |
| **PERST 之后的 iATU 出站映射** | ❌ 缺 —— **BAR0 读不到的直接原因** |
| BHI/BHIE 两段式固件投递 | ❌ 缺 |
| MHI 通道 / ring / doorbell | ❌ 缺 |
| QMI / QRTR（WLFW 服务） | ❌ 缺（注释已写「MHI 通道未实现」） |
| CE 环（8 个）+ MSI-X 32 向量 | ❌ 缺 |
| WMI / HDD（vdev、扫描、连接） | ❌ 缺 |

**结论：现有代码停在「PCIe 链路层探测」，距离能收包还差整个 host-firmware 协议栈（约 MHI+BHI+QMI+CE+WMI 五层）。**

---

## 五、XNU 侧要照着实现什么（按可验证里程碑切）

### M1 — 让 BAR0 可读（1 天级）

```
1. GDSC 上电（已有：GCC_PCIE_GDSC 0x6b004 / GCC_PCIE_PHY_GDSC 0x6c000）
2. 写 PARF 0x00 的 INT_ALL_MASK = 0x7f80d202（照抄 dmesg 实测值）
3. perst-gpio(102) assert → 延时 → 轮询 PHY ready（parf[0x414] bit7）→ de-assert
4. 轮询 pcie_sm[0x1090] / pcie_sm[0x1000] 直到链路态；14 个 debug word 在
   pcie_sm[0x1040..0x109c]（步长 8），全部有值说明 state machine 走完了
5. 配 iATU（在 0x410000000，不是 0x1c00000）：
   outbound window 0：目标 0x40400000 (2 MiB) → bus1/dev0/fn0 BAR0
   outbound window 1：目标 0x40300000 (32 KiB) → BAR2
   viewport：CTRL_2 = 0x80000000 | (index << 16)
6. 读 0x100 / 0x104 (vendor/device id) 应得 0x17cb / 0x110e
```

**iATU 的关键事实**（这三条错了就是黑屏重启）：

| 项 | 值 | 出处 |
|---|---|---|
| iATU 基址 | `0x410000000`（>4 GiB） | DT `reg` 第 5 段 `iatu` |
| iATU 大小 | `0x1000` | 同上 |
| outbound 目标 | `bus 1 / dev 0 / fn 0` | 端点 `0000:01:00.0` |
| 端点 BAR0 物理 | `0x40400000` | `pci-resource.txt` |
| 端点 BAR2 物理 | `0x40300000` | 同上 |
| RC mem window | `0x40300000–0x405fffff` | `lspci` 根端口 `0x20` 寄存器 |

> 里程碑只做到「读到 `0x17cb/0x110e`」，**不做任何写**。读成功就说明 iATU 方向对了；一旦写错方向，端点会进不可恢复状态，必须重启手机。

`popsicle_wifi.c` 里现在的 `wifi_step2c_link()` 只读 `parf[0x414] bit7`，**少了 iATU 这一步，所以永远读不到 BAR0** —— 这就是为什么代码注释里写"ELBI 单独访问会死机"：没有 iATU 时访问 0x40400000 会打到 RC 自己的地址空间。

### M2 — 两段式固件投递（**这是全篇最容易写错的一步**）（3–5 天级）

Linux 的 `mhi.ko` 走的是 **FBC（Firmware Broadcast / fbc_download）** 路径，`ath12k` 给的参数是：

```
sbl_size = SZ_512K = 0x80000
seg_len  = SZ_512K = 0x80000
fbc_download = true
```

而 `0x80000` **正好是 `amss20.bin` 里内层 Hexagon ELF 头的文件偏移**（`elfinfo.py` 实测内层头在 0x080000）。这不是巧合 —— 两段式的分界就落在这里：

```
阶段 A（BHI）   [0x000000, 0x80000)  524 288 B   ← 外层 ARM stub，一次整块投递
阶段 B（BHIE）  [0x80000, 0x6fb040)  6 795 328 B ← 内层 Hexagon 固件，分 13 段
                                                  每段 524 288 B，末段 503 872 B
```

`drivers/bus/mhi/host/boot.c` 的原文逻辑（`mhi_fw_load_handler`）：

```c
size = fbc_download ? sbl_size : firmware->size;   /* 阶段 A 只取前 512 KiB */
mhi_load_image_bhie/bhi(mhi_cntrl, fw_data, size); /* 阶段 A 走 BHI 或 BHIE */

/* 阶段 B：检测 sbl_size 之后有没有第二个 ELF 头 */
if (!memcmp(fw_data + sbl_size, ELFMAG, SELFMAG)) {
        fw_data += sbl_size;
        fw_sz   -= sbl_size;
}
mhi_alloc_bhie_table(mhi_cntrl, &fbc_image, fw_sz);  /* 13 段 + 1 张向量表 */
mhi_firmware_copy_bhie(mhi_cntrl, fw_data, fw_sz, fbc_image);
```

**关键点**：阶段 A 实际上**没有走 `mhi_load_image_bhi`**，因为 `seg_len` 非 0 → `mhi_fw_load_type_get()` 返回 `MHI_FW_LOAD_BHIE`，走的是 `mhi_load_image_bhie`。所以**两段都用 BHIE**，BHI 只用于读 serial number 和最后的 `BHI_STATUS` 错误码。

#### BHI / BHIE 寄存器偏移（从 `drivers/bus/mhi/common.h` 取，直接可用）

`BHIOFF` / `BHIEOFF` 不是硬编码 —— **从 BAR0 的 0x28 / 0x2c 读出来**，再校验 `< reg_len`：

```c
bhi_off  = read32(bar0 + 0x28);   /* BHIOFF  */
bhie_off = read32(bar0 + 0x2c);   /* BHIEOFF */
```

BHI 块（相对 `bhi_off`）：

```c
#define BHI_BHIVERSION_MINOR      0x00
#define BHI_BHIVERSION_MAJOR      0x04
#define BHI_IMGADDR_LOW           0x08
#define BHI_IMGADDR_HIGH          0x0c
#define BHI_IMGSIZE               0x10
#define BHI_IMGTXDB               0x18   /* 写 session_id 触发 */
#define BHI_INTVEC                0x20
#define BHI_EXECENV               0x28
#define BHI_STATUS                0x2c   /* bits 31:30 = 状态 */
#define BHI_ERRCODE               0x30
#define BHI_ERRDBG1               0x34
#define BHI_ERRDBG2               0x38
#define BHI_ERRDBG3               0x3c
#define BHI_SERIALNU              0x40
#define BHI_SBLANTIROLLVER        0x44
#define BHI_NUMSEG                0x48
#define BHI_MSMHWID(n)            (0x4c + 4*n)   /* n = 0..5 */
#define BHI_OEMPKHASH(n)          (0x64 + 4*n)   /* n = 0..23 */

/* BHI_STATUS 取值 */
#define BHI_STATUS_RESET     0x00
#define BHI_STATUS_SUCCESS   0x02
#define BHI_STATUS_ERROR     0x03
```

BHIE 块（相对 `bhie_off`）：

```c
#define BHIE_MSMSOCID_OFFS         0x00
#define BHIE_TXVECADDR_LOW_OFFS    0x2c
#define BHIE_TXVECADDR_HIGH_OFFS   0x30
#define BHIE_TXVECSIZE_OFFS        0x34
#define BHIE_TXVECDB_OFFS          0x3c   /* bits 29:0 = sequence_id，写入即触发 */
#define BHIE_TXVECSTATUS_OFFS      0x44   /* bits 31:30 = 状态，29:0 = seq */
#define BHIE_RXVECADDR_LOW_OFFS    0x60
#define BHIE_RXVECADDR_HIGH_OFFS   0x64
#define BHIE_RXVECSIZE_OFFS        0x68
#define BHIE_RXVECDB_OFFS          0x70
#define BHIE_RXVECSTATUS_OFFS      0x78

#define BHIE_TXVECSTATUS_STATUS_RESET       0x00
#define BHIE_TXVECSTATUS_STATUS_XFER_COMPL  0x02
#define BHIE_TXVECSTATUS_STATUS_ERROR       0x03
```

MHI 主块（相对 BAR0 基址，**不是相对 bhi_off**）：

```c
#define MHIREGLEN            0x00
#define MHIVER               0x08
#define MHICFG               0x10   /* NHWER[31:24] NER[23:16] NHWCH[15:8] NCH[7:0] */
#define CHDBOFF              0x18
#define ERDBOFF              0x20
#define MHICTRL              0x38   /* bit1 = RESET，bits 15:8 = MHISTATE */
#define MHISTATUS            0x48   /* bit0 READY、bit2 SYSERR、bits15:8 MHISTATE */
#define CCABAP_LOWER/HIGHER  0x58 / 0x5c
#define ECABAP_LOWER/HIGHER  0x60 / 0x64
#define CRCBAP_LOWER/HIGHER  0x68 / 0x6c
#define CRDB_LOWER/HIGHER    0x70 / 0x74
#define MHICTRLBASE_LOWER    0x80
#define MHICTRLLIMIT_LOWER   0x88
#define MHIDATABASE_LOWER    0x98
#define MHIDATALIMIT_LOWER   0xa0
```

#### 投递流程（可直接照写）

```
1. 读 BAR0+0x28 / BAR0+0x2c → bhi_off、bhie_off；两者都必须 < 2 MiB
2. 读 bhi+SERIALNU(0x40) 存档（Android 上是 3216895231）
3. 阶段 A：把 amss20.bin 的 [0, 0x80000) 拷进 512 KiB 物理连续 DMA 缓冲
   ├ 填 BHIE 向量表（此时只有 1 项）：{dma_addr, 0x80000}
   ├ 写 BHIE_TXVECADDR_LOW/HIGH = 向量表物理地址
   ├ 写 BHIE_TXVECSIZE = 16（1 项 × 16 B）
   ├ 写 BHIE_TXVECDB = seq（非 0 随机）
   └ 轮询 BHIE_TXVECSTATUS[31:30] == 0x02，超时读 BHI_ERRCODE/ERRDBG1..3
4. 阶段 B：把 [0x80000, 0x6fb040) 按 512 KiB 切成 13 块
   ├ 每块各自 dma_alloc_coherent(512 KiB)，末块 503 872 B
   ├ 向量表 13 项 × 16 B（struct bhi_vec_entry { __le64 dma_addr; __le64 size; }）
   ├ 同样写 TXVECADDR/TXVECSIZE(=208)/TXVECDB，轮询 XFER_COMPL
   └ 这一步由固件主动来取（BHIE = 设备侧 DMA 读主机内存）
5. 全部完成后等 EE 变成 MHI_EE_WFW（"WLAN FIRMWARE"）
6. 之后才轮到 MHI 的 MHICFG / CHDBOFF / ERDBOFF 与通道 ring
```

**三个必须注意的细节**：

- `bhi_vec_entry` 是 **16 字节**（`__le64 dma_addr` + `__le64 size`），不是 8 字节
- BHIE 的向量表本身也是一块 DMA 缓冲，它的物理地址写进 `TXVECADDR`，`TXVECSIZE` 写的是**向量表字节数**（不是固件字节数）
- 阶段 B 的 `TXVECSIZE`（固件总字节）与 `TXVECDB` 是分开写的；`mhi_fw_load_bhie()` 里 `mhi_buf->len` 用的是**向量表的长度**（`image_info->mhi_buf[entries-1].len`）

参考资料（Linux 上游，未入库）：`drivers/bus/mhi/common.h`（寄存器表）、`drivers/bus/mhi/host/boot.c`（加载逻辑）、`drivers/bus/mhi/host/init.c`（BHI 偏移读法）、`drivers/net/wireless/ath/ath12k/mhi.c`（`sbl_size`/`seg_len`/`fbc_download` 三个参数的来源）、`ath12k/pci.c`。

### M3 — MHI 通道 + QRTR（1 周级）

- ring 内存从 **32 MiB CMA** 切（Android 上这次落在 `0xD1400000`，但 **`base_pfn` 每次开机变**，XNU 必须自己固定预留）
- 需要的通道：`qcom,qrtr-mhi-cnss` 给了 `dev-id = 0x1111`、`net-id = 1` —— 主机侧起一个 QRTR net 设备，固件侧通过 MHI 通道 0 通信
- 32 MiB 里要预留：ring 描述符环、BHI 缓冲、ramdump 6 MiB

### M4 — QMI WLFW 服务（1 周级）

按 dmesg 顺序实现 4 个事务：
1. `WLFW_BIND_MHI_CHANNELS`
2. `WLFW_INIT` → 拿 `tgt cap response`（`direct cx data pin mode`）
3. `WLFW_DOWNLOAD_BDF`（24278 B regdb + 125288 B bd_p2.elf）
4. `WLFW_CAP` / `WLFW_QMI_DMS` 取 MAC

### M5 — CE + MSI-X（1 周级）

- 8 个 CE 环（`pci0_wlan_ce_1..8`），源/目的环各一；ring 内存从 32 MiB CMA 切
- **MSI-X 表在 `BAR2 + 0x0000`**（不是 0x1000！BIR=2、offset=0x0000，`parse_cfg.py` 实解）
- **PBA 在 `BAR2 + 0x1000`**
- 每个表项 16 B：`{u32 addr_lo, u32 addr_hi, u32 data, u32 ctrl}`；`ctrl` bit0 是 mask
- 32 个向量的用途分配（两次采样一致，直接照抄）：

```
vec  0      BHI
vec  1, 2   MHI
vec  4..11  CE1..CE8          （vec 8 = ce_5 在 Android 上未绑，但不代表不能用）
vec 13      wake
vec 14..31  DP0..DP17         （vec 18 = dp_4、vec 22 = dp_8 未绑）
vec  3, 12, 18, 22   未用
```

- **只记向量号，不记 IRQ 号** —— 实测同一份代码两次运行，IRQ 号从 `382–413` 变成 `363–394`（一次 runtime PM 循环导致重新枚举）。向量号才是硬件常量。

### M6 — iOS 用户态对接（最大不确定项）

XNU 上要出 `en0` 并让 iOS 的 `wifid` / `airportd` 认得，需要：

- 一个 `ifnet` 注册（**DHCP 能通 = 最小可用里程碑**）
- `IO80211Family` 的 ioctl 面（扫描结果、关联状态）—— 真机 Broadcom 走 `AppleBCMWLAN`，Qualcomm 上得自己写 shim
- `Airport` 的 `SIOCSAIRPORTSSID` 等 ioctl 必须响应，否则 `Settings` 里 WiFi 开关会一直转

> **建议顺序**：M1 → M2 → M3 打通「固件活着」→ 先只实现 `WLFW_CAP` 打印版本（证明端到端），再往 M5/M6 推。M6 之前全是可移植 C，和 Android 侧一一对应。
>
> M6 的降级方案：先把 WiFi 当普通 `ifnet` 跑通（`ifconfig` 能出、能 DHCP），不接 `IO80211Family` —— iOS 上会显示成有线网卡，但至少能上网，这比卡在 `airportd` 里强。

## 六、五个会直接卡死的坑

1. **BAR0 读不到不是寄存器错，是 iATU 没配** —— 现有代码全部只做了 RC 侧，没有 outbound window，所以对 0x40400000 的访问根本到不了端点。设备上从用户态直接 `dd resource0` 返回 `I/O error`，就是这个原因（不是驱动占用）。
2. **iATU 在 0x410000000**（40 亿以上物理地址）—— XNU 的 `ml_io_map` 对 >4 GiB 需要显式处理，`ml_io_map_unmappable` 映射是**永久**的（你自己代码注释里写了 `pandora_dpu.c r1b`），映射错了只能重启。
3. **amss20.bin 是两层 ELF，投递要按 `sbl_size = 0x80000` 在两段之间切开，且两段都走 BHIE** —— 外层 `EM_ARM`（stub，前 512 KiB），内层 `EM_HEXAGON`（6.48 MiB 真固件，entry `0x014ec9d8`，分 13 段）。**别自己解 ELF 段表**（boot ROM 要那个头）；**也别整文件一次性灌**（`fbc_download` 路径明确只先投 512 KiB）。分界点 0x80000 正好落在第二个 ELF 头上，见 M2。
4. **`sw_ctrl` GPIO 18 与蓝牙共用**，而且 **GPIO 18/19 必须 mux 到 `wcn_sw_ctrl`/`wcn_sw` 专用功能**，写成普通 gpio 会让 SW_CTRL 握手失效，固件以为主机没响应 —— WiFi 和 BT 会一起挂。
5. **`cnss_wlan_region` 的 `base_pfn` 每次开机都变**（Android 靠 CMA 动态分配，本次是 `0xD1400000`）—— XNU 侧不能硬编码这个地址，必须在自己的物理内存布局里固定预留 32 MiB 连续区。

---

## 七、还缺的（如果要更完整）

| 缺项 | 状态 | 怎么补 |
|---|---|---|
| BHI / BHIE 寄存器偏移表 | **已解决** | 从 `drivers/bus/mhi/common.h` 取，已抄进 M2；偏移本身从 `BAR0+0x28`/`0x2c` 动态读 |
| 固件投递的分段参数（`sbl_size`/`seg_len`/`fbc_download`） | **已解决** | `ath12k-mhi.c` L256-258：`512 KiB` / `512 KiB` / `true` |
| MHI 通道配置（channel id / 方向 / ring 大小） | 待补 | DT 里没有 `qcom,mhi-channels`。可从 `mhi.ko` 的 `mhi_register_controller` 调用点反出 `mhi_config` 表，或直接读运行中设备的 `MHICFG`/`CHDBOFF`/`ERDBOFF` |
| `bd_p2` 的选择依据 | 待补 | dmesg 无线索。反 `cnss2.ko` 的 `cnss_get_bdf_file_name()`，或用 `qcom,board-id = <0x08 0x00>` 硬映射 |
| 完整 PHY 序列 199 组 | **已提取** | `fdt.dtb` 里 `qcom,phy-sequence`，2388 字节，已解析到 `popsicle_wifi_names.h` |
| 射频校准数据 | 不需要 | `CalDB = 0`，日志明确 `CAL DB file not required as per BDF` |
| WMI 命令集（连 AP 用） | 待补 | 固件就绪后从 `WCNSS_qcom_cfg.ini` + `wlan_fw_api.bin` 的 TLV 头对齐；短期内可先只做 beacon 扫描 |

---

## 八、可以立刻动手的三件事（不依赖任何缺失信息）

1. **把 `popsicle_wifi_names.h` 直接 include 进 `popsicle_wifi.c`** —— 81 个常量全部来自真机实测，替换掉代码里手抄的魔数，能立刻消除 `TLMM_STRIDE`、`GPIO_PIN_MAX` 这类抄错风险
2. **`wifi_step2c_link()` 的判据改成读 `lnksta`**（`0x40400000 + 0x?` 之外的 RC 侧 `PARF`）：现在只读 `parf[0x414] bit7`，但 dmesg 里 PHY ready 之后还有 `msm_pcie_link_train` 这一步，链路速率要额外确认（本次实测 `lnksta = 0x0011` = 2.5 GT/s ×1，另一采样是 5.0 GT/s ×1 —— **说明链路协商不稳定，这一点在 Android 上就已经存在**）
3. **先只做 M1 的「读到 `0x17cb/0x110e`」当里程碑**，不做任何写。读完 id 再往下走 BHI，否则一旦 iATU 配错方向，写操作会把端点打进不可恢复状态（要重启手机）

---

*本文所有数值均来自 `adb -s 6f17201d`（Xiaomi 17 Pro Max / popsicle / SM8850）真机提取，文本类提取物已随本文入库，见 `wifi-extract/`。*
