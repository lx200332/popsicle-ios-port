# STATUS · 2026-11-11 蓝牙 SE5 打通 TX、WiFi 找到断电域、驱动全量审计

> 这份接 `STATUS-20261110-首次进入桌面与三个故障.md`。11-10 那份的结论是
> 「桌面能进、能停住，挡住 AMFI 三层故障」，把 Framebuffer 颜色也修好了。
> **11-11 这一轮把三件长期没动静的事推到了明确的边界上**：蓝牙的 GENI 序列器
> 第一次真的发出 HCI 命令、WiFi 的「PARF 全 0」定位到 GCC 的 GDSC 断电、
> 26 个自研驱动做完一次全量审计（六路并行）。
>
> 所有数字来自当次真机内核日志，不是估计。

---

## 0. 一句话现状

**三件事各有斩获，但都没有到「能用」：蓝牙发得出去、收不回来（芯片没上电）；
WiFi 总线活了、链路没训练（缺三级时钟与复位）；驱动审计确认地址层零缺陷、
链接层曾经漏编 14 个驱动。**

| 线 | 昨天 | 今天 | 卡在哪 |
|---|---|---|---|
| 蓝牙 | 读到固件四文件，SE 停 I2C | **UART 8N1 全编程、`hci op 0x1001` 已发出** | `BT_EN` = PMH0104 GPIO5（SPMI）从未拉高 |
| WiFi | `PARF` 32 字全 0、读 ELBI 就 abort | **两个 PCIe GDSC 上电、PARF 23 字非零** | 缺 resets / clocks / 199 步 PHY / PERST |
| 基带 | 5 条 leg 全 MISSING（`image/image/`） | **5 条 leg 全读到**（`checklist -> 0`） | `pas 38 init_image -> 0xffcfffbb (-3145797)` |
| 驱动审计 | — | **45/45 地址命中 DTS；26 驱动链接全到位** | 4 类依赖树里不存在 |

---

## 1. 蓝牙：SE5 的五个结构性错误（已修，发得出去）

### 1.1 症状

`hci op 0x1001 -> 0 bytes`、`hci op 0x1009 -> 0 bytes`。TX 看似"跑了"但芯片
一个字节都没回。

### 1.2 五个错误（每一条单独都足以让 HCI 收不到一个字节）

对照 Linux 的 `drivers/tty/serial/qcom_geni_serial.c` 与 `include/linux/soc/qcom/geni-se.h`：

| # | 我们写的 | Linux 要求 |
|---|---|---|
| 1 | `SE_GENI_M_CMD0 = 0x1`（opcode 0、params 1） | `1 << M_OPCODE_SHFT(27)` = `UART_START_TX` |
| 2 | 从不写 `SE_UART_TX_TRANS_LEN` | `0x270`，每次传输前写字节数 |
| 3 | 除频写进 `SE_GENI_CLK_SEL` | 那是 3 位表索引；除频在 `SER_M/S_CLK_CFG` bits 15:4，`SER_CLK_EN` 在 bit 0 |
| 4 | 从不写 `SE_GENI_S_CMD0` | `UART_START_READ`（`1 << S_OPCODE_SHFT`），否则 RX FIFO 永不填 |
| 5 | 帧格式完全没编程 | `TX_TRANS_CFG 0x25c` / `TX_WORD_LEN 0x268` / `STOP 0x26c` / `RX_TRANS_CFG 0x280` / `RX_WORD_LEN 0x28c` / `RX_STALE_CNT 0x294` |

### 1.3 三个定义/判据错

| 项 | 原来 | 实际 |
|---|---|---|
| `SE_GENI_DEFAULT_PROTOCOL` | `0x040`（与 `SE_GENI_STATUS` 撞车） | **`0x044`**（`STATUS 0x40 / DEFAULT_PROTOCOL 0x44`） |
| `SE_GENI_TX_FIFO_STATUS` 空位判据 | `& 1u` | 空 FIFO 整字读 **0**，bit0 不是 has-room；用 `TX_FIFO_WC = GENMASK(27,0)` 计数 |
| `BT_CLK_HZ` | `19200000`（那是 SE0/I2C） | **`64000000`**，QUP ≥ 2.5 减半成 16x ⇒ 除频 `64000000/(16*115200) = 35` |

