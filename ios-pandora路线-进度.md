# popsicle → iOS（ios-pandora 路线）进度与状态

> 最后更新：2026-10-24 14:30  
> 参考实现：ShueiTsuki/ios-pandora（小米 17 Pro / SM8850，同 SoC）

## ★ 里程碑（2026-10-24 14:25）：真内核在 popsicle 真机上启动到用户态 PID 1

**整条链在真机上跑通了。** `fastboot boot` 后手机屏幕输出完整 XNU 启动日志（照片 20261024_142517.jpg / 20261024_142649.jpg 佐证）：

| 阶段         | 屏幕证据                                                                                                                                                                             |
| ---------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| skywalk 自检 | `sk_copy32_0 … PASSED` / `sk_copy64_64x … PASSED` / `sk_zero_128 … PASSED` / `memcmp mask … PASSED`（全部 PASSED）                                                                   |
| BSD 初始化    | `bsd_init: calling memorystatus_init / sysctl_mib_init / bsd_autoconf / kerninit / loattach`                                                                                     |
| 网络栈        | `ifnet_attach: All kernel threads created for interface lo0/gif0/stf0 have been scheduled` + `ether_family_init` + `net_init_run`                                                |
| 根挂载        | `Added memory device md0/rad0 (01000000/12000000) at FFFFFFFF00650C000` → `BSD root: md0, major 1, minor 0` → `vfs_mountroot` → `VFS_ROOT`                                       |
| init 加载    | `load_init_program: attempting to load /sbin/launchd`                                                                                                                            |
| **用户态**    | **`*** SWEET userspace: init running as PID 1 ***`** + `SWEET init: still alive` 持续心跳                                                                                            |
| 后续         | `eventhandler_init / aio_init / pshm_cache_init / psum_cache_init / hw_wq_init / dlit_init` + `DLIL: running SUM6 self-tests … PASSED` + `socketinit / domaininit / skwalk_init` |

### 这次的镜像与参数（可复现）

| 项        | 值                                                                                                                                                                                                                                                                   |
| -------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 内核       | `C:\项目\xnu-kernel\kernel.development.pandora` 16,796,672 B，sha256 `3afe0758c3ad548e9d76e7dda76c1a5d727eade5999136e611963f4d71330b29`（4K 页 xnu-8796.141.3，`-sectalign,__TEXT,__text,0x1000`）                                                                         |
| ADT      | `mkadt_popsicle.py` → 18,960 B，sha256 `d2f758308670644f0240e621dd882f30bb42dbc16c5d7f2b867aca9f7b030420`（FB 1200×2608×stride 4800，PHYS_BASE 0x8c0000000）                                                                                                            |
| payload  | `mkpayload-real.py`（virt_base 用 `min(s['vmaddr'])` 版）→ 16,210,784 B，sha256 `bf92ff43acbfdd23d5e3185395b0f977b8c9679b1ffdc229242d9c007cd296be`；cmdline `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=md0`                                |
| RAM disk | `sweet-init` 16,635 B（clang-18 + `mkinit.py` 在 Linux 离线编出的静态 arm64 Mach-O），sha256 `f5de3714ff95e6a3b9ece30723f677de0f3f74ec5eb985a00c5ae872f0ac9bf1`                                                                                                                |
| loader   | `shim/loader-popsicle/wsl-out/loader-boot.img` 16,261,120 B，sha256 `35c371fb9a07c7faea8e5d35126ac90c24db9b02c13ac7bf9dcfeef0a0a76f5d`（Image 16,255,840 B sha256 `7d0c721723ef46ce1399d67c8324b57baade3562f72ad7c85b9694f6aaf0d3df`，`image_size 0xf80b60`、flags 0xa） |
| 启动方式     | `fastboot boot loader-boot.img`（只内存，零写入）                                                                                                                                                                                                                            |

### 之前的 hello 通路验证（同日 14:15，先证格式正确）

- `popsicle-hello-boot.img` 20,480 B → `Sending 20 KB OKAY / Booting OKAY`，fastboot t=5s 消失，手机 **~115s** 回 Android（≈ hello 的 120s 倒计时 + PSCI reset）。
- logfs 佐证：`Handling Cmd: download:00005000`（20 KB）→ `Download Finished` → `Handling Cmd: boot`。
- **根因彻底坐实**：以前失败全因镜像格式错（PE/COFF MZ 或 gzip）；ABL 只认**裸 arm64 Linux Image**（偏移 56 `ARM\x64`、偏移 16 image_size、flags 0xa、v4 头、不带 ramdisk）。

