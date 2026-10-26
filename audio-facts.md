# popsicle（Xiaomi 17 Pro Max · SM8850/canoe）iOS 16.5 hackios 移植 — ⑤音频驱动预研

> 数据来源：厂商合并 DTB 反编译 `dts/popsicle_overlay.dts`（ODM DTBO）、MiCode `canoe_base.dts`（canoe.dtb 反编译）、WSL `/root/build/xk-dt/qcom/`（MiCode 源码树）、`/root/build/xk-popsicle/`（soc-repo）、真机提取 `D:\Backup\Downloads\666\tiqu\`。查不到的写「无据」，不臆测。

## 1. 音频控制器型号（真机已知 → DTB 验证）

| 芯片 | 数量 | DTB 节点 | compatible | 状态 | 真机 dmesg 证据 |
|---|---|---|---|---|---|
| WCD9395（WCD939x 家族，USB-C 音频开关 + split codec） | 1 | overlay fragment@228 `wcd939x-codec` | `qcom,wcd939x-codec` | ok | dmesg 片段无 wcd939 字样；但 init 日志确认 ADSP 音频 PD 运行 |
| WCD939x USBSS I2C 开关 | 1 | base `i2c@a8c000/wcd939x_i2c@e` | `qcom,wcd939x-i2c` | ok | fragment@27 引 `qcom,wcd_usbss`（USB 驱动侧） |
| CS35L43（×2：底部 B / 顶部 T 双扬声器） | 2 | overlay fragment@240 `cs35l43@40` / `cs35l43@42` | `cirrus,cs35l43` | ok | `cs35l43 13-0040 / 13-0042`（Linux i2c-13） |
| CS40L26A（haptic 振动 + 音频共流） | 1 | overlay fragment@240 `cs40l26@43` | `cirrus,cs40l26a` | ok | `cs40l26-codec cs40l26-codec.3.auto`（平台设备名） |
| 外置 WSA 扬声器功放（wsa884x） | 0（本机不用） | overlay `wsa884x@02170220/02170221` | disabled | disabled | — |
| swr-haptics（旧 WSA haptic 槽） | 0 | overlay `swr_haptics@f0170220` | disabled | disabled | — |

**真机三颗 Cirrus 共流铁证**（`dmesg-full.txt:1446-1455`、`3625-3634`）：三颗 codec 同时收到 `hw_params wl=32, width=32, rate=48000, channels=3` + `set_sysclk freq=6144000` —— 同一条 TDM/ASP 总线、48 kHz×32 bit×4 slot（3 声道有效 + 1 空槽），MCLK 6.144 MHz（=48k×128）。

## 2. I2C 总线与设备参数

### 2.1 主音频 I2C：qupv3_se13 → i2c@1980000（Linux i2c-13）

- 位置：`/soc/qcom,qupv3_3_geni_se@19c0000/i2c@1980000`（canoe_base.dts:17337-17369；别名 `i2c13`）
- reg = `0x1980000 0x4000`；GIC 中断 `interrupts=<0 0x175 4>`（**GIC_SPI 373**，IRQ_TYPE_LEVEL_HIGH）
- 基树 status=disabled，overlay fragment@240 置 ok
- 引脚（base :2764-2803 qupv3_se13_i2c_pins）：SDA=**GPIO64** func `qup3_se0_l0`、SCL=**GPIO65** func `qup3_se0_l1`，bias-pull-up

| 设备 | reg | 真实 I2C 地址 | 复位 GPIO | IRQ | sound-name-prefix | 关键私有参数 |
|---|---|---|---|---|---|---|
| cs35l43@40 | 0x40 | `cirrus,i2c-addr=0x28` | GPIO10（`reset-gpios=<tlmm 0xa 0x0>`，低有效） | `interrupts=<0xb 0x2008>` = **GIC_SPI 11** LEVEL_HIGH | "B"（底部） | bst-ipk-ma=0xfa0(4000mA)、asp-sdout-hiz=0x3、gpio2-src-sel=0x4、vpbr{thld 4,max-att 6,atk-vol 5,enable 1}、low-pwr-mode-standby=1 |
| cs35l43@42 | 0x42 | `cirrus,i2c-addr=0x2a` | GPIO54（`0x36 0x0`） | `interrupts=<0x2f 0x2008>` = **GIC_SPI 47** LEVEL_HIGH | "T"（顶部） | 同上；另有 pinctrl `rcv_gpio_high/low`=**GPIO174**（接收器/听筒模式切换） |
| cs40l26@43 | 0x43 | —（I2C 地址=reg 0x43，haptic core 走 `cirrus,cs40l26a` of_match） | GPIO9（`0x9 0x1`，flag=1 高有效） | `interrupts=<0x50 0x8>` = **GIC_SPI 80** | — | f0-default=0x2a8000(2.785 MHz 谐振)、vpbr-thld-uv=0x2a713c/max-att 9dB、vbbr-thld-uv=0xa0384/max-att 4dB、dbc{thresh 0x384 fs,rise 0x5b8d80,fall 0x6acfc0,env 0x7a1200}、bst-ipk-microamp、pm-stdby 500ms/pm-active 200ms |

pinctrl（fragment@244）：GPIO10/54/9 reset = output-high + drive-strength 16mA + bias-disable；GPIO11/47 IRQ = input-enable + bias-pull-up + 2mA；GPIO80 IRQ = input-enable + 2mA（无上拉）。

### 2.2 WCD9395 控制 I2C：qupv3_se3 → i2c@a8c000（Linux i2c-3）

- canoe_base.dts:16858-16883：reg=`0xa8c000 0x4000`，GIC 中断 `interrupts=<0 0x33f 4>`（**GIC_SPI 831**），`qcom,shared`
- 子节点 `wcd939x_i2c@e`：reg=0xe，`vdd-usb-cp-supply=<&L15B>`——WCD9395 的 USB-C/audio 模式开关控制通道（不是 codec 数据通道）
- canoe.dtsi（WSL 源码 :4280）与 canoe-mtp.dtsi(:48-61) 补充：SPMI 中断 `interrupts=<0x0 0xb6 0x1 IRQ_TYPE_EDGE_BOTH>`（interrupt-parent=spmi0_bus），`interrupt-names="usb_wcd"`，nvmem `usb_mode`，`mode-switch`
- 复位：overlay fragment@242/244 给 wcd_usbss 加 `reset-gpios=<tlmm 0xa2 0x0>` = **GPIO162**（active=output-high、sleep=output-low，16mA）

## 3. SoundWire 控制器（LPASS 内，SM8850 SPF 架构）

base `spf_core_platform`（canoe_base.dts:21460-21553），overlay fragment@208 填参数：

| 控制器 | 别名 | 基址 | SWRM IO 基址 | IRQ | master_id | 设备 |
|---|---|---|---|---|---|---|
| rx-macro@6AC0000/rx_swr_master | swr1 | 0x6AC0000 | `swrm-io-base=0x6ad0000` | GIC_SPI 155（0x9b） | 2 | `wcd939x-rx-slave` reg=<0xe 0x1170224>（WCD9395 RX 侧） |
| va-macro@7660000/va_swr_master | swr2 | 0x7660000 | `0x7630000` | GIC_SPI 761/785（0x2f9/0x311） | 3 | `wcd939x-tx-slave` reg=<0xe 0x1170223>（TX/麦克风侧） |
| wsa-macro@6B00000 | swr0 | 0x6B00000 | — | — | — | **disabled**（无外置 WSA） |
| wsa2-macro@6AA0000 | swr3 | 0x6AA0000 | `0x6ab0000` | GIC_SPI 171（0xab） | — | wsa884x×2 disabled、swr_haptics disabled |
| tx-macro@6AE0000 | — | 0x6AE0000 | — | — | — | 空壳 |
| lpass_bt_swr@6CA0000 | swr4 | 0x6CA0000 | `0x6ca0000` | GIC_SPI 138（0x8a） | — | BT |

- fragment@234：`lpass_cdc qcom,num-macros=3`（rx/va/tx）
- LPI TLMM（fragment@212，`lpi_pinctrl@07760000`，基址 0x7760000，23 个 GPIO）：swr_tx = GPIO0(clk,f1)/1/2；swr_rx = GPIO3(clk,f1)/4/5；wsa_swr=GPIO10(f2)/11；另有 lpi_i2s0-4 引脚组
- LPASS 时钟投票：`lpass_cdc` 挂 `vote_lpass_core_hw`/`vote_lpass_audio_hw`（audio-ref-clk src 9/11）

## 4. 供电（PMIC 通道已由 overlay 符号表解析）

**WCD939x-codec**（fragment@228）：
| 电源 | PMIC 通道 | 电压 |
|---|---|---|
| cdc-vdd-buck | **L15B** | 1.8V（0x1b7740，on-demand） |
| cdc-vdd-rx | **L15B** | 1.8V |
| cdc-vdd-tx | **L15B** | 1.8V |
| cdc-vdd-px | **L1G** | 1.2V（0x124f80，on-demand） |
| cdc-vdd-mic-bias | **BOB1** | 3.4~3.75V（0x33e140~0x395f80） |
| micbias1-4 | 芯片内 | 2750mV（0xabe） |

wcd939x_i2c@e：vdd-usb-cp = L15B。Cirrus 三颗的供电在 DT 中**无据**（无 supply 属性——固定电源直连，Android 侧由复位时序控制）。

## 5. 音频数据路径（DT + dmesg 交叉验证）

```
canoe-mtp-snd-card (sound 节点, qcom,model="canoe-mtp-snd-card")
  ├─ asoc-codec: stub + lpass-cdc + wcd939x_codec(0x31b) + swr-haptics(0x31c, disabled)
  ├─ qcom,tdm-audio-intf=1, tdm-max-slots=4, tdm-clk-attribute=4（invert, couple no）
  ├─ qcom,pri-mi2s-gpios=pri_tdm_pinctrl(0x31a)
  ├─ audio-routing: AMIC1-5 / MIC BIAS1-4 / TX DMIC0-3 / VA DMIC0-3 / IN1_HPHL / IN2_HPHR / IN3_EAR(AUX_OUT)
  └─ wcd939x-i2c-handle=wcd_usbss
