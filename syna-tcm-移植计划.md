# Synaptics TCM 主屏触控移植 · 文件级预研计划

> 生成时间：2026-10-24（触控预研子代理 f636c7c8）
> 定位：popsicle 主屏（1200×2608）Synaptics TCM 触控驱动移植到 hackios XNU（xnu-8796.121.2），
> 复用 redwood_goodix.c 无框架风格，协议参照 Linux 主线 Synaptics TCM 驱动（LineageOS sm8350 树）。
> 本文档只出计划，不含完整代码；所有硬件数字来自主控已实锤参数，所有 Linux/XNU 行号均为实测 grep 所得。

---

## §1 硬件参数总表（出处标注：主控已实锤）

### 1.1 触控芯片与总线（主控已实锤）

| 项目 | 值 | 备注 |
|---|---|---|
| 芯片 | Synaptics TCM | SPI 接口 |
| SE 节点 | `/soc/qcom,qupv3_4_geni_se@1ac0000/spi@1a80000` | qupv3_4 = wrap4 S0 |
| SE 寄存器基址 | `0x1a80000`，size `0x4000` | 主屏 touch SE |
| SPI max | 15 MHz（DTB 实测） | popsicle DTB synaptics,tcm-spi@0 |
| GIC SPI 中断 | `<0x9e 0x2008>` = **GIC 158** | 芯片级 IRQ |
| qts trusted-touch spi-irq | `0x287` = **GIC 647** | 安全虚拟机侧用，驱动本体用 158 |
| GPIO IRQ | **158**（0x9e） | |
| GPIO RST | **66**（0x42） | |
| SPI 引脚 | **GPIO 48 / 49 / 50 / 51**（qup4_se0_l0..l3，mux=1） | pinctrl 组 `mi_ts_spi_*_active_first` |
| avdd | fixed regulator `touch_avdd_vreg`，enable **GPIO 117**（0x75），active-high | 上电需先拉 GPIO117 |
| iovdd | RPMh `pmh0110_i_l4`（L4I，固定 **1.8V**） | 资源名 `L4I_E0` |
| 面板坐标 | max-x=**1200**（0x4b0）、max-y=**2608**（0xa30） | |
| panel-display-resolution | 1440×3200 | 显示分辨率，触控坐标用 1200×2608 |
| fod | fod-lx=**597**、fod-ly=**2406**；fod 尺寸 **246×246** | 屏下指纹区域 |
| 电极 | rx=**18**、tx=**39** | |
| 固件 | `synaptics_spi_popsicle_00.img`（cfg_0，ddic-id=1）；`synaptics_spi_popsicle_01.img`（cfg_1，ddic-id=2） | 两套 DDIC 配置 |
| test-limit | `popsicle_test_limits_S3910P.csv` | 产线测试限值（S3910P 型号） |
| qts trusted-touch | vm_mode/tvm；io-base `0x1a80000` size `0x1000`；vm-gpio-list 48/49/50/51/66/158 | |

### 1.2 GCC 时钟（GCC 基址 0x100000；主控已实锤）

| 时钟 | 投票寄存器 | halt 寄存器 |
|---|---|---|
| wrap4 M_AHB | CBCR `0x52018` BIT(22) | halt `0x23548` |
| wrap4 S_AHB | CBCR `0x52018` BIT(23) | halt `0x2354c` |
| wrap4 core | CBCR `0x52018` BIT(24) | halt `0x23550` |
| SE0（QUP4_SE0） | CBCR `0x52018` BIT(26) | halt `0xa9004` |
| SE0 RCG | cmd_rcgr @ `0xa900c` 附近 | 选频/使能（仿 redwood_goodix.c rcg_rate） |

> 注意与 redwood 差异：redwood 用 GCC_VOTE=0x52008 + SE13 的 CBCR/halt；popsicle 主屏是 **wrap4 SE0**，
> 四个时钟的 CBCR 全在 `0x52018`，但 halt 寄存器分属两组（wrap 组 0x235xx + QUP_SE0 组 0xa9004）。

### 1.3 与副屏（GT9615）对照（避免接线混淆）

| | 主屏 Syna TCM（本文对象） | 副屏 GT9615（gt9615-移植计划.md） |
|---|---|---|
| SE | qupv3_4 SE0 `0x1a80000` | qupv3_3 SE2 `0x1988000` |
| GIC | 158 | 201 |
| GPIO | 48-51 / 66 / 158 | 12-15 / 185 / 201 / 82 |
| avdd | GPIO117 | GPIO82 |
| iovdd | L4I 1.8V | L2F_E0 1.2V |
| CBCR | 0x52018 BIT(22/23/24/26) | 0x52018 BIT(14) |

---

## §2 redwood_goodix.c 骨架分解