### 脚本（全部可重跑）

- `<TOOLS>\cache\diag\build-sweet-init.sh`
- `<TOOLS>\cache\diag\mk-payload-real.sh`
- `<TOOLS>\cache\diag\build-loader-real.sh`
- `<TOOLS>\cache\diag\run-hello-boot.sh` / `run-loader-boot.sh`

### 当前手机状态

停在 `SWEET init: still alive` 心跳循环里（PID 1 测试程序不退出）。退出方式：长按电源键 10-15 秒。

### 下一步（到 iOS 锁屏只差 iOS 根文件系统）

- 现在 `rd=md0` 挂的是 sweet-init 测试 RAM disk。要到锁屏必须换成 **iOS 16.7.16 (20H392) 的 HFS+ 根**：
  1. 下载 IPSW（iPhone10,3 / d22ap，约 5 GB）
  2. `wsl/prep_root_tools.sh`（apfs-fuse 源码编译 + hfsplus.ko + 解 IPSW）
  3. `tools/mkiosroot.sh`（apfs-fuse 挂 OS/SystemOS/AppOS → mkfs.hfsplus -J → rsync + dyld cache 硬链）
  4. `pinfile` 把 iosroot.img 钉在 userdata（免清数据）+ 取 extents
  5. `mkadt_popsicle.py --root-extents <json>` + `mkpayload --cmdline "… rd=ufsroot0"` → 重编 loader → `fastboot boot`

## 核心结论（不变）

- 之前 `fastboot boot` 失败的根因 = **镜像格式错**（PE/COFF 或 gzip），不是 ABL 不支持。正确格式 = **裸 arm64 Linux Image**（偏移 56 `ARM\x64`、flags 0xa、v4 头、不带 ramdisk）。
- pandora 与 popsicle 同 SM8850/canoe，板级几乎全同，唯一真差异 = 分辨率（pandora 1220×2656 / popsicle 1200×2608）。

## 已完成（本机离线，全部实测通过）

### 1. shim 编译（WSL clang-18 交叉，无需 macOS/NDK 27）

| 产物                        | 大小      | sha256                                                           |
| ------------------------- | ------- | ---------------------------------------------------------------- |
| popsicle-hello-Image.bin  | 16124 B | 1bfe206e7c3572aca92cc409fe504ab73c9a870ccdcb3b0379843a45046a994e |
| popsicle-hello-boot.img   | 20480 B | da9a639b15c9cfd2edacafff8d3498ad975763b9faf3547cd302f7b5d60e503d |
| popsicle-loader-Image.bin | 49833 B | 7d95615c1d1bf1dc71cb7b0d9019387aadc80115bf14ceab9aea3de7871a8f1a |
| popsicle-loader-boot.img  | 57344 B | 0320939e537b055da9bda82ff419e2e51357470f9d087acbeecf8080fb6c11a2 |