### 1.4 三处按真机实测改的

- **SE stride `0x13c`**（boot X3 dump 反推：`SE0 0xa8004 … SE5 0xa8630`，`0x62c/5`）。
  旧的 `0x40` 算出来是 SE1 的寄存器，`0x140` 让 `bt_on()` 在错误的字上误判通过。
- **`GCC_VOTE 0x52018` 必须饱和写 `0xffffffff`**。`0x1f00` 只启动 SE0，`0x3f00`
  仍让 SE5 停着，只有多出来的 wrapper `core`/`core_2x` 位才让 SE5 的 CBCR
  从 `0x88004000` 变成 `0x8004000`。
- **`CFG_RCGR` 的 MND 位（bit 20）不能清**。`clk_print_regs` 在真机读到
  `0x00100001`，之前把它清掉是错的。

### 1.5 结果（boot XF）

```
[0.380] SE protocol block 0x40=0 0x44=0 0x48=0x11 0x4c=0
[0.380] SE firmware 0x206 major 2, default protocol 0 (0); switching to UART
[0.380] SE now on UART (0x3)
[0.380] uart 8N1, m_clk_cfg 0x231 (div 35), s_cmd0 0x8000000, tx_wm 0x2 rx_wm 0x2
[0.380] frame tx_cfg 0x2 word_len 0x8 stop 0 rx_cfg 0 rx_word_len 0x8 stale 0xa0 geni_cfg 0
[0.380] m_irq 0x40000000 tx_status 0 rx_status 0 status 0x41000
[0.421] hci op 0x1001 -> 0 bytes:
[0.461] hci op 0x1009 -> 0 bytes:
[0.461] done
```

`m_irq 0x40000000` = `M_TX_FIFO_WATERMARK_EN` ⇒ TX 引擎真在跑；`status 0x41000`
= bit12 `S_GENI_CMD_ACTIVE` ⇒ 二级序列器已启动。**五处寄存器错误全部修好。**

### 1.6 下一个卡点：BT_EN 从来没人拉过

```
[0.329] PANDORA BT: BT_EN is pmh0104@9 GPIO 5 (SPMI, not driven here); sw_ctrl low
```

真机 DT 反解失败（`/proc/device-tree` 只含已 probe 的节点，`0x76a` 找不到），
改从仓库 `dts/popsicle_overlay.dts:1218-1266` 与 `:23872-23924` 命中：

```
pmh0104@9/pinctrl@8800  phandle 0xa4, compatible "qcom,pmh0104-gpio"
  bt_uwb_en { pins="gpio5"; output-enable; bias-disable; }   ← 这就是 BT_EN
bt_peach
  qcom,bt-reset-gpio   = <0xa4 0x5 0x0>
  qcom,bt-sw-ctrl-gpio = <0xffffffff 0x12 0x0>   (TLMM pin 18，已驱动)
```

七路电源轨全部是 RPMh 稳压器，真名在 `__fixups__` 段：

| RPMh 资源 | 供 |
|---|---|
| `S2J` | `bt-vdd-dig` / `bt-vdd-aon` / `bt-vdd-rfa0p75`（一个供三路） |
| `S7F` | `bt-vdd-rfa1p25` |
| `S8F` | `bt-vdd-rfa1p8` |
| `L2G` | `bt-vdd18-aon` |
| `L3G` | `bt-vdd12-io` |
| `S1J` | `bt-vdd-wlan-aon` |
| `L6K` | `bt-ant-ldo` |

**我们的内核有两处够不着**：

- `sweet_spmi.h` 只有 `sweet_spmi_init()` 与 `sweet_spmi_read()`，
  `sweet_spmi.c` 头注释第一句就是 `PMIC READ ONLY` —— **没有任何写 PMIC 的路径**。