> 精读对象：WSL `<BUILD>/hackios/xnu-src/xnu-8796.121.2/bsd/dev/redwood_goodix.c`（1515 行），
> 本地副本 `extract/syna-src/redwood_goodix.c`。行号为 2026-10-24 精读实测。

### 2.1 芯片无关骨架（可直接复用，只改参数）

| 区块（实测行号） | 内容 | 移植动作 |
|---|---|---|
| L45-69 | `rd()/wr()` volatile u32 读写 + `sleep_us/sleep_ms`（delay_for_interval） | **照搬** |
| L71-114 | TLMM GPIO：TLMM_PHYS=0x0f100000（redwood 值）；GPIO_CFG(n)=tlmm+0x1000*n（[1:0]pull [5:2]function [8:6]drive [9]output）；GPIO_IO(n)=+4；gpio_configure/set/get/output | **照搬但基址改 0x0f000000**（见 §4 第 2 行） |
| L116-261 | RPMh/cmd-db：CMD_DB_PHYS=0x80860000（magic 0x0c0330db）；RSC_PHYS=0x18220000；TCS(reg)=rsc+0xd00+reg；WRITE_4_BYTES=0x10008\|RESPONSE=0x100；rpmh_address(name)/rpmh_wait/rpmh_write/io_supply_on | **照搬**；资源名改 `L4I_E0`；io_supply_on 电压校验 1800mV |
| L263-350 | GCC 时钟：GCC_PHYS=0x00100000；GCC_VOTE=0x52008；CBCR bit31=CLOCK_OFF；rcg_rate(parent[8])；clocks_on() 投票+轮询 1000×10us | **框架照搬**；投票位与 halt 表改 popsicle wrap4 组（见 §4.5） |
| L352-556 | GENI SPI FIFO 轮询：SE_PHYS；GENI_FW_REVISION_RO 0x068 [15:8]==1 验 SPI；SE_HW_PARAM_0 0xe24 [21:16] FIFO 深度；GENI_M_CMD0 0x600 op=3<<27 FULL_DUPLEX；M_IRQ_STATUS 0x610；TX_FIFO 0x700 / RX_FIFO 0x780；TX/RX_FIFO_STATUS 0x800/0x804；M_CMD_DONE=bit0；M_ERRORS=(1<<1\|2\|3\|24\|25\|28\|29)；geni_spi_init/drain/recover/transfer | **照搬**；SE_PHYS 改 0x1a80000；全双工轮询天然适配 TCM SPI |
| L558-592 | bitbang 回退：bitbang_init/bitbang_transfer（CS 低→8bit MSB 先→CS 高）+ 函数指针 `static bool (*spi_transfer)(const uint8_t*, uint8_t*, uint32_t)` | **照搬**；TCM 报文同样是字节流 |
| L1334-1357 | show_state（全寄存器打印）/ power_on（RST 低→iovdd→avdd→延时） | **照搬改序**：popsicle 用 GPIO117 使能 avdd + rpmh L4I iovdd；RST=GPIO66 |
| L1359-1423 | use_engine_pins（4 引脚 mux=1）/ bring_up() 编排（detect→power_on→clocks_on→geni_spi_init→chip_probe，失败走 bitbang） | **照搬改值**；引脚改 48/49/50/51；SPI 目标 15MHz（DTB 上限） |
| L1425-1500 | 轮询线程框架：goodix_thread（bring_up 失败 thread_terminate；循环 gpio_get(PIN_INT) 低→chip_poll()；poll_us=8000 默认 / touch_poll_us boot-arg；idle_looks 每 80ms 兜底；60 秒统计打印） | **照搬**；PIN_INT 改 GPIO158；chip_poll 换 TCM 实现 |
| L1502-1515 | redwood_goodix_init（boot-arg touchscreen=1 门控、touch_flip、kernel_thread_start） | **照搬**；改名 popsicle_syna_init，boot-arg 用 `syna` 或复用 touchscreen=1 |
| bsd/sys/redwood_input.h:40 | `void redwood_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y);` | **直接调用**，不改接口 |

### 2.2 必须为 TCM 重写的芯片协议部分