- 全部 flags 0xa / raw arm64 Image / header v4 / 无绝对重定位。
- loader-popsicle 编译用 dummy payload（真 payload 需内核 + mkpayload.py）。
- 产物在 `<TOOLS>\cache\pandora-out\`。

### 2. ADT 生成器

- `tools/mkadt_popsicle.py`（19931 B）：FB_W/H/STRIDE 已改 1200/2608/4800；compatible 保持 'xiaomi,pandora'（内核 patch 0110 硬编码匹配）。
- 实跑产 `/tmp/popsicle.adt` = 18960 字节 ✓（唯一 warning = d22ap.adt 缺失，仅影响 /product 节点）。

### 3. 触控对齐（直接复用 pandora_touch.c，零改动）

- popsicle 主屏触控 = Synaptics S3910P（与 pandora 同款）。
- 板级参数逐项一致：SE 0x01a80000、TLMM **0x0f100000**（真值）、IRQ/RST/AVDD GPIO 158/66/117、SPI 48/49/50/51、iovdd RPMh L4I_E0 1.8V、GCC vote 0x52018 + SE0 RCG 0xa9014。

### 4. 免清数据工具链（pinfile + sweet-init）

- `pinfile`：3,348,216 B，静态 arm64 Android（本机 NDK 25.1.8937393 编译），sha256 37d0d90b…。
- `sweet-init`：16635 B，静态 Mach-O（系统 clang-18 `--target=arm64-apple-ios16.0` + ios-sweet/tools/mkinit.py）。
- `hfsprogs` 已装（mkfs.hfsplus / fsck.hfsplus，mkiosroot.sh 依赖）。

### 5. loader-popsicle 编译（WSL clang-18，dummy payload）

- 产物：Image 49152 B sha256 `1ac19b4b9169f0aa20db300ada2796e1aa28258e548822fe9635c933224c545a`；loader-boot.img 53248 B sha256 `5d8ec4d4e3f28352fc91efab9d8d23891d7f040436a0fefd8892077558995b10`。
- 用 dummy payload（SWEETXNU v1 + 1 空段）验证编译链，flags 0xa / raw Image / v4 / payload 内嵌 @0xb000 4096 对齐 ✓。
- 构建脚本 `<TOOLS>\cache\diag\build-loader-popsicle.sh`（WSL clang-18，替代 NDK 27.2）。

### 6. QEMU 链路验证（工具链正确性证明，关键里程碑）

- 链路：`mkadt_popsicle.py --qemu`（ADT 13192 B）→ `mkpayload.py`（hackios 内核 15,179,520 B → payload 15,193,728 B，fixups=0、virt_base 0xfffffff006000000、entry 0xfffffff00744a4a8、kernel_end 0xfffffff008264000、11 个段）→ `qemu-sweet.py`（30 秒）。
- QEMU 结果：phys_base 0xb0000000、boot_args 0xb2268000、**entry 0xb144a4a8（= entry_va − virt_base + phys_base，地址换算正确）**；CPU 真正进入内核执行（PC 推进到 0xfffffff0074514e8，即 entry+0x7040 之后）。
- **预期内**：内核在 EL1 里 data abort 循环（ESR 0x96000046 = EC 0x25 Data Abort at EL1，PC 0xfffffff00744ed68）。原因：hackios 内核（xnu-8796.121.2，16K 页，Mu-Silicium UEFI 引导约定）≠ pandora 内核（xnu-8796.141.3，4K 页 + sweet/pandora patch 的 boot_args/ADT 约定），boot_args/ADT 字段对不上是必然。**这不是工具链失败，恰好证明工具链（ADT 生成→payload 打包→段放置→CPU 跳转）端到端正确**。
- 结论：本机可离线完成的全部工具链环节已验证通过；只差 pandora 真内核（4K 页）即可真机引导。
- 脚本 `<TOOLS>\cache\diag\run-qemu-verify.sh`，产物 `<BUILD>/qemu-verify/`。

## 待办 / 阻塞

### 阻塞 A：内核（唯一硬卡点）

- pandora 内核只在私有仓库 GitHub Actions 编译（macOS + Xcode 15），本机无此环境。
- run 55 的 `xnu-kernel` artifact（60.4 MB，sha256 c4e1eb53…）**未过期（2027-01-20）**，但匿名下载 **401 Requires authentication**。
- 三条路：①用户给 GitHub PAT（repo 权限）→ gh run download；②fork 跑 xnu.yml；③请作者发 release。
- 本机 Linux 有 theos ld64-951.9（支持 arm64），理论上可 local_relink.py 重链接，但仍需 CI 的 objects/buildtree artifact（同样 401）。

### 阻塞 B：手机插上（验证 hello 通路）

- 判据：`fastboot boot popsicle-hello-boot.img` → 屏幕打印 POPSICLE/EL1/CPU/内存/FB 1200×2608，且不回退 slot a。
- 当前手机不在线（adb/fastboot 空，usbipd Connected 无手机）。

### 待办 C：iOS root 镜像（内核到位后可离线完成）

- 依赖：apfs-fuse（需从源码编译，FUSE3）+ hfsprogs（已装）+ IPSW 提取（iPhone10,3 16.7.16 20H392）。
- 流程：`wsl/prep_root_tools.sh`（hfsplus.ko + apfs-fuse + unzip IPSW）→ `mkiosroot.sh`（apfs-fuse 挂载 OS/SystemOS/AppOS → HFS+ 镜像）。
- 免清数据：pinfile 在 userdata 固定 pinned 文件，extents 写入 ADT `/chosen/fuxi-root-extents`。

## 下一步（等用户决策）

1. 给 PAT（拿内核）—— 最快；
2. 或插手机（先验证 hello 通路，内核没到也能做）；
3. 内核到手后：mkpayload.py（内核+ADT+cmdline）→ loader-popsicle build.sh 真 payload → fastboot boot → iOS 锁屏。