- `sweet_rpmh.c:331` 把稳压器白名单硬编码成 `strcmp(res, "ldoc11")` 且
  `mv` 必须 2950..3400（唯一调用者是 `sweet_touch.c:578` 的触摸 VLDO）。

---

## 2. WiFi：断电的 GDSC 域（已修，总线活了、链路没通）

### 2.1 起点

```
[0.169] popsicle_wifi: step2c parf[0x414] = 0x00000000 -> phy not ready (bit 7)
[0.169] popsicle_wifi: step2c pcie_sm[0x1090] = 0x00000000
[0.169] popsicle_wifi: step2c link is not trained
```

### 2.2 定位过程（三次都以 abort 换信息）

1. 盲扫 GCC `0x3b000..0x3c000` → 第一个字就 `esr 0x96000010`、`far 0xfffffff8d3d04000`，
   崩溃点在 `_wifi_thread + 0x6d8`（`sym.py` 二分符号化）。**GCC 窗口不是 uniformly 可读，不能扫。**
2. 改逐候选读，确认 abort 点就在 `0x3b000`。
3. 按 SM8550 `gcc-sm8550.c` 的 GDSC 偏移表，把 `GCC_SIZE` 从 `0x40000` 扩到
   **`0x80000`**，读七个候选 → 全部读通。

### 2.3 根因：PCIe 两个 GDSC 域一直断电

| 寄存器 | 值 | 含义 |
|---|---|---|
| `gcc[0x39004]` `usb30_prim GDSCR` | `0xfa22f800` | bit0=0，**已上电**（对照） |
| `gcc[0x6b004]` `pcie_0 GDSCR` | `0x0222f801` | bit0=1，**SW_COLLAPSE 断电** |
| `gcc[0x6b008]` `pcie_0 CFG_GDSCR` | `0x04088000` | bit15=1，`POWER_DOWN_COMPLETE` |
| `gcc[0x6c000]` `pcie_0_phy GDSCR` | `0x02222801` | bit0=1，**断电** |

这一条同时解释三件事：PARF 读全 0 却不 abort（总线能答、域内无电）、
ELBI 读 abort（未上电窗口）、USB 一直正常（同一 GCC 上它上电）。

### 2.4 修法

清 `SW_COLLAPSE`（bit0）后轮询 `CFG_GDSCR` bit16（`POWER_UP_COMPLETE`），
10 µs 一步。门控 `wifi_gdsc=1`（默认关，因为这一步写 GCC）。

### 2.5 结果（boot G4 / G6）

```
gdsc pcie_0     (0x6b004) = 0x0222f801, clearing SW_COLLAPSE
gdsc pcie_0     after 10 us: 0xfa22f800, cfg = 0x04070000 -> up
gdsc pcie_0_phy (0x6c000) = 0x02222801, clearing SW_COLLAPSE
gdsc pcie_0_phy after 10 us: 0xfa222800, cfg = 0x04070000 -> up
```

**PARF 从 32 字全 0 → 23 字非零**：

| 偏移 | 值 | 含义 |
|---|---|---|
| `parf[0x1c]` | `0x00101010` | LTSSM 状态 |
| `parf[0x24]` | `0x90040400` | 版本 + Device ID |
| `parf[0x54]` | `0x33aa33aa` | AUX 时钟配置 |
| `parf[0x18]` | `0x00010000` | 链路状态位 |

### 2.6 下一个卡点：上电不等于能读 ELBI

`parf[0x414]` PHY ready 仍 0、`pcie_sm[0x1090]` 仍 0、读 ELBI 仍复位
（boot G5 甚至一字节未写就复位）。剩下的三级，按 DTS 顺序：

1. **resets**：`reset-names = "pcie_0_core_reset", "pcie_0_phy_reset"`（`<&gcc 6>` / `<&gcc 9>`）
2. **clocks**：pipe / aux / cfg_ahb / slv / mstr_axi，`qcom,aux-clk-freq`
3. **199 步 phy-sequence**（`qcom,phy-sequence` 597 words = 199 三元组，
   delay 全 0，max offset `0x19d0`，已生成 `wifi_phy_seq.h` 6,877 B）