| 区块（实测行号） | redwood_goodix 现状 | TCM 重写要点 |
|---|---|---|
| L594-893 全套 Goodix 协议 | goodix_read 帧 `0xf1`+BE addr+8B dummy；goodix_write `0xf0`；checksum 16bit 和；goodix_probe 读 FW_VERSION 0x10014/IC_INFO 0x10070；goodix_command 16B cmd+ACK/BUSY；goodix_report 8B/触点 | **全部重写**：TCM 是 A5 报文流（见 §3），无地址寄存器概念、无 0xf1/0xf0 帧 |
| L895-1297 FocalTech 半套 | focaltech 0xA0/0x00 + CRC-16/Kermit + boot ROM | **整段删除**（GT9615 草稿已删除半边的先例） |
| L1299-1332 芯片分发 | TOUCH_GOODIX/TOUCH_FOCALTECH + GPIO96/68 电平判型 | **改单芯片**：主屏只 TCM，无判型；或保留枚举仅 TOUCH_SYNA |
| L1299+ chip_probe/chip_poll 中转 | 指向 goodix_probe/goodix_poll | 指向 tcm_probe / tcm_poll |
| 报告解析 | goodix_report：id=t[0]>>4、kind=t[0]&0xf、x=le16(t+2)、65535 归一 | TCM 报文：A5 头 + code + len + payload + 0x5a 尾（见 §3.5）；坐标解析需 SET_TOUCH_REPORT_CONFIG 约定位宽（首启用 index4/class4/x12/y12 方案，见 §3.5） |

### 2.3 结论

redwood_goodix.c 的 **~60% 行是芯片无关骨架**（寄存器访问、GPIO、RPMh、GCC、GENI SPI FIFO、bitbang、轮询线程、上报接线），
移植工作量集中在：①换 5 处板级参数（§4）；②把 Goodix 协议层（~300 行）替换为 TCM A5 报文实现（~250 行）；③删 FocalTech 半边。

---

## §3 Linux Synaptics TCM 协议要点

> 精读对象：`extract/syna-src/lineage_sm8350_*`（LineageOS android_kernel_oneplus_sm8350 树
> `drivers/input/touchscreen/synaptics_tcm/` 全套 14 文件，2026-10-24 下载）。
> 行号均为实测。该树 = Synaptics 官方 TCM 驱动 + 一加板级适配，协议部分与主线同源。

### 3.1 报文格式（core.h:66-68，core.c:1167-1331）

```
读报文：  [0]=0xA5(marker) [1]=code [2..3]=payload长度(LE) [4..]payload [尾]=0x5A(padding)
写命令：  同构，code=命令码（见 §3.2），payload 前 2 字节按 write_message 自动填长度
```
- `MESSAGE_HEADER_SIZE=4`、`MESSAGE_MARKER=0xa5`、`MESSAGE_PADDING=0x5a`（core.h:66-68）。
- 读流程 `syna_tcm_read_message`（core.c:1167）：先读 MIN_READ_LENGTH=9 字节（core.c:47 预测读），
  校验 marker==0xa5；code≤0x0f 是「状态报告」（STATUS_IDLE/BUSY=无 payload，直接返回），
  否则 payload 长度不足时走 `syna_tcm_continued_read`（core.c:815）续读，最后校验尾部 padding 字节==0x5a。
- **SPI 上每次读都要发哑字节换回数据**（spi.c:426-483 syna_tcm_spi_read：tx_buf 全 0xff 全双工），
  GENI 全双工 FIFO 轮询（redwood geni_transfer）天然匹配，无需改动。

### 3.2 命令集（core.h:209-243）

首启最少命令子集：
| 命令 | code | 用途 |
|---|---|---|
| CMD_RESET | 0x04 | 软复位（硬件 RST 拉低是另一路，§3.4） |
| CMD_IDENTIFY | 0x02 | 拿 version/mode/part_number/build_id/max_write_size |
| CMD_GET_BOOT_INFO | 0x10 | bootloader 模式下的 boot status/asic_id/flash 布局 |
| CMD_GET_APPLICATION_INFO | 0x20 | app 模式：status/static_config_size/max_x/max_y/max_objects/customer_config_id |
| CMD_SET_TOUCH_REPORT_CONFIG | 0x26 | 约定触点报告的字段位宽（touch.c:880-891） |
| CMD_ENABLE_REPORT | 0x05 | 开流（app 模式且 APP_STATUS_OK 时有效） |
| CMD_GET_FEATURES | 0x32 | dual_firmware 能力（复位流程用） |
| CMD_RUN_APPLICATION_FIRMWARE | 0x14 | bootloader→app 跳转 |

其余（REFLASH 0x12/0x13、ERASE 0x11、DOWNLOAD_CONFIG 0x30、COMMIT_CONFIG 0x28、REZERO 0x27、DEEP_SLEEP 0x2c/0x2d、PRODUCTION_TEST 0x2a/0x31 等）**首启全部不需要**。

状态码（core.h:245-256）：STATUS_IDLE=0x00 / OK=0x01 / BUSY=0x02 / CONTINUED_READ=0x03 / ERROR=0x0f / INVALID=0xff。

### 3.3 bootloader vs app 模式（core.h:162-190）

- `id_info.mode`（CMD_IDENTIFY 响应首部）：MODE_APPLICATION=0x01、MODE_HOST_DOWNLOAD=0x02、
  MODE_BOOTLOADER=0x0b、MODE_TDDI_BOOTLOADER=0x0c、MODE_PRODUCTION_TEST=0x0e。
