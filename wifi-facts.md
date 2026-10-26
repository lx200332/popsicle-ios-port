# popsicle Wi-Fi 驱动预研（④）— 硬件事实与 XNU 移植评估

> 范围：iOS 16.5 hackios 移植 · Xiaomi 17 Pro Max（SM8850/canoe, codename popsicle）
> 证据来源：`<TOOLS>\cache\popsicle.dtb`（厂商合并 DTB）、`<DUMPS>\tiqu\02-devicetree\live-fdt.dtb`（真机 DTB）、
> `<DUMPS>\tiqu\10-props-logs\dmesg-full.txt`（真机内核日志）、`<DUMPS>\tiqu\09-vendor-config\`（vendor 分区提取）、
> MiCode `xk-dt`（WSL `<BUILD>/xk-dt/qcom/`）、soc-repo `<BUILD>/xk-popsicle/`、hackios xnu-8796.121.2 `bsd/redwood_wifi.c`。
> 约定：查不到的写「无据」，不臆测。DTB 值以真机 live-fdt.dtb 与厂商合并 DTB 相互印证为准。

---

## 0. 结论摘要（先读这里）

1. **芯片**：真机 PCI device id = **0x110e**（DTB `supported-ids` + dmesg `cnss: Device id: 0x110e` 双重证实），挂在 **PCIe0 根端口** 下，BDF **0000:01:00.0**，MHI 设备名 `mhi_110e_00.01.00`，MSI 传输（32 个 MSI 事件经 GIC ITS）。**非 WPSS 嵌入式方案**（live-fdt 字节级无 wpss 节点；对比之下 alor.dtsi 的 WPSS/WCN7750 是另一个平台方案）。
2. **驱动**：真机运行的是 Qualcomm **qca_cld**（cnss2 家族）vendor 驱动，板级名 **peach_v2**（dmesg 加载 `wlan/qca_cld/peach_v2/WCNSS_qcom_cfg.ini`，日志前缀 `wlan_pld:`）。0x110e 不在 mainline **ath12k** PCI 表（1107/1109/1112），但架构同族（PCIe + MHI + QMI WLFW + amss 固件），mainline ath12k 侧只需 **加一行 PCI id + peach 板文件** 即具可比性。
3. **移植骨架**：redwood（WCN6750 WPSS + ath11k）的引导链要整体替换为 **PCIe 枚举 → PERST 释放 → MHI(dev_id 0x110e, 固件 peach/amss20.bin) → QMI WLFW 服务(经 qcom,qrtr-mhi-cnss dev-id 0x1111) → qca_cld/ath12k 对象**；XNU 侧沿用 `redwood_lx.c` 的「树外驱动编成一个对象 + lx_os_* shim」思路。
4. **主要阻塞点**：0x110e 需新 PCI id；BDF（bd_p2.elf）与固件必须从真机 vendor 分区取；8 路 RPMh 供电 + TLMM 16/18/19/204/205 + PMIC GPIO5 的供电/复位时序要在 XNU 重放；cnss-peach 平台驱动源码本机**无据**。

---

## 1. Wi-Fi 控制器型号判定

| 证据 | 值 | 来源 |
|---|---|---|
| DTB `supported-ids` | `<0x110e>` | cnss-peach 节点（live-fdt + 合并 DTB 一致） |
| dmesg | `cnss: Device id: 0x110e`（L249–250，出现两次） | dmesg-full.txt |
| dmesg MHI 设备名 | `mhi mhi_110e_00.01.00` | dmesg L263 / L2949 |
| PCI BDF | `0000:01:00.0`（wlan_mac.bin 固件加载路径 `/devices/platform/soc/1c00000.pcie/pci0000:00/0000:00:00.0/0000:01:00.0/firmware/...`） | dmesg L507 |
| compatible | `qcom,cnss-peach` | DTB 节点名 `qcom,cnss-peach@b0000000` |

- **ath12k mainline 对照**：`drivers/net/wireless/ath/ath12k/wifi7/pci.c` 仅含 QCN9274=0x1109、WCN7850=0x1107、QCC2072=0x1112；**0x110e 不在表内**。
- **ath11k 对照**：0x1103=WCN6855（ath11k）；FastConnect 7800 = WCN7850 = 17cb:1107。0x110e 与上述均不同。
- **0x110e 的具体营销名（如 FastConnect 7900）本机材料无直接证据**。用户口径「FastConnect 7900 / qca_cld3_peach_v2」中，**qca_cld3_peach_v2 的板级配置已在本机材料证实**（见 §5），营销名不作证、不否定。
- 无 WPSS：live-fdt.dtb 字节级确认 `wpss:false`、`wcn7850:false`、`ath12k:false`（字符串检索无命中）→ 明确是 **PCIe 直连卡**。

---

## 2. DTB：cnss-peach 节点（平台驱动侧）

节点：`/soc/qcom,cnss-peach@b0000000`，`compatible = "qcom,cnss-peach"`

| 属性 | 值 | 说明 |
|---|---|---|
| `reg` | `<0x0 0xb0000000 0x0 0x10000>` | reg-names=`"smmu_iova_ipa"` —— CNSS 平台驱动地址，**不是**控制器 BAR |
| `clocks` | `<rpmhcc 6>` = `"rf_clk"` | RPMH_RF_CLK1=6（`qcom,rpmh.h:15`：CXO=0..LN_BB=3..RF_CLK1=6） |
| `nvmem-cells` | `pmk8850@0/sdam@7100/xo_calib-data@59`（reg `<0x59 0x1>`），name=`"xo_calib_reg"` | XO 校准位 |
| `qcom,qmp` | `aoss` | AOP QMP 通道 |
| `supported-ids` | `<0x110e>` | PCI device id 白名单 |

→ 该节点只描述平台胶水（时钟/校准/QMP），真正的设备资源在 PCIe 端点 BAR 上，由 PCIe 枚举给出。

---

## 3. PCIe0 根端口（真正的总线资源）

节点：`/soc/pcie@1c00000`，compatible `qcom,pci-msm`；驱动名 dmesg 里为 `msm_pcie`（RC0）。

**寄存器块（reg + reg-names）**

| name | 基址 | 大小 |
|---|---|---|
| parf | 0x01c00000 | 0x3000 |
| phy | 0x01c06000 | 0x2000 |
| dm_core | 0x4_00000000（<0x4 0x0>） | 0x10000000 |
| elbi | 0x4_00000f20 | 0xa8 |
| iatu | 0x4_10000000 | 0x1000 |
| pcie_sm | 0x01d07000 | 0x7000 |
| mhi（alor-pcie.dtsi 源码有；真机 DTB reg-names 无该项） | 0x01c03000 | 0x1000 |

**中断（GIC SPI，level-high）**

| 名称 | GIC SPI |
|---|---|
| global_int | 140 (0x8c) |
| int_a / int_b / int_c / int_d | 149 / 150 / 151 / 152 (0x95–0x98) |

**MSI/SMMU 映射**：`msi-map` base=0x1400（32 事件 0x1400–0x141f，经 gic_its）；`iommu-map`/SID base=0x1400；`pcie_qtb@16cd000` TBU stream-id-range 0x1400–0x17ff。真机 interrupts.txt：`ITS-PCI-MSI-0000:01:00.0`，中断名 `pci0_wlan_ce_1..8`、`wlan_wake_irq`、`pci0_wlan_grp_dp_0..12`（CE=copy engine 完成、DP=数据路径）。

**GPIO/复位/时钟**：perst=TLMM 102、clkreq=TLMM 103、wake=TLMM 104；`resets=<gcc 6>`（GCC_PCIE_0_BCR，qcom,alor-gcc.h:208）、`<gcc 9>`（GCC_PCIE_0_PHY_BCR，:211）；`target-link-speed=3`（**Gen3**）。内核 cmdline 带 `pci-msm-drv.pcie_sm_regs=0x1D07000,0x1040,0x1048,0x3000,1`。

**子节点 `pcie_rp/cnss_pci0`**：`memory-region=<cnss_wlan_region>`（32MB shared-dma-pool）；`iommu-addresses` 两分区（0x0–0x18000000、0xb0000000–0xb5000000）；`qcom,iommu-group=cnss_audio_iommu_group0`（fastmap/coherent）。

**真机 iomem.txt**：`40300000-43ffffff` 与 `410100000-6ffffffff` 归属 pcie@1c00000（64 位 BAR 窗口）。

**dmesg 链路时序**：`msm_pcie_enable` → `pcie_phy_init`（PHY ready）→ PERST de-assert → `link_train` RC0 link initialized（~14.58s）；日志有两次完整链路初始化（L233–262 与 L2935–2948，第二次应为固件重启后重连）。

---

## 4. 供电 / GPIO / 时钟（时序重放清单）

**供电（RPMh 域，phandle 已解析）**

| 节点属性 | 电源轨 | PMIC 输出 |
|---|---|---|
| vdd-wlan-aon | 常开域 | pmh0110_g_l2 |
| vdd-wlan-io12 | IO 1.2V | pmh0110_g_l3 |
| vdd-wlan-cx | CX | pmh0104_s1 (S1J) |
| vdd-wlan-dig | 数字 | pmh0104_s2 (S2J) |
| vdd-wlan-rfa1 | RF 模拟 1 | pmh0110_f_s8 (S8F) |
| vdd-wlan-rfa2 | RF 模拟 2 | pmh0110_f_s7 (S7F) |
| vdd-wlan-ant-share | 天线共享 | pmr735d_l3 (L3K) |
| wcal-pbs | 校准 | pmk8850_wcal_pbs |

**pmu_vreg_map 字符串**（cnss-peach 节点内）：VDDD_AON_0P9 / VDDD_WLMX_0P9 / VDDD_BTCX_0P9 / VDDD_BTCMX_0P9 → **S2J1**；VDDD_WLCX_0P9 → **S1J1**；VDDA_RFA_1P3 / VDDA_PCIE_0P9 / VDDA_PCIE_1P2 → **S7F0**；VDDA_RFA_1P9 → **S8F0**。
**vreg_pdc_map**：S1J1→bb、S2J1→rf、S7F0→rf、S8F0→rf（PDC 电源塌陷域映射）。

**GPIO（TLMM，`soc/pinctrl@f000000`，compatible `qcom,canoe-tlmm`，reg 0xf000000+0x202000）**

| 信号 | GPIO |
|---|---|
| wlan-en-gpio | TLMM 16 |
| qcom,sw-ctrl-gpio | 18 |
| qcom,wlan-sw-ctrl-gpio | 19 |
| wlan-host-sol-gpio | 204 |
| wlan-dev-sol-gpio | 205 |
| mpm_wake_set_gpios | <18 19> |
| qcom,bt-en-gpio | pmh0104 PMIC GPIO 5（SPMI pinctrl@8800） |

pinctrl 组 `cnss_pins`：wlan_en_active/sleep（gpio16）、sw_ctrl（gpio18, fn wcn_sw_ctrl）、sw_ctrl_wl_cx（gpio19, fn wcn_sw）、host_sol_default（gpio204）。真机 interrupts.txt：`msmgpio 205 Edge` = wlan_dev_sol_irq（设备 SOL=signal of life）。

**配套节点**：`qcom,qrtr-mhi-cnss`（dev-id **0x1111**, net-id 1，QMI WLFW 服务走 MHI 上的 QRTR）；`qcom,cnss-direct-link`（SID 0x100f）；`qcom,cnss-vendor-wlan-wonder`（status okay）；`spi_cnss`（qcom,spi-cnss，spi-max-freq 0x1e84800=2MHz）；热区 `wifi_therm`（pmh0101_wifi_therm ADC）。

---

## 5. 固件与板级配置（真机证据）

**dmesg 固件加载序列**（目录 `/vendor/firmware_mnt/image/peach/`）：

| 文件 | 大小 | 作用 |
|---|---|---|
| peach/amss20.bin | 无据（dmesg 报 `failed with error -2` 后走 sysfs fallback；实际由 mhi_110e 设备加载） | WLAN 主固件（amss 家族） |
| peach/bd_p2.elf | 125288 B | BDF 板数据文件（Host BDF config: HW_XPA: 1 CalDB: 0 Radio OFF: 0） |
| peach/regdb_xiaomi.bin | 24278 B | 小米 regulatory db |
| peach/qdss_trace_config_v2.cfg（及 _perf_v2） | — | 调试 trace 配置 |
| wlan/qca_cld/peach_v2/WCNSS_qcom_cfg.ini | — | 板级 ini（经 `/devices/virtual/firmware/` 加载） |
| wlan/qca_cld/peach_v2/wlan_mac.bin | — | MAC（DMS 下发 `<WIFI_MAC>`；路径含 PCI BDF） |

**vendor 分区提取（09-vendor-config）**：
- `vendor-firmware.tar`：`./wlan/qca_cld/{kiwi_v2,peach_v2,wcn7750}/WCNSS_qcom_cfg.ini`（symlink → `/vendor/etc/wifi/<board>/`）、`wlan_mac.bin`（→ `/mnt/vendor/persist/wlan/wlan_mac.bin`）、`wlanmdsp.otaupdate`。
- `vendor-etc.tar`：`./wifi/peach_v2/WCNSS_qcom_cfg.ini` 关键配置：**`mlo_support_link_num=3`**、`gNumVdevs=5`、`gSoftApMaxPeers=10`、`gChannelBondingMode5GHz=1`、6G/320MHz 的 CNN tflite 模型（CNN_6G_320MHz_EVM/MM.tflite 等 28 个）、`models/qcom_game_latency_predict*.dlc`、`qcom_qoe_predict.dlc` → WiFi 7 (802.11be) 3-link MLO 板级配置，**证实 qca_cld3_peach_v2 板级名**。
- `vendor-modules-listing`：仅 `/vendor/lib/modules → /vendor_dlkm/lib/modules` symlink，**驱动模块本体未提取，模块文件名无据**。

**dmesg 驱动日志前缀**：`wlan_pld:pld_is_ipa_offload_disabled` → 驱动家族为 **qca_cld（cnss2 + PLD 层）**，不是 mainline ath12k（mainline 日志前缀为 `ath12k_pci`）。日志中未出现 `ath12k`/`ath11k` 字样。

---

## 6. 驱动判定：qca_cld(peach_v2) vs ath11k vs ath12k

| 维度 | ath11k | ath12k | 真机（qca_cld peach_v2） |
|---|---|---|---|
| 代表 PCI id | 0x1103 (WCN6855) | 0x1107 (WCN7850) / 0x1109 / 0x1112 | **0x110e（不在 mainline 任何表内）** |
| 总线 | PCIe + MHI | PCIe + MHI | PCIe0 Gen3 + MHI（`mhi_110e_00.01.00`）✓ |
| 固件 | amss.bin | amss.bin | **amss20.bin**（文件名后缀 20 与 0x110e 家族对应，具体型号无据） |
| QMI | WLFW | WLFW | QMI WLFW service connected（L345），经 qrtr-mhi-cnss dev-id 0x1111 ✓ |
| 802.11 能力 | ax | be (WiFi7) | be：MLO 3-link + 6G 320MHz 配置 ✓ |

结论：**架构与 ath12k 同族（PCIe+MHI+QMI+amss 固件），真机运行 vendor qca_cld 技术包**。向 mainline ath12k 对齐的工作量 = 1 行 PCI id（0x110e）+ peach 板文件（BD 数据用真机 bd_p2.elf 的 board-id）+ amss20.bin 加载名。

---

## 7. XNU 侧参考：redwood_wifi.c（hackios xnu-8796.121.2）

- redwood = POCO X5 Pro（SC7280），**WCN6750 WPSS 方案**：SMEM/SMP2P/IPCC/AOP mailbox → TZ 认证 wpss.mdt+b02..b07 → GLINK/QRTR → ath11k。顶部 200 行注释完整记录 `wifi_step=1..16` 引导调查序列与 `wifi=1` 全流程（无中断轮询 ring 起步，`wifi_irq=1` 用 MSI 经 SMMU context 到 GIC）。
- `bsd/conf/files`：redwood_wifi.c:134、redwood_apple80211.c:124、redwood_wlfw.c:143、redwood_lx.c:144、redwood_sta.c:145（均 standard）；头文件 `sys/redwood_qrtr.h`、`redwood_lx.h`、`redwood_sta.h`、`redwood_net.h`。
- `redwood_lx.c` 架构（**移植复用的关键**）：ath11k 在树外原样编译成一个对象（`tools/mk-ath11k.sh`, `wifi/lx`），导出 `lx_start()`；内核经 `sys/redwood_lx.h` 提供 `lx_os_alloc` / `lx_os_free` / `lx_os_radio_memory`（kmem_alloc_contig 连续物理 + dc civac 刷缓存）等 shim；无对象时 fallback 只报错。
- `redwood_sta.h` 是纯 C 边界：bss/scan/join/state 结构，扫描/加入在 station 侧，帧经 `redwood_net.h`，iOS 请求由 `redwood_apple80211.c` 应答。

**popsicle 与 redwood 的差异**：引导链完全不同（WPSS 固件认证链 → 换成 PCIe 枚举 + MHI + 平台供电时序），但**驱动对象接入 XNU 的 shim 层（lx_os_* + apple80211 对接）可以直接复用**。

---

## 8. 移植骨架路线（评估）

```
XNU 启动
 ├─ 1. RPMh 供电上电（8 路：L2/L3/S1J/S2J/S8F/S7F/L3K + wcal-pbs，按 pmu_vreg_map 电压）
 ├─ 2. TLMM：wlan-en(16) 拉高、sw-ctrl(18/19)、SOL(204/205)；PMIC GPIO5 bt-en
 ├─ 3. GCC 复位：GCC_PCIE_0_PHY_BCR(9) → GCC_PCIE_0_BCR(6) 顺序释放
 ├─ 4. pcie@1c00000 枚举（parf/phy/dm_core/elbi/iatu/pcie_sm 寄存器，Gen3，PERST=TLMM102）
 ├─ 5. MSI 32 事件 + SMMU SID 0x1400 域映射（cnss_pci0 iommu-addresses 两分区）
 ├─ 6. MHI 枚举端点 0x110e（加载 peach/amss20.bin；BDF bd_p2.elf；regdb_xiaomi.bin）
 ├─ 7. QMI WLFW 服务（qrtr-mhi-cnss dev-id 0x1111）→ 校准（xo_calib sdam@7100）
 └─ 8. qca_cld/ath12k 核心对象（照 redwood_lx.c shim 思路编译进 XNU，lx_start()）
       └─ redwood_sta.h/redwood_net.h/redwood_apple80211.c 复用为 iOS 80211 对接层