4. **PERST**（`perst-gpio = <0x30 0x66>` = TLMM GPIO 102）

Android 的黄金路径（`dmesg`）：`assert PERST` → 配 PARF
（`PCIE20_PARF_INT_ALL_MASK: 0x7f80d202` / `PARF_INT_ALL_2_TYPE: 0x410`）→
**`pcie_phy_init: PCIe RC0 PHY is ready!`** → `Release the reset of endpoint` →
`de-assert PERST` → `msm_pcie_link_train: PCIe RC0 link initialized` → MHI 加载固件。
端点 `0000:01:00.0` = `17cb:110e`。

> 顺带纠正一条旧判断：`fuxi_wlan.c`（105 KB）**就是 PCIe 主机控制器驱动**，
> 只是需要 `fuxiwlan=1` 才启动。此前认为「缺 PCIe 主机控制器、要新写」是错的。
> 但审计确认它的 13 个窗口全是 **SM8550 地址**，且全树 `IOPCI*` 符号为 0、
> 没有 `registerService` —— 它是一段未接线的旧代码。

---

## 3. 基带：固件读到了，PAS 认证过不去

### 3.1 一条 bug 打掉两条线（已修）

`pandora_adsp.c` 的 `fw_open()` 在 `part != 0` 时自己拼 `"image/%s"`，而
`pandora_fw_volume_size()/_read()` 的约定已含 `image/` ⇒ 实际查
`image/image/modem.mdt` = ENOENT。

| | 修前（X0） | 修后（X1） |
|---|---|---|
| BT 四文件 | 全 missing | `gngbtfw20.tlv 320728` / `.mbn 333868` / `.bin 13832` / `.bin 744` 逐字相符 |
| modem 5 leg | 全 MISSING，`checklist -> 2` | `checklist -> 0 (5 legs)` |

修法：`fw_volume_name()` 剥前缀（`p_fwpref.py`）。

### 3.2 新卡点

```
PANDORA MODEM: pas 38 init_image (4044 bytes) -> a0 0xffcfffbb (-3145797)
```

对照：ADSP 的 `pas 13` 全 0 成功。`modem_dtb` 的 ELF 离线校验完全合规
（`image/modem.mdt: ELF32, 28 phdrs`），所以不是镜像本身的问题。
`0xffcfffbb` 落在 SCM 的负数返回区间，指向 `PAS_AUTH_AND_RESET` 这条带认证的路径。

---

## 4. 驱动全量审计（六路并行，只读）

对象：26 个自研驱动。方法：静态源码 + 反汇编内核 + 真机 DTS + klog 实测 +
本地 OTA 解包。总报告在 `_audit/驱动审计-总报告-20261111.md`（另有
`gpu-` / `touch-` / `fuxi_wlan-` / `zero-` / `需要手机什么-` 五份分报告）。

### 4.1 链接层：零缺陷（但历史上曾经很糟）

| 检查 | 结果 |
|---|---|
| `conf/files` 注册 | 35 个驱动源 + `qcom/FatVolume.c` + 40 个 `sweet_hfs/` 全部在册 |
| repo vs buildtree 漂移 | 26 个驱动 `.c` 逐对 md5 **全部 SAME** |
| 内核符号 | 31 个入口全在，`llvm-nm -u` = **0**，`_sweet_ufs_init` 有 **68 条 `bl`** |

**历史教训**：`conf/files` 里列了 ≠ CI 会编它。11-03 那批 CI 对象里
`pandora_bt/modem/thermal/sensors/idle/mem` + 全部 `popsicle_*` **ABSENT**，
`qcom/` 子目录整个不存在；`sweet_ufs.o` 还是旧版，14 个 init 里只有 9 个被链接。
这是「BT/modem/WiFi/audio/camera/fp 六个从来没跑过」的真正原因。