- app 模式 `app_info.status`：APP_STATUS_OK=0x00 / BOOTING=0x01 / UPDATING=0x02 / BAD_APP_CONFIG=0xff。
- bootloader 模式 `boot_info.status`：BOOT_STATUS_OK=0x00 / APP_BAD_DISPLAY_CRC=0xfc / BAD_APP_FIRMWARE=0xfe 等。
- **判断逻辑**（core.c:2152-2233 syna_tcm_identify）：CMD_IDENTIFY → 按 mode 分派
  （APPLICATION→CMD_GET_APPLICATION_INFO；BOOTLOADER→CMD_GET_BOOT_INFO）。
- **首启策略**：RST 释放后先 CMD_IDENTIFY；若 mode=APPLICATION 直接进入 report 配置；
  若 mode=BOOTLOADER 则发 CMD_RUN_APPLICATION_FIRMWARE（0x14）后重 IDENTIFY（core.c:2704-2724 的 reset 流程同款）。
  出厂已烧 fw 的机器理论上直接 APP 模式（§3.6）。

### 3.4 硬件/软件 reset 时序（core.c:2633-2742）

`reset(hw=true)`：
1. `gpio_set_value(reset_gpio, reset_on_state)`（RST 低有效：拉低）
2. `msleep(reset_active_ms)`（保持低电平宽度）
3. `gpio_set_value(reset_gpio, !reset_on_state)`（拉高）
4. `msleep(reset_delay_ms)`（boot delay）
5. `identify()` → 若 bootloader 模式则 CMD_RUN_APPLICATION_FIRMWARE + MODE_SWITCH_DELAY_MS=100ms（core.c:83）+ 重新 identify
6. app 模式 + APP_STATUS_OK → 发 CMD_GET_FEATURES 读 dual_firmware 位。

`reset(hw=false)` = CMD_RESET 软复位，之后同样等待 `reset_delay_ms` + identify。
具体 `reset_active_ms/reset_delay_ms` 数值是 DT 板级参数（spi.c:141-165 的 `synaptics,reset-active-ms/reset-delay-ms`），
popsicle DTB 无这两项 → 归入「无法确定项」用默认值（TCM 惯例 reset_active=20ms、reset_delay=100ms 量级），实机调。

### 3.5 report buffer 与坐标解析（touch.c:238-410, 880-891, 961-1022）

- **开启流程**（touch_init → touch_set_input_reporting，touch.c:961）：app 模式且 APP_STATUS_OK 才继续；
  CMD_SET_TOUCH_REPORT_CONFIG 下发**触点字段位宽表**（touch.c:875-891 的 out.buf 构造）：
  ```
  TOUCH_FOREACH_ACTIVE_OBJECT(1)
    TOUCH_OBJECT_N_INDEX(2), 4bit
    TOUCH_OBJECT_N_CLASSIFICATION(3), 4bit
    TOUCH_OBJECT_N_X_POSITION(4), 12bit
    TOUCH_OBJECT_N_Y_POSITION(5), 12bit
  TOUCH_FOREACH_END(4)
  TOUCH_END(0)
  ```
  （枚举 touch_report_code，touch.c:50-80；12bit 与 max-x=1200/max-y=2608 的 4096 满量程对齐。）
- 然后 CMD_GET_APPLICATION_INFO 拿 max_x/max_y/max_objects 建 input 设备，最后使能中断/轮询。
- **报告到达**：code=REPORT_TOUCH(0x11)（core.h:261；core.c:796-803 dispatch → touch_syncbox → touch_report，touch.c:1091-1109）。
- **坐标解析**（touch_parse_report，touch.c:238-410）：按上面下发的 config 表逐字段 bit 级抽取
  （offset 累加、TOUCH_PAD_TO_NEXT_BYTE 跨字节对齐）；object 状态 LIFT=0/FINGER=1/GLOVED_FINGER=2；
  无触点（num_of_active_objects==0 或 payload 耗尽）即上报全抬起。
- **上报差分**（touch_report，touch.c:616-669）：与 redwood contact[] 差分思路一致——
  prev_status[idx] 对比，DOWN/MOVE/UP 三类事件，坐标经 swap_axes/x_flip/y_flip 变换
  （max_x-1200/max_y-2608 为基准）。移植时映射到 `redwood_touch_post(type,finger,x,y)`。
- 轮询读触发与 redwood 相同：GPIO158 低电平→读报文（或 80ms 兜底）。

### 3.6 fw/config 首启是否可跳过（结论 + 判断依据）

