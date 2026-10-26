# popsicle 指纹 / 相机 / 基带 驱动预研事实

> 提取日期 2026-10-24 · 材料：厂商 DTB（popsicle.dtb）、MiCode xk-dt、soc-repo xk-popsicle、真机提取包（10-props-logs / 06-display-touch / 03-modem）
> 约定：查不到的证据写「无据」，不臆测。

---

## 一、指纹

### 1.1 控制器型号
- **「Goodix 超声波 us2」无据**。全 DTB 无 `us2` / `ultrasonic` / `fp` 的 SPI 子节点 compatible 字符串；驱动目录（`drivers/input/fingerprint/mi_fp/`）中 `fp_ultra.h` 仅有 `FW_EVENT_IPC=3`、`MAX_FW_EVENTS=128`，「ultra」是文件名，不构成超声波证据。
- 内核驱动确认为小米指纹框架驱动 **mi_fp**：
  - `fp_driver.h:181`：`#define DRIVER_COMPATIBLE "xiaomi,xiaomi-fp"`
  - `fp_driver.c:1237`：`static int fp_probe(struct spi_device *driver_device)`
  - `fp_driver.c:1563`：`static struct spi_driver fp_spi_driver`（`spi_register_driver` 注册）
  - **mi_fp 是标准 Linux SPI 框架驱动**，与 GT9615（副屏触控）同为 SPI 从设备类驱动，框架层可复用；但绑定靠 OF compatible 匹配。

### 1.2 总线 / reg / IRQ / GPIO / 供电（DTB `/soc/xiaomi_fingerprint`）
| 项 | 值 |
|---|---|
| compatible | `xiaomi,xiaomi-fp` |
| IRQ | tlmm **99**（`xiaomi,gpio_irq`，pinctrl irq_default bias-pull-down） |
| IPC | tlmm **99**（`xiaomi,gpio_ipc`） |
| vendor | `P2_n2_132_431`（`xiaomi,vendor_names`） |
| fp_3v3 | L9B = pmh0101_l9，3.3V（phandle 0x43d） |
| fp_1v8 | `fingerprint_1v8_vdd` = **GPIO56**，1.8V enable-active-high（固定 regulator） |
| 其他 | netlink-event=25；sensor-loc=<480 1906 719 2165>；intr2-pin-enable + side-config="TIC" + default-electrical=<1 1> |
| 源 | popsicle-sm8850.dtsi:9-40（pandora/pudding 同款） |

### 1.3 SPI 通路：**无据**
- DTB 全部 `spi@` 节点枚举结果（仅 4 个）：ir-spi@0（se0）、goodix_gt9615v@0（spi@1988000=qupv3_se15，副屏触摸）、spi_cnss（spi@1990000，WLAN）、synaptics_tcm@0（spi@1a80000=qupv3_se19，主屏触摸）。
- **无指纹 SPI 子节点**，I2C 设备表亦无指纹。mi_fp 的 `fp_probe(struct spi_device*)` 在 popsicle DTB 上没有可绑定的 SPI 从设备。
- 真机佐证：dmesg-full.txt 中 xiaomi-fp 只有 `fp_screen_state_for_fingerprint_callback`（DRM PANEL BLANK/UNBLANK，command 2/3），无 probe/寄存器读日志；input-devices.txt 无指纹设备（仅两个 Xiaomi_Touch_Input、cs40l26 haptic、pwrkey、Headset）。
- **结论**：驱动框架与 GPIO/供电事实齐全，但 SPI 总线号、CS、片选、时钟极性等链路事实缺失 → 移植需要的关键通路参数「无据」。是否 GT9615 同款可复用 SPI 框架：**框架可复用（同为 spi_driver），总线资源不可复用（GT9615=se15 触摸总线）**。

---

## 二、相机

