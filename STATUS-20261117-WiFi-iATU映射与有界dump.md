# STATUS · 2026-11-17 WiFi M1：iATU 出站映射、有界 dump、三次上机定位

> 这份接 `STATUS-20261113-蓝牙上电链路打通与驱动审计修复.md`。11-13 那份的收口结论是
> 「蓝牙电源与时钟链全部打通、SE5 UART 自环四字节全对，剩下 ROM 静默与 `M_TX_FIFO_RD_ERR`」。
> 用户随后要求**暂停蓝牙、转 WiFi**，于是 11-13 → 11-17 这一轮只做了一件事：
> **按 session-cf5ef41d 提取的真机数据，给 `popsicle_wifi.c` 补上 iATU 出站映射与有界 dump，
> 并把它第一次打上真机。**
>
> 所有数字来自当次真机内核日志（`klogYX` / `klogYY` / `klogYZ` / `klogZA`）与 Android 侧
> debugfs / `lspci` / `dmesg` 只读取证，不是估计。

---

## 0. 一句话现状

**WiFi 的 M1（让 BAR0 可读）已经写进驱动并在真机上跑过三次：iATU（`0x410000000`，> 4 GiB）
与 BAR0 两个高位窗口都映射成功、都有返回 VA；上电链（GDSC + GPIO）与三处 dump 现在按
「先上电、后 dump、且 dump 有上界」的顺序执行。三次上机全部在「域还没上电」的状态下
盲扫寄存器而死，死点每次往后挪一段（`parf[0x58]` → `pcie_sm[0x1048]`），
`boot-zb`（先上电后 dump + sm-debug 也设上界）已编译、已重链、已打包，尚未上机。**

| 项 | 11-13 | 11-17 | 卡在哪 |
|---|---|---|---|
| `popsicle_wifi.c` | 619 行（repo 与 buildtree 同一版） | **866 行 / 41,846 B / md5 `def8a27c…`** | — |
| iATU 出站映射 | 完全没有 | **`wifi_step5_iatu()` 按 Linux `dw_pcie_prog_outbound_atu()` 写序，region 0 = BAR0 2 MiB identity** | 未验证读回 |
| 上电 vs dump 顺序 | GDSC 上电排在 dump 之后 | **`wifi_poweron` → `wifi_gdsc` → step1/2/2b/2c → `wifi_gcc` → `wifi_iatu`** | — |
| PARF dump | 无限扫 32 字（死在 `0x58`） | **默认 `0x58` 字节（`0x00..0x54`），两个域都 up 才放宽到 32 字** | — |
| pcie_sm dump | 无限扫 14 字（死在 `0x1048`） | **默认 1 字，两个域都 up 才放宽到 14 字** | — |
| `wifi_gdsc_on()` | 「读回 0」当上电成功 | **显式拒绝 `cfgv == 0`** | — |
| `wifi_step0_power_on()` 延时 | `2000 µs` = 2 ms（注释说 2 s） | **`20000 µs` = 20 ms** | — |
| 是否上机 | — | **三次（yx / yy / yz / za），全部未进 step4** | 域未上电即盲扫 |

---

## 1. 这一轮做了什么（按时间顺序）

### 1.1 先把真机事实对齐（session-cf5ef41d 的结论）

那个会话已经把 WiFi 的硬件事实提取完（`C:\dsh-对话\popsicle-wifi\REPORT.md` 33,901 B 八节 +
`popsicle_wifi_names.h` 166 个宏 + 25 MB 固件）。对本轮直接有用的六条：

- 端点 `0000:01:00.0 = 17cb:110e`（`class 0x028000`、rev 01），根端口 `17cb:0123`。
- BAR0 `0x40400000-0x405fffff`（2 MiB，flags `0x140204`，与 `/proc/iomem` 的 `cnss` 保留区同址）、
  BAR2 `0x40300000-0x40307fff`（32 KiB，MSI-X 表在其 `+0x0`，PBA 在 `+0x1000`）。
- 链路实测 `current_link_speed 5.0 GT/s`、`width 1`（`max 8.0 GT/s / width 2`）、`D0`。
- `/soc/pcie@1c00000` 的 `reg` 六段：`0x01c00000+0x3000`（parf）、`0x01c06000+0x2000`（phy）、
  `0x400000000+0x10000000`（dm_core）、`0x4000f200000+0xa8`（elbi）、**`0x410000000+0x1000`（iatu）**、
  `0x01d07000+0x7000`（pcie_sm）。