```

可行度评估：步骤 1–7 是「平台胶水重写」（有 DTB 全参数 + dmesg 时序作参照，工作量大但无原理性障碍）；步骤 8 的驱动核心可自真机 qca_cld 技术包或 mainline ath12k（+1 行 id）二选一，XNU 侧复用 redwood 的 lx shim。

---

## 9. 阻塞点 / 风险

| # | 阻塞点 | 说明 |
|---|---|---|
| 1 | **0x110e 无 mainline 支持** | ath12k wifi7/pci.c 需新增 1 行；vendor qca_cld 源码本机无 |
| 2 | **cnss-peach 平台驱动源码无据** | xk-dt 仓库无 cnss-peach dtsi（canoe.dtsi 仅 qrtr-mhi-cnss + iommu group；alor.dtsi 的 wpss_pas 是 WCN7750 方案，非本机）→ 平台时序只能从 DTB + dmesg 反推 |
| 3 | **固件/BDF 依赖真机 vendor 分区** | amss20.bin、bd_p2.elf、regdb_xiaomi.bin 均需从真机提取（本机 tar 只有 ini/mac 文件） |
| 4 | **供电/复位时序重放** | 8 路 RPMh + 4 组 pmu_vreg_map + vreg_pdc_map（bb/rf 域）+ 4 GPIO + PMIC GPIO 的上下电顺序只能参照 dmesg 时间线 |
| 5 | **校准链** | xo_calib（pmk8850 sdam@7100）+ "Calibration took 8456 ms" + DMS MAC 下发需复刻 |
| 6 | WiFi7 特性面 | MLO 3-link/320MHz 的 tflite 模型与 vendor_cmd.xml 属 qca_cld 私有，mainline ath12k 不一定带同构实现（iOS 16.5 侧 apple80211 亦无 WiFi7 原生支持，可先降级 ax/be 2.4+5G 单链路运行） |

**无据清单**（不臆测）：0x110e 营销名；amss20.bin 大小/版本；驱动模块文件名（vendor-modules 只有 symlink）；cnss-peach 的 dtsi 源码与平台驱动源码；BDF board-id 具体值。
