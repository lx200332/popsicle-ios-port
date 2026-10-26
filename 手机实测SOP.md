# popsicle 刷 iOS 16.5 —— 手机实测 SOP

> 状态：离线全部完成并逐字节验收；唯一剩余是实机 `fastboot boot`。
> 更新：2026-10-24（ABL 要求 PE/COFF、PAYLOAD_BASE=0x888000000、logfs 回退版）

## 0. 前置条件（均已就绪）

| 前置 | 状态 |
|---|---|
| **实机镜像** `popsicle-v4-pe.img`（4,276,224 B，sha256 `b56af40a…`） | ✅ `<WORKSPACE>\popsicle-port\artifacts\popsicle-v4-pe.img` 与 WSL `<BUILD>/hackios/out/popsicle-v4-pe.img` |
| PE shim（4,268,032 B，Subsystem=10 EFI_APP，零 boot services 调用） | ✅ `popsicle-pe-shim.efi`（3d06c7ac…） |
| UEFI FD（4 MiB，XnuLoader hook + "XNU Payload" Reserv @0x888000000 + logfs 诊断回退） | ✅ `SILICIUM_UEFI.fd` sha256 `645a4913…` |
| loader.efi 手机版（PAYLOAD_BASE=0x888000000，含 platform_popsicle） | ✅ md5 `c1213b8f…`（镜像内嵌条目已核验同 md5） |
| 自编 XNU 内核（xnu-8796.121.2-dirty，QEMU 回归通过） | ✅ `mach.development.bcm2837` sha256 `fa7569c1…` |
| 一键实测脚本 / 验证器 | ✅ `<TOOLS>\cache\hk-real-test-pe.sh` / `hk-verify-pe-img.mjs` |
| usbipd-win 5.3.0 + WSL vhci-hcd 自动加载 + WSL fastboot/adb | ✅ 全部就绪 |

## 1. 手机插入 USB 后（直通）

```bash
# Windows PowerShell：
& 'C:\Program Files\usbipd-win\usbipd.exe' list        # 找手机 BUSID（18d1:xxxx）
& 'C:\Program Files\usbipd-win\usbipd.exe' bind -b <BUSID>
& 'C:\Program Files\usbipd-win\usbipd.exe' attach -w -b <BUSID>

# WSL 里验证：
fastboot devices    # 应见 <MY_SERIAL> fastboot（若在 Android 态先 adb reboot bootloader）
```

## 2. 实机启动

**阶段 A：fastboot boot PE 镜像（本版核心路径）**

```bash
cd <BUILD>/hackios
fastboot boot out/popsicle-v4-pe.img
# 预期：ABL PeCoffLoader 装载 PE → shim 把 FD 搬 0xC6800000、payload 搬 0x888000000
#       → UEFI 起 → BootManager UnableToBoot → XnuLoaderMain
#       → 在 0x888000000 找 loader.efi → 进 loader（USB 重枚举为 xnuloader）
# 判据：fastboot getvar product 含 xnuloader = SUCCESS
```

**阶段 B：xl 工具链（上传系统镜像 + 跑内核）**

```bash
cd <BUILD>/hackios
./xl enter                                          # 进 loader（用修好的 env 重打包）
./xl ramdisk userland/system.apfs                   # 上传 2.5GB 系统镜像，约 80 秒
# 基础 BOOTARGS + ④⑤ 五个调查驱动门控（逐外设排查时单独保留对应门控）
XL_SECS=900 XL_CMDLINE="touchscreen=1 syna=1 cpufreq=1 bootcpu=7 cpus=8 wifi_probe=1 audio_probe=1 fp_probe=1 camera_probe=1 modem_probe=1" ./xl run
./xl screenshot logs/screen.png                     # 拉屏验证
./xl log                                            # 拉日志（含 popsicle_wifi/audio/fp/camera/modem 调查输出）
./xl reboot                                         # 回 Android
```

## 3. 预期里程碑（按序）

1. **loader 屏 + USB 日志**：`./xl log` 有 `platform popsicle`、`display 1200 x 2608 at fc800000`
2. **内核 -v 文本**：Darwin Kernel Version 22.5.0（自编 = 带 `-dirty` 与当日时间戳）
3. **SpringBoard 锁屏**：`./xl screenshot` 有画面
4. **触控**：主屏 Synaptics TCM 与副屏 GT9615 双驱动已编译进内核（`syna=1`/`touchscreen=1` 门控）——实机坐标上报待验证；15 处 [TBD §6.x] 参数按首启日志逐个收敛
5. **④⑤调查输出**：`./xl log` 里依次出现 `popsicle_wifi: FastConnect 7900 survey ...` / `popsicle_audio: ...` / `popsicle_fp: ...` / `popsicle_camera: ...` / `popsicle_modem: ...`（五门控开启时）

## 4. 已知风险与预案

| 风险 | 预案 |
|---|---|
| FAIL A：停在 ABL fastboot（product 仍 popsicle） | 镜像未被执行——核对 hv=4 与 PE 头（验证器 hk-verify-pe-img.mjs） |
| FAIL B：设备消失→看门狗重启回 Android | 链早期崩溃——**读 logfs 分区取 XnuLoader 256KB 诊断**（logdump 不存在，logfs 回退已入 FD；`adb shell su -c "dd if=/dev/block/by-name/logfs of=/sdcard/lf.bin bs=4096"` 后拉回分析） |
| platform 选成 qemu-virt | loader 打印的 fb_base 不是 0xFC800000 → 查 UEFI GOP |
| 屏幕黑 | `./xl screenshot` 直接拉帧缓冲（GOP 的 0xFC800000），不依赖屏显 |
| 手机写保留区崩溃 | PAYLOAD_BASE=0x888000000 已避开 gunyah hypervisor 保留区（0xE0000000-0xFBFFFFFF reserved 已实锤）与全部 48 条 FDT reserved + 62 条 UEFI 描述符 |

## 5. 产物对照（当前有效）

| 文件 | 值 |
|---|---|
| `artifacts/popsicle-v4-pe.img` | sha256 `b56af40acba420865c3fdc5002413513cd87e9989351e3b7dc457ab475275a62` |
| `artifacts/popsicle-pe-shim.efi` | sha256 `3d06c7ac…` |
| `artifacts/SILICIUM_UEFI.fd-logfs` | sha256 `645a4913…` |
| `artifacts/loader-popsicle.efi` | md5 `c1213b8f…` |
| `artifacts/mach.development.bcm2837-five45` | sha256 `c836fd92…`（**实机当前内核**：双触控+五调查驱动；`-gt9615` 后缀版 43184fcb 是早一版） |

## 6. 注意事项

- **UEFI 重编陷阱**：WSL/Windows 时钟漂移会让 make 假跳过 → 先 `touch` 旧 Build 时间戳或删 Build 全量重编（`<TOOLS>\cache\rebuild-uefi-force.sh`）。
- **loader 版本陷阱**：QEMU 回归会临时切 qemu 版 loader（PAYLOAD_BASE=0xF0000000），跑完必须恢复手机版（`cp -a loader-phone.efi loader.efi`）再打包，否则镜像内 loader 指向错误地址。
- 镜像重打一条命令：`<TOOLS>\cache\hk-mk-pe-img.sh`（FD/payload 变化后）。