- `qcom,phy-sequence` = 199 个 `(offset, mask, value)` 三元组。
- **`popsicle_wifi.c` 从来没有 iATU 出站映射** —— 这就是「BAR0 读不到」「ELBI 单独读会死机」
  的直接原因：没映射时访问 `0x40400000` 会落到 RC 自己的地址空间。

### 1.2 iATU 的权威写法（Linux 主线，逐字）

下载并逐字读了 `drivers/pci/controller/dwc/pcie-designware.h` / `.c` / `-host.c`：

```c
/* pcie-designware.h:202-228 */
#define IATU_VIEWPORT              0x900
#define ATU_REGION_DIR_IB          BIT(31)
#define ATU_REGION_DIR_OB          0
#define ATU_VIEWPORT_BASE          0x904
#define ATU_UNROLL_BASE(dir, index) (((index) << 9) | ((dir == ATU_REGION_DIR_IB) ? BIT(8) : 0))
#define ATU_REGION_CTRL1           0x000
#define ATU_REGION_CTRL2           0x004     /* ATU_ENABLE BIT(31) */
#define ATU_LOWER_BASE             0x008
#define ATU_UPPER_BASE             0x00C
#define ATU_LIMIT                  0x010
#define ATU_LOWER_TARGET           0x014
#define ATU_UPPER_TARGET           0x018
#define ATU_UPPER_LIMIT            0x020
```

`dw_pcie_prog_outbound_atu()`（`pcie-designware.c:533`）的写序是
**LOWER_BASE → UPPER_BASE → LIMIT → [≥460A] UPPER_LIMIT → LOWER_TARGET → UPPER_TARGET →
CTRL1 → CTRL2（最后写，含 `ATU_ENABLE`）**。

两条可用的寻址路径：
- **unrolled**（v4.80a+）：`atu_base + ((index) << 9)`，判据是 `IATU_VIEWPORT` 读回 `0xFFFFFFFF`。
- **viewport**（老式）：`IATU_VIEWPORT = dir | index`，之后在 `atu_base + 0..0x2C` 上操作。

本机的 iATU 窗口是 `0x410000000 + 0x1000` = **8 个区 × 0x200**，正好是 unrolled 布局；
但 Android vendor 驱动里写的是 `CTRL_2 = 0x80000000 | (index << 16)`（Qcom 私有 viewport 编码），
**与 Linux 主线 `dir | index` 不是一套，不能混用** —— 驱动里两套都留了宏，判定放在运行时。

### 1.3 驱动的三处不改不行的地方

| 问题 | 原状 | 现在 |
|---|---|---|
| PARF dump 无限扫 | 连读 `parf 0x00..0x7c` 32 字 | 默认 `0x58` 字节；`step4` 报两个域都 up 才放宽到 32 字 |
| `pcie_sm` debug dump 无限扫 | 连读 `0x1040` 起 14 字 | 默认 1 字；同上才放宽到 14 字 |
| `wifi_gdsc_on()` 判成功 | `(cfgv & POWER_UP_COMPLETE) != 0 \|\| cfgv == 0` | 显式拒绝 `cfgv == 0`，打印 `treating it as NOT up (unreadable or still collapsed)` |

另外两处按 Android 实测校正：`wifi_step0_power_on()` 的等待从 `2000 µs`（2 ms，注释却写 2 s）
改成 `20000 µs`；`wifi_thread` 的调用顺序改成**先上电后 dump**。

---

## 2. `popsicle_wifi.c` 现在的样子（866 行）

### 2.1 新增 boot-arg（旧版 5 个 → 新版 8 个）

`wifi_probe`（总开关）、`wifi_poweron`、`wifi_elbi`、`wifi_gcc`、`wifi_gdsc`（旧有）
+ **`wifi_iatu`**、**`wifi_bar0read`**、**`wifi_delayms`**。

`wifi_delayms` 是为一个具体事故加的：第一次带 `wifi_iatu=1` 引导（boot-yx）产出了
**空日志环**（rawhash 仍是全零的 `b69dae56…`、klog 0 行）—— 线程在 `0.2 s` 就摸到
`0x410000000`，早于 `pl_thread` 每 1 s 的 console flush，死得连一行都没落盘。
加 2 s 延时后（boot-yy）日志环活了下来。

### 2.2 新增定义（逐字）