### 4.2 地址层：45/45 命中真机 DTS

解析 `popsicle_merged.dtb`（1,723,024 B → 925 节点 / 1,124 个 reg 区间，
按 `#address-cells`/`#size-cells` 自上而下继承），45 个 MMIO 基址全部落在窗口内。
此前怀疑的几处全部排除：

- **`TLMM 0x0f100000`** = `pinctrl@f000000`（`reg=<0xf000000 0x202000>`）+ `0x100000`。
  这是 downstream `pinctrl-canoe.ko` 的约定，**代码正确**，用 `0x0f0xxxxx` 会挂总线。
- `AD_RSC_PA 0x18920000` 是 `rsc@18900000` 的第三段 reg。
- `GMU_PA 0x3d37000` 是 `qcom,kgsl-3d0@3d00000` 的 `+0x37000`。
- `MD_MEM_PA 0x8aa00000` 与 `modem.mdt` 的 `ph1 paddr` 逐条相符。

### 4.3 运行层四档

| 档 | 数量 | 代表 |
|---|---|---|
| 实测跑通（有 klog 实证） | 11 | `pandora_gpu`（15 级全 done）、`pandora_dpu`（`598 frames scanned out`）、`pandora_touch`（1,924 条事件配对）、`pandora_cpu`（8 核 PSCI 全 rc=0） |
| 刚写完、没上机 | 3 | `pandora_thermal` / `pandora_idle` / `pandora_mem` |
| 只有门禁或只读巡检 | 12 | `popsicle_audio/camera/fp/sensors/amp` 等 |
| **依赖在树里根本不存在** | 4 类 | I2C/SPI 控制器（`IOPCI*`、0 个 I2C 控制器）、SSC 传感器子系统、8 个 Cryptex 服务、PMIC 写路径 |

### 4.4 审计顺手纠正的三条旧结论

| 旧结论 | 实际 |
|---|---|
| 「PMGR 同一次 boot 打两次」 | **两个 boot 的日志混读**。`restart type 4` 复位后复用同一 4 MiB 环。klogG/X/Y `fail30` = 0 |
| 「3 个 init 是 DEAD 入口」 | 假阳性。C 符号带前导 `_`，`grep -w` 漏了；三个都有调用者（`SweetFramebuffer.cpp:1532` / `SweetBattery.cpp:385` / `startup.c:721`） |
| 「`pandora_mem.c` 没注册」 | 头注释写「NOT REGISTERED -- deliberately」，实际已被 `conf/files:150` 注册并调用 |

---

## 5. 本轮两次真实过错（写在这里，防止复发）

### 5.1 `cat` 一个伪文件把手机读崩

`/sys/kernel/debug/regmap/100000.clock-controller/registers` 在 `ls -l` 下是
**0 字节、`-r--------`**，实际是伪文件：`cat` 会遍历整个 GCC（`range 0-1f41f0`，
128K 个寄存器）。GCC 里有带读副作用的寄存器（vote / BCR / GDSC），读到即断电 +
不可恢复总线错误 → 整机复位。

**此后一切 GCC 读数只用 `/sys/kernel/debug/clk/clk_summary` 与
`/sys/kernel/debug/clk/<clk>/clk_print_regs`**（clk 框架自带的单时钟导出）。
`dump_gcc*.sh` / `gcc_regs*.sh` 全部作废。

### 5.2 `pandora_reboot_after=30` 造成重启循环

测试镜像不该带这个参数。`pl_thread` 满 30 s 就 `pl_psci_reset()`，
`fastboot boot` 下形成循环。

**规则：除专门测「到点重启」外，测试镜像一律用 `cmd_btscan_noreboot.txt`（581 B，无该参数）。**

---

## 6. 构建 / 取证链（本轮可复现的那几条）