**结论：firmware 首启跳过（出厂已烧）；config 首启跳过（用芯片出厂默认 report 布局不行——必须下发 SET_TOUCH_REPORT_CONFIG 的运行时位宽约定，但不需要烧写 flash config）。**

判断依据：
1. **驱动不自动刷 fw**：官方 core 的 probe（core.c:3308-3540）与 deferred_probe（core.c:3602）**不含任何
   request_firmware/flash 写入**；刷写只在 `reflash.c` 的 sysfs 手动触发（reflash_sysfs_reflash_store，reflash.c:414-517），
   用户态不写 reflash 节点就绝不刷。→ XNU 移植不实现 reflash 半边即可。
2. **出厂已烧的判据**：RST 后 CMD_IDENTIFY 直接返回 MODE_APPLICATION + APP_STATUS_OK 即证明 fw 在 flash 中；
   只有 boot_status=BAD_APP_FIRMWARE(0xfe) 才需要下载（core.h:170-177）。XNU 首启流程在 identify 后
   若见 bootloader 模式只发 CMD_RUN_APPLICATION_FIRMWARE（0x14），不实现 host-download 协议
   （reflash.c/zeroflash.c 那套 WRITE_FLASH/ERASE_FLASH 全部不移植）。
3. **config 的「两段」区分**：
   - **运行时 report config**（CMD_SET_TOUCH_REPORT_CONFIG 0x26）= 掉电即失的寄存器约定，**必须下发**，否则触点字段位宽未知；
   - **flash 内 static/app config**（CMD_DOWNLOAD_CONFIG 0x30 + COMMIT_CONFIG 0x28，reflash.c:1861-1918 的
     reflash_update(app_config) 路线）= 出厂已按 ddic-id 烧好（popsicle DTB 的 00/01.img 就是两套 DDIC 出厂配置），
     首启**不碰**。
4. **风险兜底**：若实机 app_status==BAD_APP_CONFIG(0xff)，说明出厂 config 异常 → 该场景需要 reflash 半边
   （DOWNLOAD_CONFIG+COMMIT_CONFIG），列为无法确定项 §6.6，首启先不实现，观察后再定。
5. **test-limit CSV**：`popsicle_test_limits_S3910P.csv` 只在产线测试（PRODUCTION_TEST 命令 + testing.c
   的 open/short/raw 数据比对）里用，驱动首启完全无关（官方 testing.c 里也没有 CSV 解析，限值由产线工具侧比对）。
   → **不移植 testing 模块**。

### 3.7 官方驱动里可丢弃的模块（XNU 移植范围裁剪）

| Linux 文件 | 首启需要 | 说明 |
|---|---|---|
| synaptics_tcm_spi.c | ✅ 全部参考 | SPI 传输（rmi_read/rmi_write 仅在 RMI 模式用，TCM A5 模式只需 read/write 两函数） |
| synaptics_tcm_core.c | ✅ 核心流程 | probe/reset/identify/read/write_message/dispatch，砍 sysfs/watchdog/notifier |
| synaptics_tcm_touch.c | ✅ 核心流程 | report config 下发 + 坐标解析 + 差分 |
| synaptics_tcm_reflash.c | ❌ 首启不移植 | 仅观察 fw 镜像结构（boot_config/app_config 分区与 ddic-id 关系） |
| synaptics_tcm_zeroflash.c | ❌ | host-download 裸片烧写 |
| synaptics_tcm_recovery.c | ❌ | 恢复模式 |
| synaptics_tcm_testing.c / diagnostics.c | ❌ | 产线测试 |

---

## §4 五处板级差异表（redwood → popsicle 主屏）