```c
#define PCIE_PARF_SAFE_BYTES    0x58u        /* 0x00..0x54, the measured-readback prefix */
#define PCIE_IATU_PHYS          0x410000000ULL
#define PCIE_IATU_SIZE          0x00001000ULL
#define PCIE_BAR0_PHYS          0x40400000ULL /* BAR0 2 MiB, = reserved-memory cnss */
#define PCIE_BAR0_SIZE          0x00200000ULL
#define ATU_ENABLE              0x80000000u
#define ATU_TD                  0x00000100u    /* ECRC: set on cores before 5.10A */
#define ATU_UNROLL(index, ib)   ((uint32_t)((index) << 9) | ((ib) ? 0x100u : 0u))

static bool     wifi_bar0read;                 /* the ONE read that aborts when the link is down */
static unsigned wifi_parf_bytes = PCIE_PARF_SAFE_BYTES;
static unsigned wifi_sm_words   = 1u;
static unsigned wifi_delayms;                  /* sleep before the first MMIO touch */
```

### 2.3 新增函数

`iatu_rd(uint32_t off)`、`iatu_wr(uint32_t off, uint32_t value)`、
`wifi_step5_iatu(void)`（region 0 = BAR0 2 MiB identity，严格按 Linux 八步写序；
`wifi_bar0read=1` 才做那一次「读穿窗口」的动作 —— **编程窗口是安全的，读穿它才是会中止的那个**）。

### 2.4 `wifi_thread` 的新顺序（这就是本轮全部争议的答案）

```
[wifi_delayms]  先睡 N 毫秒，让日志环先落盘
[映射]          TLMM 0xf100000+0xce000 / PARF / PHY / ELBI / SM / GCC
                + iATU 0x410000000+0x1000（> 4 GiB）+ BAR0 0x40400000+0x1000
                每次 ml_io_map_unmappable 前后都打印，任一返回 0 就 thread_terminate
wifi_poweron ?  wifi_step0_power_on()        ← GPIO 16 / 18 上电
wifi_gdsc   ?  wifi_step4_power()            ← ★ 上电，已提到所有 dump 之前
                wifi_step1_pins()
                wifi_step2_pcie()
                wifi_step2b_parf_dump()      ← 有上界
                wifi_step2c_link()           ← sm debug 有上界
wifi_gcc    ?  wifi_step3_gcc_probe()
wifi_iatu   ?  wifi_step5_iatu()
wifi_elbi   ?  wifi_step2d_elbi()
                survey done → thread_terminate
```

---

## 3. 三次上机的读数（全部原文）

### 3.1 boot-yx —— 空环

`D:/ios/_work/boot-yx.img` `e672f829f300bc995d4d0145bda9a2fe24274049d07dab1843c932107ce70ffb`，
cmdline `wifi_poweron=1 wifi_gcc=1 wifi_gdsc=1 wifi_iatu=1`（无延时）。
`adb back at try 7`、boot_id `2dbe8844-…`、uptime 10.38；**`klogYX.txt` 0 行**，
环 rawhash 仍 `b69dae56…`（内核在第一次 1 s flush 之前就死了）。

### 3.2 boot-yy —— 写 GPIO 之后静默

`boot-yy.img` `d0d87009ad1d47851ecf0a713d01fe6bce64645d3d64de675a94e5ba41ef4b56`，+`wifi_delayms=2000`。
`klogYY.txt` **561 行**，17 行 `popsicle_wifi` 全部按预期打印：

```
[0.169] popsicle_wifi: FastConnect 7900 survey, boot-arg wifi_probe=1 (wifi_poweron=1: step 0 will WRITE gpio16/gpio18)
[2.170] popsicle_wifi: wifi_delayms done, starting the survey
[2.170] popsicle_wifi: mapping TLMM 0xf100000 + 0xce000        → TLMM -> 0xfffffff8b02a1000
[2.170] popsicle_wifi: mapping iATU 0x410000000 + 0x1000 (above 4 GiB) → 0xfffffff8afe35000
[2.170] popsicle_wifi: mapping BAR0 0x40400000 + 0x1000        → 0xfffffff8afe36000
[2.170] popsicle_wifi: power-on wlan-en (gpio16) before cfg=0x00000001 io=0x00000000
[2.170] popsicle_wifi: power-on wlan-en (gpio16) writing io=0x2 then cfg=0x00000200
[2.170] popsicle_wifi: power-on wlan-en (gpio16) after cfg=0x00000200 io=0x00000003 OK
[2.170] popsicle_wifi: power-on sw-ctrl (gpio18) before cfg=0x00000001 io=0x00000000
[2.170] popsicle_wifi: power-on sw-ctrl (gpio18) after cfg=0x00000200 io=0x00000003 OK
（到此为止：无 step1、无 abort、无 esr —— 又一次静默复位）
```

**这一轮最重要的新事实：iATU 与 BAR0 这两个高位映射本身是成功的（都有返回 VA）**。
`grep abort|esr|panic` 零命中 ⇒ 不是可报告的 data abort。
死点在「把 wlan-en 与 sw-ctrl 驱高之后、`step1_pins()` 第一次读之前」。