```
- **扬声器 TDM（pri TDM0）**：基树 pinctrl（canoe_base.dts:4322-4536，与 canoe-pinctrl.dtsi 一致）——SCK=**GPIO122**、WS=**GPIO125**、SD0=**GPIO123**、SD1=**GPIO124**，func `i2s0_sck/ws/data0/data1`，8mA、bias-disable；sleep 时 func=gpio + pull-down + input-enable。TDM1 = GPIO117/119/118/120（本机未用）
- **帧结构**：48kHz × 32bit × 4 slot，3 声道有效（B、T 各 1 声道 + haptic），sysclk/MCLK 6.144 MHz；CS40L26 ASP 取 RX1/RX2 两槽（`cs40l26.c` FRAME_CONTROL5 slot 寄存器，`PREVENT_WAKEUP_BY_DAI_OPS` 下 tdm_slot={0,1}）
- **ADSP**：SPF/AudioReach（GPR）架构——`remoteproc-adsp@03000000/glink-edge/qcom,gpr` 为 audio_gpr（canoe_base.dts:22029）。真机 `init.svc.vendor.audioadsprpcd_audiopd=running`。**旧 Lahaina 的 APR/ASM/ADM/AFE 驱动树在 canoe 上不存在**（xk-popsicle `sound/soc/` 下只有 codecs/ 目录，无 qcom 机器驱动/AFE 驱动——MiCode soc-repo 不含音频 DSP 协议栈，只有 cs40l26 ASoC codec + haptic input 驱动）
- **CS40L26 双驱动**：`drivers/input/misc/cs40l26_haptic/`（I2C probe，of_match `cirrus,cs40l26a`，register map）+ `sound/soc/codecs/cs40l26.c`（**纯 platform 驱动 `cs40l26-codec`，无 DT 匹配表**——platform 设备由厂商闭源 machine driver 动态注册，dmesg 的 `cs40l26-codec.3.auto` 即 .3.auto 平台编号，机制无据）
- **CS35L43 驱动**：soc-repo 中**无**（grep cs35l43 全树零命中）——厂商闭源模块，固件 `/odm/firmware/{B,T}-cs35l43-delta-music-spk-prot.bin` v7.30.1（DSP1 扬声器保护算法，cal_r_sel 9179/7827）
- **接收器模式**：cs35l43@42 通过 GPIO174 高/低切换 rcv 模式（听筒复用顶部扬声器）
- **WCD9395**：split codec（qcom,split-codec=1），RX 走 SoundWire master2、TX 走 master3；USB-C 模拟音频/数字耳机由 MBHC（msm-mbhc-hphl-swh=0 无硬件开关）经 USBSS 切换芯片进入

## 6. XNU 侧可参考路径（hackios redwood）

| 文件 | 作用 | 对 popsicle 的可复用性 |
|---|---|---|
| `bsd/dev/redwood_virtio_sound.c` | QEMU virtio-sound（VIRTIO_ID_SOUND=25，mmio 0x0a000000） | **直接可用**——骨架阶段（QEMU 模拟器出声）零修改 |
| `bsd/dev/redwood_audio.c` | /dev/redwood-audio 字符设备（48kHz/2ch/16bit，ioctl 音量），双 sink 分发 | 直接可用；真机 sink 目前空缺（注释原话："On the phone there is none yet (redwood_speaker.c is where it begins)"） |
| `bsd/dev/redwood_sound.c` | 真机出声：boot-arg sound=1..4，**ADSP APR 协议**（ASM 流 + ADM 路径 + AFE 端口），PRI_TDM_RX_0 端口 0x9000/组 0x9100，样本经 SMMU 流 0x1801 共享内存 | **协议层不兼容**：redwood=Lahaina APR/ASM/ADM/AFE；popsicle=SM8850 **AudioReach GPR**。TDM 参数思路（48k×32bit×slot、位时钟 3.072MHz、invert、槽映射）完全可迁移，但 GPR 客户端必须新写 |
| `bsd/dev/redwood_speaker.c` | 手写 GPIO I2C 直驱双 AW88261（POCO X5 Pro），ADSP 提供 TDM 时钟 | **模式可复制**：popsicle 上 = 手写 I2C 配 CS35L43×2（寄存器与 AW88261 完全不同，需 CS35L43 寄存器图），TDM 时钟仍依赖 ADSP 起 AFE |
| `bsd/dev/redwood_haptic.c` | 手写 I2C 直驱 AW86927 马达 | 模式参考（CS40L26 寄存器图不同），且 popsicle 的 CS40L26 振动与音频共用 TDM 流 |
| `bsd/dev/redwood_adsp.c` | ADSP 启动/关停 | 启动逻辑可参考；GPR 握手需重写 |

## 7. 结论：骨架路线与阻塞点

**骨架路线（建议执行顺序）**
1. **QEMU 阶段**：沿用 `redwood_virtio_sound.c` → iOS 音频栈即可出声。零风险。
2. **真机扬声器最小闭环（iOS 出声）**：`redwood_speaker.c` 模式 + `redwood_sound.c` 模式二选一或组合：
   - I2C 侧：手写 GPIO I2C 配 CS35L43@0x28/0x2a（SDA64/SCL65、RST 10/54、IRQ 11/47），写入功放寄存器、加载 delta 保护固件（`.bin` 从 /odm/firmware 提取）
   - TDM 侧：样本投递给 ADSP 的 GPR 流（等价 redwood 的 ASM/AFE 路径）。TDM0 引脚（122/125/123/124）由 ADSP 控制
3. **音频+振动**：同一 TDM 流 3 声道喂给 CS40L26（振动通道）
4. **耳机/麦克风（远期）**：WCD9395 split codec 需要 XNU 侧 SoundWire master 驱动（rx-macro 0x6AD0000 / va-macro 0x7630000）+ USBSS 模式切换（i2c@a8c000@0xe + GPIO162）——工作量最大，首版可 skip

**阻塞点（按严重度）**
- **B1（高）**：popsicle 的 ADSP 是 **AudioReach GPR 协议**，redwood 的 APR ASM/ADM/AFE 客户端（redwood_sound.c/redwood_adsp.c）不能直接复用。需为 SM8850 新写 GPR 客户端（open graph → 配置 AFE 端口 → 建流 → 写样本）。SM8850 的 AFE 端口 ID / 图配置在现有材料中**无据**（真机 dmesg 无 AFE 端口号日志，需真机 mixer/`adb shell dumpsys media.audio_flinger` 或 tinymix 抓取）
- **B2（高）**：TDM 总线由 ADSP 独占驱动（Android 下 XNU 无法让 ADSP 的 GPR 图同时给 Android 和 XNU 用——hackios 是全盘替换，ADSP 固件仍在，但 GPR 图配置需 XNU 重建）。若想完全绕开 ADSP 自驱动 TDM，需要写 LPASS LPAIF/TDM 硬件驱动（无参考代码，寄存器无据）
- **B3（中）**：CS35L43 驱动（含 DSP 固件加载/保护算法握手）厂商闭源，XNU 侧需从零实现 I2C 寄存器图 + firmware 加载（固件二进制可提取，格式靠 Cirrus 公共 Linux 驱动 cs35l41/43 族参考）
- **B4（中）**：CS40L26 在 Android 中由闭源 machine driver 注册为 `.3.auto` 平台设备；XNU 侧走同一 TDM 流取 2 槽即可振动，寄存器图需参考公共 Cirrus cs40l26 驱动（soc-repo 有完整 ASoC + haptic 驱动源码，**寄存器图可从这两份 GPL 源码直接提取**）
- **B5（低）**：WCD9395 麦克风/耳机链路（SoundWire master + USBSS 开关 + MBHC）——留到最后，首版不阻塞

**一句话结论**：扬声器出声可行——QEMU 用现成 virtio；真机路线 = 手写 I2C 直驱 CS35L43（redwood_speaker.c 模式）+ 新写 SM8850 GPR 客户端把样本送进 TDM0（redwood_sound.c 模式，协议层重写），首个真实阻塞点是 GPR 图/AFE 端口号无据，需真机抓取。