| # | 项目 | redwood_goodix.c 现值（实测行号） | popsicle 主屏值（主控已实锤） | 寄存器/位 |
|---|---|---|---|---|
| 1 | SE 基址 | `SE_PHYS 0x00a94000`（SE13，L352 起） | **`0x1a80000`**，size 0x4000 | GENI 寄存器布局不变（FW_REV 0x068、M_CMD0 0x600、TX_FIFO 0x700 等全套偏移照用） |
| 2 | TLMM 基址 | `TLMM_PHYS 0x0f100000`（L71，redwood 值） | **`0x0f000000`**（合并 DTB `/soc/pinctrl@f000000` reg 实锤 `<0x0f000000 0x202000>`；popsicle_goodix.c:97 同值，MemoryMapLib TLMM 描述符同） | GPIO_CFG(n)=tlmm+0x1000*n 公式不变；**注意 redwood 是 0x0f100000，勿照抄** |
| 3a | SPI GPIO | 引擎引脚 redwood 组（L1359 use_engine_pins） | **48/49/50/51**，qup4_se0_l0..l3，mux=1 | 每脚 GPIO_CFG [5:2]=1（function） |
| 3b | IRQ GPIO | PIN_INT（redwood 组） | **158**（0x9e） | 输入、低有效（沿 GIC 158 上升/下降沿取触发） |
| 3c | RST GPIO | redwood RST 脚 | **66**（0x42） | 低有效 |
| 4a | avdd | io_supply_on 的固定/投票路径（redwood avdd） | fixed `touch_avdd_vreg`，enable **GPIO117**（0x75）active-high | GPIO117 输出高 = avdd 上电 |
| 4b | iovdd | io_supply_on 资源名（L116-261 RPMh 全套） | RPMh `L4I_E0`，**1800mV** | rpmh_write(VRM addr L4I, 0x2008 电压词, MSGID_WRITE_4_BYTES\|RESPONSE)；电压校验 1800 |
| 5a | GCC 投票 | GCC_VOTE=0x52008（L263-350） | **0x52018**（wrap4 的 AHB_CBCR），BIT(22) M_AHB / BIT(23) S_AHB / BIT(24) core / BIT(26) SE0 | CBCR bit31=CLOCK_OFF 轮询 |
| 5b | GCC halt | redwood halt 表 | M_AHB `0x23548` / S_AHB `0x2354c` / core `0x23550` / SE0 `0xa9004` | |
| 5c | SE0 RCG | redwood SE13 RCG | cmd_rcgr @ `0xa900c` 附近 | rcg_rate() 选频（15MHz 目标）；parent 表照 redwood |

**上电顺序（仿 redwood power_on L1334-1357 改编）**：RST(66) 拉低 → iovdd(L4I) → avdd(GPIO117=1) →
延时 → RST(66) 拉高 → 延时(reset_delay_ms) → clocks_on(5a/5b) → geni_spi_init(15MHz) → tcm_probe。
（GT9615 草稿顺序是 reset 低→avdd→iovdd→INT；Synaptics 参考 DT 惯例 avdd 先行也可，两者都在 §6 实机验证清单里备选。）

---

## §5 文件级移植步骤

### 5.1 新增文件 `bsd/dev/popsicle_syna.c`（唯一新文件）

分区块（自上而下）：
1. 头注释：来源 = redwood_goodix.c 骨架 + LineageOS sm8350 synaptics_tcm 协议；popsicle 主屏参数表。
2. 复制 redwood_goodix.c 芯片无关区块（§2.1 表，逐段带行号注释来源）。
3. `#define` 板级参数区：SE_PHYS 0x1a80000、GPIO 48/49/50/51/66/158/117、CBCR/halt 表（§4）、L4I_E0。
4. TCM 协议层（替换 Goodix 段）：
   - `tcm_transfer(tx,rx,len)`：GENI 全双工（tx NULL 则发 0xff 哑字节，等效 spi.c:426-483）。
   - `tcm_write_message(code,payload,len)`：组 A5 帧（头 4B + payload + 0x5a 尾）。
   - `tcm_read_message()`：读 MIN_READ_LENGTH=9 → 校验 0xa5 → 按 payload 长续读 → 校验 0x5a 尾
     （对应 core.c:1167-1331，简化掉 mutex/completion）。
   - `tcm_probe()`：RST 时序（§3.4）→ CMD_IDENTIFY → 按 mode 分派 GET_APP_INFO/GET_BOOT_INFO →
     app 且 APP_STATUS_OK → 发 CMD_SET_TOUCH_REPORT_CONFIG（§3.5 位宽表）→ 记录 max_x/max_y。
   - `tcm_poll()`：读报文 → code==0x11 → 按位宽表解触点 → contact[] 差分 → `redwood_touch_post`。
5. 轮询线程 `syna_thread`：照 redwood goodix_thread（L1425-1500）改 GPIO158/兜底间隔。
6. `popsicle_syna_init()`：boot-arg 门控（`syna=1` 或沿用 `touchscreen=1`）+ kernel_thread_start。
7. 编译护栏：只 `#include <sys/types.h>` 等 redwood_goodix.c 已用头；不加任何 Linux 头。

### 5.2 `bsd/conf/files` 注册（实测行号）

```
112: bsd/dev/redwood_input.c		standard
114: bsd/dev/redwood_goodix.c	standard
```
在第 114 行后追加一行：
```
bsd/dev/popsicle_syna.c		standard
```

### 5.3 `bsd/sys/redwood_input.h` 声明（实测行号）

第 40 行 `void redwood_touch_post(...)` 之后加：
```
void popsicle_syna_init(void);
```

### 5.4 `bsd/dev/redwood_input.c` 接线（实测行号）

- `redwood_input_init()` 在 **redwood_input.c:209**，devfs 节点创建于 :213（`devfs_make_node(..., "redwood-touch")`）。
- 任务指认的「219 行附近」实测 = **redwood_input.c:219 `redwood_goodix_init();`**（init 调用序列
  redwood_cpufreq→virtio→**goodix**→buttons→battery→ufs→net→adreno→wifi→adsp）。