### 3.3 boot-yz —— 死在 `parf[0x58]`

`boot-yz.img` `bcf7d39e6dacae9e16191a13ffa000a0360e69b847eaaec4a935f8472d195625`，去掉 `wifi_poweron=1`。
`klogYZ.txt` **586 行**，`parf[0x00]..parf[0x54]` 全 `0x00000000` 一个字一个字读出来，
**`parf[0x58]` 那一行没有值** ⇒ 卡在 32 字 dump 的第 23 字。
`grep 'step5|iatu|step4|step3|step2c|survey done'` 零命中 ⇒ 从未走到 step2c 之后。

### 3.4 boot-za（fix1 之后）—— 死在 `pcie_sm[0x1048]`

`boot-za.img` `d4722df64635aee4a3833039362fe9517181f9fdbe08fda8405544806dbc6436`，
cmdline `cmd_wifi_m1c.txt`（580 B，**无 `wifi_poweron`**，含 `wifi_gcc=1 wifi_gdsc=1 wifi_iatu=1 wifi_delayms=2000`）。
`klogZA.txt` **593 行**，`abort|esr|panic|restart type` 零命中。

**fix1 生效**：PARF dump 变成
`step2b PCIe PARF hex dump 0x00..0x54 (22 x u32, raw -- … the window is the measured-safe prefix
until step 4 reports the domains up)`，22 字全部读完 —— **boot-yz 死的那一步这次通过了**。

**随即死在新位置**：`step2c` 打印 `pcie_sm[0x1040] = 0x00000000` 之后，`pcie_sm[0x1048] =`
那一行没有值 ⇒ 卡在 `qcom,pcie-sm-debug` 十四字 dump 的第二字。

同 boot 其它读数：`wlan-en cfg (gpio16) = 0x1 / level = 0x0`、`sw-ctrl cfg (gpio18) = 0x1 / level = 0x0`、
`host-sol cfg (gpio204) = 0x801`、`dev-sol cfg (gpio205) = 0x801`、
`step2c parf[0x414] = 0`（phy not ready）、`pcie_sm[0x1090] = 0`、`pcie_sm[0x1000] = 0`。

**前提要说清**：这三次（yy / yz / za）**都没有跑到 `step4` 的 GDSC 上电**，
所以所有 dump 都是在「域还没上电」的状态下做的 —— 这正是连续两次暴毙的共同条件。

---

## 4. 由此得出的判断（这是本轮真正的产出）

**同一类错误出现了三次，形状一模一样**：

| 次数 | 位置 | 结果 |
|---|---|---|
| 1（蓝牙，11-12） | `gcc[0x3b000]` 盲扫 GCC | `esr 0x96000010`，整机复位 |
| 2（WiFi，boot-yz） | `parf[0x58]` | 静默复位 |
| 3（WiFi，boot-za） | `pcie_sm[0x1048]` | 静默复位 |

结论不是「再缩小一次上界」，而是：**对未上电的寄存器块做逐字盲扫，本身就是错的做法。**
所以 fix2 做的两件事是：

1. 给 `pcie_sm` 的十四字 dump 加与 PARF 同款的上界（默认 1 字，两个域都 up 才放宽）；
2. **把 `wifi_step4_power()` 提到 `step1/2/2b/2c` 之前** —— 先上电，再 dump。

`boot-zb`（内核 16,816,104 B / sha256 `8f9ad93825b95ba4b70bcb8c8d78f6364f8e8476be3ad26a33652d97875088e6`，
wifi 对象 40,496 B / md5 `1837da578fcf44cc5ac0c451be12ca6c`，undefined 0）已编译、已重链，
`buildZB.sh` 已就绪，**尚未执行打包与上机**。

---

## 5. 这一轮修掉的三个真 bug（都是原先就有的）

| # | 位置 | 现象 | 修法 |
|---|---|---|---|
| 1 | `wifi_gdsc_on()` | 把「读回 `0x00000000`」当上电成功（`\|\| cfgv == 0`），于是上电失败的域会被当成已 up，后续 dump 直接扫全窗 | 显式拒绝 `cfgv == 0` 并打印 NOT up |
| 2 | `wifi_step0_power_on()` | `delay_for_interval(2000, NSEC_PER_USEC)` 看着像 2 s，**第一参是微秒** ⇒ 实际 2 ms | 改 `20000`（20 ms），并在注释里写明单位 |
| 3 | `rlx??.sh` 的对象来源 | `popsicle_wifi.o` 在 CI 对象树里**根本不存在**（`ls …/bsd/DEVELOPMENT/ \| grep wifi` 零命中），全靠脚本里一行 `cp /opt/work/wifNEW/obj/…`；cp 失败或编到旧文件时，重链会静默用上一代对象 | `rlxZA.sh` 起加**对象来源守卫**：缺文件直接 `FATAL … exit 2`，存在则打印 `--- wifi object provenance ---` + `ls -l` + `md5sum` |