### 2.1 四摄型号（实锤，真机 getprop 凯撒+1 解码）
| # | 角色 | 型号 | 模组 bin |
|---|---|---|---|
| 0 | wide 主摄 | **STX9500U** | `/odm/lib64/camera/qti/sensormodule/popsicle_ofilm_stx9500u_wide_i.bin` |
| 1 | front 前摄 | **OV50M** | `popsicle_ofilm_ov50m_front_i.bin` |
| 2 | ultra 超广角 | **OV50M** | `popsicle_ofilm_ov50m_ultra_i.bin` |
| 3 | tele 4x 长焦 | **S5KGN8** | `popsicle_ofilm_s5kgn8_tele_i.bin` |

- 全系 **ofilm** 模组。mi.module.info：back_main=0x99、back_ultra=0x8D、front_main=0x8D、back_tele4x=0x90。
- 源：getprop-all.txt:1581-1609（modulename0-3，凯撒+1：`` ` ``=空格）。

### 2.2 CCI 控制器与总线挂载
- **CCI 3 个**：`qcom,cci0@941b000` / `cci1@941c000` / `cci2@941d000`，reg 各 0x1000，IRQ GIC 426/427/428（0x1aa/0x1ab/0x1ac），clock-rates 0x23c3460=37.5MHz，pctrl-map m0/m1 四态。cci1/cci2 无子设备。
- 挂载：
  - `qcom,cam-sensor0` @ cci0，**cci-master=1**，compatible="qcom,cam-sensor"（wide STX9500U）
  - `qcom,cam-sensor3` @ cci0，**cci-master=0**，use-shared-clk（tele S5KGN8）
  - `qcom,cam-sensor1` @ i2c@884000（qupv3_2_geni_se@8c0000），**reg=0x10**（front OV50M）
  - `qcom,cam-sensor2` @ i2c@880000，**reg=0x36**（ultra OV50M）
- 同 bus 外设：eeprom1@0x51 / actuator1@0x9（sensor1）；eeprom2@0x50 / actuator2@0x9（sensor2）。

### 2.3 时钟 / GPIO
| 传感器 | MCLK | RESET | csiphy | aon-id |
|---|---|---|---|---|
| s0 wide | cam_mclk5，gpio94（clock <0x53 0xc>） | CAMW_RESET gpio106（pinctrl cam_sensor_wide_reset） | 1 | - |
| s1 front | cam_asc_mclk4，gpio93（clock <0x53 0xa>） | CAM_RESET4 gpio7（active_rst4） | 4 | 0 |
| s2 ultra | cam_asc_mclk2，gpio91（clock <0x53 0x6>） | CAM_RESET2 gpio3（active_rst2） | 2 | 1 |
| s3 tele | cam_mclk3，gpio92（clock <0x53 0x8>） | CAM_RESET1_1P8 gpio15（active_rst1） | 0 | - |

- 全部 `clock-rates=0x124f800` = **19.2MHz nominal**。gpios raw 实证：s0=<tlmm 94, 106>，s3=<tlmm 92, 105（0x69）>。sensor-position：s0/s2/s3 yaw=180 roll=90；s1 yaw=0 roll=270。

### 2.4 供电（regulator-names + rgltr-min/max 逐项，单位 V）
| rail | s0 wide | s1 front | s2 ultra | s3 tele |
|---|---|---|---|---|
| cam_vio | 1.2–1.8（pm8010_m_l5） | 1.2–1.8（l5m） | 1.2–1.8（l5m） | 1.2–1.8（l5m） |
| cam_vdig | 0.952（pmh0101_l18） | 1.1（pm8010_m_l1） | 1.1（pm8010_m_l2） | 1.08（pmh0110_f_l1） |
| cam_vana | 2.8 | 2.8（pm8010_m_l3） | 2.8（pm8010_m_l6） | 2.2 |
| cam_v_custom1 | 1.8 | - | - | 1.8（pm8010_m_l4） |
| cam_vaf | 3.308 | 2.832（pmh0101_l14） | 2.832（pm8010_m_l7） | 3.304–3.544（pmh0101_l16） |
| cam_v_custom2 | - | - | - | 3.304（pmh0101_l2） |
| cam_v_custom3 | 1.2（camera_iovdd_ldo=regulator-1v2） | 1.2 | 1.2 | 1.2 |
| cam_bob | 3.4–3.76（pmh0101_bob1） | - | - | 3.4–3.76（bob1） |
| cam_bob2 | 3.296（pmh0101_bob2） | 3.296（bob2） | 3.296（bob2） | - |

- s3 另有 `ois-src`（长焦 OIS）。regulator phandle 关键映射：bob1=0x434、bob2=0x435、l2=0x437、l14=0x43e、l16=0x43f、l18=0x441、pm8010_m_l1-7=0x464-0x46a、pmh0110_f_l1=0x44e、regulator-1v2=0x279。
- 注意：0x277/0x278/0x27a/0x280 在 DTB 中与 modem2-etm0 endpoint、trigout_a、tmc、sp_region **重复编号**（s0 的 cam_vana/cam_v_custom1/cam_vaf、s3 的 cam_vana 引用这些 phandle 但目标节点非 regulator）→ 这几个 rail 的 regulator 身份以 rgltr 数组电压为准，具体 LDO 来源「无据」。x86 pinctrl 组 cam_sensor_wide_avdd/avdd1/afvdd/tele_* 存在但无 phandle 引用佐证。
- xk-dt 相机材料仅 canoe-pmic-overlay.dtsi:47-102（cam_sensor_1p8_rst0/1），canoe.dtsi 无 cam-sensor 节点 → 相机 DT 为厂商私有，MiCode 侧无源。真机 vendor.camera-provider pid 2242、cameraserver pid 2943 存活。

### 2.5 hackios redwood camera HEIC 复用度
- **「无据」**：popsicle-port 工作区没有任何 hackios/redwood/HEIC 材料（全部 12 个 md 均为触控/显示/构建相关）。以 redwood 有 camera HEIC 实现为前提的复用度评估，需先取得 hackios 框架源码，否则不成立。

---

## 三、基带

### 3.1 平台与固件
- getprop：`gsm.version.baseband = MPSS.DE.9.0-1a29cb0006,MPSS.DE.9.0-1a29cb0006`；modem_build_datetime=Oct 15 2026 13:39:53；modem_sw_mbn_count=421；modem_release=Sep 11 2026；active_modems.max_count=2；sim_slots.count=2。
- dmesg：modem 固件代号 **peach_v2**（`[2873:I:IPA]` wlan_ipa_* 多处出现）；firmware 路径 `/vendor/modem_firmware/image/peach/qdss_trace_config_perf_v2.cfg`；rmt_storage `/boot/modem_fs1` 4MB。
- **「X85」字符串全材料无据**（DTB/getprop/dmesg/soc-repo 均无 x85）。

### 3.2 IPA / rmnet 节点（DTB 实锤）
- `qcom,ipa@3e00000`：compatible="qcom,ipa"；reg ipa-base 0x3e00000/0x84000 + gsi-base 0x3e04000/0xfc000；**qcom,ipa-hw-ver=<0x18>**；ipa-irq=GIC 0x28f、gsi-irq=0x1b0；qcom,ee=0；qcom,platform-type=1；cfg-offset 0x140000；firmware-names="ipa_fws"；pas-ids=0xf；interconnect 3 路径（ipa_to_llcc/llcc_to_ebi1/appss_to_ipa）；qcom,use-ipa-tethering-bridge；qcom,ipa-wdi3-over-gsi。
- `qcom,rmnet-ipa`：compatible="qcom,rmnet-ipa3"（rmnet-ll/ctl enable、napi、sg）。
- `qcom,ipa_fws`：compatible="qcom,pil-tz-generic"，firmware-name="ipa_fws"，pas-id=0xf，status=disabled。
- reserved-memory：mpss_region@8aa00000、q6_mpss_dtb_region@99500000、ipa_fw_region@99580000、ipa_gsi_region@99590000、camera_region@99600000、camera_2_region@99e00000。
- dmesg 运行时：`ipa3_wwan_ioctl dev(rmnet_data4) register to IPA`、`rmnet_data3 link add`、IPA_CLIENT_APPS_WAN_CONS。
- soc-repo 侧：canoe_perf.bzl:185 `CONFIG_QCOM_IPA:"n"`（vendor 内核关 IPA 数据路径）；canoe_tuivm.bzl:44 CONFIG_QCOM_MEM_BUF_IPA_RESERVE=y；drivers/ 无 net/ipa 源码目录。

### 3.3 固件签名链
- modem：`remoteproc-mss@04080000` compatible="qcom,canoe-modem-pas"，firmware-name="modem.mdt","modem_dtb.mdt"；wdog GIC_SPI 264（0x108）；cx=VDD_CX_LEVEL、mx=VDD_MODEM_LEVEL；qcom,qmp；glink-edge remote-pid=1 label=modem glink-label=mpss；QRTR/DS 通道（qcom,modem_qrtr=qcom,ipcrtr、qcom,modem_ds）。smp2p-modem、smp2p-ipa-1-in/out、qrtr-mhi-cnss 均在。
- IPA：`qcom,ipa_fws` 走 **PIL-TZ**（qcom,pil-tz-generic，pas-id 0xf）→ IPA 固件与 modem.mdt 同属 QCOM 安全启动信任链（TZ 校验 + hyp/PIL 加载）。
- 真机分区（03-modem，_inventory 实测大小）：modem_a/b 各 160MB；modemfirmware_a/b 各 200MB；mbnconfig 32MB；fsg 8MB；modemst1/2 各 8MB（sha256：modemst1=3e18062119fac07cb5f0867db73f42c77d46c8cb896826a1f94c08d8085d22ac、modemst2=f6b674661be0a2f4c94bf5b6a6d10ed9bf724a6825a007f44dac8140acbe726f）；apdp/apdpb 256KB；fsc 128KB；multiimgoem/qti 各 32KB。

### 3.4 hackios 基带支持路径评估
- 工作区无任何 hackios 框架/基带驱动材料（grep hackios/redwood/HEIC 零命中）→ 框架侧是否已有 modem/RIL 抽象「无据」。
- 从 popsicle 侧事实看，任何基带支持路径必须越过：① modem.mdt/modem_dtb.mdt 的 **QCOM PIL-TZ 安全启动签名校验**（TZ 锁定，无签名固件不加载）；② mpss 的 QRTR/DS/glink 协议栈与 IPA 数据面（rmnet-ipa3）；③ Android 侧 mbnconfig 421 个 SW MBN 的载波配置体系。
- **阻塞于：无 hackios 框架/基带驱动材料 + QCOM PIL-TZ 固件签名链（modem.mdt 与 ipa_fws 均受 TZ 校验）**。在取得 hackios 框架源码与红木 modem 驱动实现之前，基带为不可评估状态。

---

## 附：材料来源索引
- 厂商 DTB：`C:\dsh-tools\cache\popsicle.dtb`（1,723,024B，解析器 `C:\dsh-对话\popsicle-port\tools\fdt.mjs`）
- MiCode DT：`/root/build/xk-dt/qcom/`（canoe.dtsi、canoe-pmic-overlay.dtsi）
- soc-repo：`/root/build/xk-popsicle/`（drivers/input/fingerprint/mi_fp/、canoe_perf.bzl、canoe_tuivm.bzl）
- 真机包：`D:\Backup\Downloads\666\tiqu\10-props-logs\`（getprop-all.txt、dmesg-full.txt）、`06-display-touch\input-devices.txt`、`03-modem\`（分区镜像 + _inventory + 00-VERIFY.txt）