- 接线动作：在 :219 `redwood_goodix_init();` 后加 `popsicle_syna_init();`（主屏在副屏之前或之后均可，
  两个驱动完全独立，顺序只影响打印次序）。
- 若 GT9615 草稿也按计划接在 :219 之后，两处调用并列：`redwood_goodix_init(); popsicle_goodix_init(); popsicle_syna_init();`。

### 5.5 与 GT9615 草稿共用 `/dev/redwood-touch` 的并发注意

1. **单一全局队列**：redwood_input.c 只有一套 touch_queue + `touch_lck_grp`（实测 :59），
   `redwood_touch_post`（:138）无芯片来源标识。两个驱动的线程都会往同一队列 post。
2. **同时只跑一个芯片**：主屏/副屏同时触摸时事件混流，用户态读到的 finger id 可能碰撞。
   首启（锁屏）场景只有主屏触摸 → **短期可接受**；若后续双屏同时用，需给 `redwood_touch_post`
   加 source 参数（改 h:40 接口 + :138 入队结构 + 用户态约定），列为二期改动，**首启不做**。
3. **boot-arg 门控互斥**：GT9615 用 `touchscreen=1` 门控（redwood_goodix 同款）。为避免两个驱动同抢，
   syna 门控参数用独立名（如 `syna=1`），首启经 loader 传参同时开两个时，popsicle_syna.c 先探测
   CMD_IDENTIFY 成功才继续——**探测失败立即 thread_terminate，不 print 错误刷屏**（复用 goodix 的失败安静退出路径）。
4. **GENI/GPIO 互不冲突**：两芯片 SE/GPIO/GIC 完全不同（§1.3 表），寄存器级无竞争；唯一共享点是
   轮询线程都查各自 GPIO，且都调用 redwood_touch_post——线程调度天然互斥，无需加锁。

### 5.6 构建与核验

1. `cd xnu-8796.121.2 && make ARCH_CONFIG=ARM64 KERNEL_CONFIG=DEVELOPMENT`（复用方向 2 已验证配方）。
2. 核验：`strings mach.development.bcm2837 | grep -c syna` 应 ≥1；`grep -c "popsicle_syna" ` 目标文件行数。
3. 与 GT9615 同批编译：两个新 .c 同时进 files，一次构建同时验收。

---

## §6 无法确定项清单（每条待实机验证）

| # | 无法确定项 | 预研倾向 | 实机验证命令/动作 |
|---|---|---|---|
| 6.1 | RST 时序精确值：popsicle DTB 无 `synaptics,reset-active-ms/reset-delay-ms` | 默认 active 20ms / delay 100ms，量级照 TCM 惯例（core.c:83 MODE_SWITCH_DELAY_MS=100） | `adb shell dmesg \| grep -i syna` 看 identify 结果；失败则 50ms 步进调 delay |
| 6.2 | 上电顺序（avdd 先 vs iovdd 先）与 GPIO117 使能时序 | 计划默认 RST 低→iovdd→avdd→RST 高（redwood 顺序） | 上电后 `CMD_IDENTIFY` 响应是否 0xa5 开头；无响应换 avdd 先行重试 |
| 6.3 | SPI 实际可跑频率（DTB 上限 15MHz，GENI 全双工 FIFO 轮询能否吃满） | 首启 5MHz 保守起，成功后升 15MHz | 读 9 字节报文 marker==0xa5 即链路 OK；错帧则降频/开 byte_delay_us 单字节模式（spi.c:439-483 的 fallback 思路） |
| 6.4 | IRQ 触发沿与极型：GIC158 是否与 TLMM 158 配置一致（pinctrl active_first 组的 bias） | redwood 模板按低有效轮询（轮询模式不依赖 IRQ 触发沿） | 轮询模式下 `gpio_get(158)` 触摸时是否翻低；统计打印每秒报告数非 0 |
| 6.5 | CMD_IDENTIFY 后芯片处于哪个 mode：出厂 fw 是否直接 APP 模式（§3.6 判断依据） | 出厂直接 APPLICATION + APP_STATUS_OK | 启动日志预期打印 `Firmware mode = 0x01` + `Application status = 0x00`（core.c:2727-2739 同款打印） |
| 6.6 | app_status==BAD_APP_CONFIG(0xff) 的概率：出厂 config 异常才需要 DOWNLOAD_CONFIG/COMMIT_CONFIG | 概率极低，首启不实现 reflash 半边 | 若日志出现 `Application status = 0xff` → 二期补 DOWNLOAD_CONFIG(0x30)+COMMIT_CONFIG(0x28)，用 ddic-id 选 00/01.img |
| 6.7 | report config 位宽表：12bit x/y 是否与出厂默认一致（芯片出厂 config 可能用 16bit 或不同 index 位宽） | 12bit 与 max 1200/2608 匹配，为官方 touch.c 默认 | 触摸划动看坐标是否连续 0..1200/0..2608；越界/跳变则改位宽表重发 SET_TOUCH_REPORT_CONFIG |
| 6.8 | 坐标映射：TCM max_x/max_y 报告值 vs 面板 1200×2608 的翻转/偏移（x_flip/y_flip/swap_axes） | 默认不翻转（参考 DT 无 flip 属性） | 锁屏划动方向是否与手指一致；反向则启用 flip（touch.c:637-643 的变换逻辑） |
| 6.9 | fod（屏下指纹 597,2406 246×246）坐标是否经同一触点流上报、是否影响锁屏 | 不影响锁屏滑动 | 锁屏 OK 后按 fod 区域触摸看报告坐标落区 |
| 6.10 | qts trusted-touch（GIC647/io-base 0x1a80000/vm-gpio-list）在无安全虚拟机时是否干扰 SPI 事务 | 无 VM 场景不触发 | 首启日志无 qts 报错即可；若 SPI 偶发卡死查 tvm 寄存器占用 |
| 6.11 | GCC wrap4 时钟投票：halting 检查 vs 实际（0x52018 BIT(26) SE0 是否还依赖 M_AHB 先行） | 按 redwood 四时钟同时投票 | `show_state()` 打印 0x52018/0xa9004；CBCR bit31 全 0 才继续 |
| 6.12 | boot-arg 传递：XNU 经 loader 传 `syna=1` 的参数是否被 redwood 的 PEArgv 解析链接受 | 复用 touchscreen=1 同链（redwood_goodix_init L1502 的 boot-arg 读取） | `xl log \| grep -i syna` 应打印 init 入口行 |