第 3 条与蓝牙那次 `ccbt5.sh` 硬编码源文件路径（改源码不生效、`compiled 1/1` 但 obj 不变）
是同一类错误，这次提前堵住了。

---

## 6. 与蓝牙那轮的经验对照（用户 11-13 直接问过）

| 蓝牙踩过的 | WiFi 里的对应位置 | 现状 |
|---|---|---|
| `gcc[0x3b000]` 盲扫未验证偏移 | `parf` 32 字 / `pcie_sm` 14 字 | **已设上界（fix1 + fix2）** |
| `cat …/regmap/…/registers` 读副作用 | `wifi_step3_gcc_probe()` 的 7 个候选偏移 | 未动，仍靠 `wifi_gcc` 门控 |
| 空日志环 = 死得比 1 s flush 早 | boot-yx 0 行 | **`wifi_delayms` 解决** |
| 驱动对象陈旧 / 编译到旧文件 | `popsicle_wifi.o` 不在 CI 树 | **对象来源守卫** |
| `size_t` 收 `int` 的负 errno | M2 起会遇到 | 未到，已记进 REPORT 的坑清单 |
| 就地丢弃 RX | 同上 | 未到 |
| 照抄另一颗 SoC 的偏移 | `0x6b004` / `0x6c000` 是从 SM8550 推定的 | **仍未在 Android 侧证实** |
| DTS `init-microvolt` ≠ Android 实跑值 | 蓝牙七路电压就是这么错的 | WiFi 的 8 条轨同样要按 `regulator_summary` 实测值填 |

---

## 7. 本轮文件清单

| 文件 | 说明 |
|---|---|
| `D:/ios/popsicle-port/popsicle_wifi.c` | **866 行 / 41,846 B / md5 `def8a27c8982bca84a38d71780b96c0f`**（= buildtree 版） |
| `data-a-live-popsicle/xnu/patches/0342-popsicle-wifi-iatu-and-bounded-dumps.patch` | 43,965 B / 878 行 / 7 hunks / +693 −34；**反向重放验证 md5 与 buildtree 一致** |
| `D:/ios/_work/boot-yx.img` | `e672f829…` / 空环 |
| `D:/ios/_work/boot-yy.img` | `d0d87009…` / 死在写 GPIO 之后 |
| `D:/ios/_work/boot-yz.img` | `bcf7d39e…` / 死在 `parf[0x58]` |
| `D:/ios/_work/boot-za.img` | `d4722df6…` / fix1，死在 `pcie_sm[0x1048]` |
| `/opt/work/outZB/kernel.development.pandora` | `8f9ad938…` / 16,816,104 B / undefined 0（fix2，**未打包上机**） |
| `C:\dsh-对话\popsicle-wifi\REPORT.md` | 真机数据提取报告（八节，含 M1–M6 里程碑） |
| `C:\dsh-对话\popsicle-wifi\popsicle_wifi_names.h` | 166 个宏（PCI/BAR/GPIO/电源/时钟 + BHI/BHIE/MHI + MSI-X 向量表） |

---

## 8. 下一步（接续时从这里开始）

1. **`buildZB.sh` → `boot-zb.img` → 上机**：验证「先上电后 dump」是否让 `step4` 报
   `both domains up`、`parf[0x414] bit7` 是否终于置位、放宽后的 32 字 dump 能否整段读完。
2. 若 `step4` 仍报「a domain did not report up」：`0x6b004` / `0x6c000` 这两个 GDSC 偏移
   **是从 SM8550 推定的**，必须在 Android 侧用 `clk_summary` / `regulator_summary` 证死或改值。
3. 域上电之后才是真正的 M1 里程碑：`wifi_iatu=1 wifi_bar0read=1` 读 `0x100/0x104`，
   期望 `0x17cb / 0x110e`。**在那之前不要开 `wifi_bar0read`**（读穿未训练的窗口会整机复位）。
4. M2（BHI/BHIE 两段式投递）见 `REPORT.md` 第五节 —— `sbl_size 0x80000` + 13 段 BHIE，
   `bhi_vec_entry` 是 16 字节不是 8，这是全篇最容易写错的一步。