```
编译单文件   ccbt.sh      借 pandora_adsp.c 的 CI 行 sed 成目标文件
                          XNU_CC_OBJDIR=/opt/work/bt2/obj \
                          python3 /opt/work/tools/xnu-cc.py <log> /opt/work/ci/buildtree
重链         rlx3.sh TAG  → /opt/work/out<TAG>/kernel.development.pandora
打包         buildX*.sh   ┐ mkprelink → patch_amfi(P1P2) → mkadt_pandora --gpu-pool 1024 --amp
                          │ → adt_merge_chosen(只补 mac-address-wifi0) → mkpayload
                          │ → addtrustcache(两个 TC) → loader → boo*.img
                          └ 由上一层脚本 sed 派生，必须同时 sed K= 里的 out 路径
清环         pinfile rawwrite /data/local/tmp/plog2.img /dev/block/sda34 all --yes < plog-zero.img
             期望 rawhash b69dae56a14d1a8314ed40664c4033ea0a550eea2673e04df42a66ac6b9faf2c
引导         boot3.sh img   只发不等待（busid 自动 awk，本次 busid 1-1）
等回来       waitback.sh    约 70 s
读日志       pinfile rawread ... > out.raw → pk.py 解析
```

**工具陷阱**（本轮踩过）：

- `xnu-cc.py` 的 `bt` 参数必须是 `.../buildtree`，**不是** `.../buildtree/xnu`
  （传错会 `FileNotFoundError: .../buildtree/xnu/build/xnu.obj/DEVELOPMENT_ARM64_PANDORA`）。
- 任何 WSL 脚本必须写成 `/mnt/c/...` 再 `wsl bash <path>`；
  `bash -c "... \$B/..."` 里的 `\$B` 会被 Windows bash 层先吃掉。
- Windows 侧 `python.exe` 不能读 `/mnt/d/...`，必须 `D:/ios/...`；
  `adb push` 同理。
- `adb devices` 有 `(no serial number)` 干扰项，所有命令必须 `-s 6f17201d`。

---

## 7. 今日提交

| 仓库 | 提交 | 内容 |
|---|---|---|
| `popsicle-port` | `3c7b9bc` | modem/bt/wifi 从真正存固件的分区读 |
| | `5d28c11` | modem：shutdown → dtb(auth) → 主镜像作为 extra |
| | `c2639a5` | bt：从 bluetooth 分区读 ganges 固件，认 FAT12 |
| | `cd19cb3` | wifi：给两个 PCIe GDSC 上电并探测 GCC |
| | `2328e86` | adsp：两个 volume helper 在找 `image/image/` |
| | **`58109ef`** | **bt：UART 里那五处寄存器错，逐条修掉** |
| `data-a-live-popsicle` | `e69e461` | xnu 0338：给 `pandora_part` 显式句柄，两个分区可同时读 |
| `_ios-sweet` | **`e26ac92`** | **rootfs：关掉 deep framebuffer，面板拿到 BGRA8888**（本地，push 403，补丁 `_work/ios-sweet-e26ac92.patch`） |

---

## 8. 下一步（按性价比排序）

1. **BT 上电**：放开 `sweet_rpmh_vreg_enable` 的白名单到 S2J/S7F/S8F/L2G/L3G/S1J/L6K，
   并给 `sweet_spmi.c` 加 `EXT_WRITEL` 写 `pmh0104@9` GPIO5。做完重测
   `hci op 0x1001`，期望 `0e 0c`。
2. **WiFi 三级**：resets → clocks → 199 步 PHY → PERST(GPIO102)。每一步都能用
   `parf[0x414]` bit7 与 `pcie_sm[0x1090]` 作判据。
3. **modem `pas 38`**：`0xffcfffbb` 先反查 SCM 返回码表，再对照 vendor 的
   `canoe_mpss_resource` 里 auth 那一段的参数。
4. **CI 门禁**：反汇编 `_sweet_ufs_init` 提 `bl` 列表，与 `conf/files` 交叉核对 ——
   防「列了但没编」再次发生（这次已经发生过一次，六个驱动白等了八天）。
5. 上游 `_ios-sweet` 的 403 需要把 `e26ac92` 走 PR 或者换远端。