---

## §7 首启里程碑判据（锁屏滑动可用 = 目标）

| 级别 | 判据（日志/行为） | 对应验证命令 |
|---|---|---|
| **L1 驱动加载** | `popsicle_syna_init` 入口打印（boot-arg 门控通过）；files 注册无编译错误；strings 命中 `syna` | `strings mach.development.bcm2837 \| grep -c syna` ≥1 |
| **L2 板级资源就绪** | 打印 `no command database`/`no regulator L4I_E0`/`clocks stay off` 等 redwood 风格错误**不出现**；show_state 打印 SE/TLMM/GCC 寄存器非 0 | `xl log \| grep -i syna`（XNU 日志经 loader） |
| **L3 芯片握手** | CMD_IDENTIFY 响应 marker==0xa5；打印 `Firmware mode = 0x01`（APPLICATION）+ `Application status = 0x00`；bitbang 回退不触发 | `xl log \| grep -i syna`；`adb shell dmesg \| grep -i syna` |
| **L4 报告流开启** | SET_TOUCH_REPORT_CONFIG 成功；触摸时 `gpio_get(158)` 翻低且 tcm_poll 解出 REPORT_TOUCH(0x11)；统计打印报告数>0 | `xl log \| grep -i "report"`；`adb shell dmesg \| grep -i syna` |
| **L5 坐标上报正确（目标）** | `finger N at x, y` 打印，x∈[0,1200] y∈[0,2608] 连续无跳变，方向与手指一致；**锁屏滑动可用** | `xl log \| grep -i "finger"`；锁屏实际划动验证 |

判据晋升规则：L1/L2 属编译与寄存器级，可在 XNU 构建验收；L3-L5 必须实机（USB 恢复后经 loader 起 XNU）。

---

## 附录 A：本地证据文件

- `extract/syna-src/redwood_goodix.c`：XNU 骨架精读对象（1515 行，行号引用实测）。
- `extract/syna-src/lineage_sm8350_synaptics_tcm_{core.c,core.h,spi.c,touch.c,reflash.c,testing.c,...}`：
  LineageOS sm8350 树官方 TCM 驱动全套 14 文件（协议行号引用实测）。
- `extract/syna-src/redwood_input.c`：XNU 接线点对象（227 行）。
- 行号实测命令：`grep -n` 于 WSL xnu 树 + 本地副本；§2 行号来自 2026-10-24 精读笔记。

## 附录 B：与 GT9615 计划的衔接

- 本计划 = gt9615-移植计划.md 的主屏姊妹篇；两者共用 redwood_input.c:219 接线点（§5.4-5.5）。
- 编译验证统一在方向 2 的 theos/XNU 构建环境做（两个 .c 同批进 files）。
- 首启目标聚焦主屏（Synaptics = 锁屏硬依赖），副屏 GT9615 为第二阶段。
