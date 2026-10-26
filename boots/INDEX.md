# Boot images

每个镜像都是 `fastboot boot` 用的内存启动镜像，**不写任何分区**。
全部经过真机验证（Xiaomi 17 Pro Max，ADT 几何 1200x2608/4800）。

解压：`gzip -d loader-ct3-boot.img.gz`，然后 `fastboot boot loader-ct3-boot.img`。

## 共同 boot-args 前缀

```
debug=0x8 serial=3 cpus=8 keepsyms=1 -noprogress rd=ufsroot0 ufsrw=1 rootrw=1
```

各镜像在此基础上追加自己的开关（见下表）。

## 里程碑

| 镜像 | 大小 | sha256 (前 16) | 段数 | 里程碑 | 真机验证结论 |
|---|---|---|---|---|---|
| `loader-ct1-boot.img.gz` | 6.4 MB | `df1625dc884a4e0a` | 16 | CT policy 放行（四处补丁 R1/R2/B/C） | 首次 Early boot complete；SpringBoard / lsd / containermanagerd 全部 exec；无 initproc failed |
| `loader-ct2-boot.img.gz` | 6.4 MB | `45e2a82e680bbfaf` | 16 | ct1 + P5（SE0 初始化失败也继续） | 触摸仍 stopped —— pandora_touch=1 只给 bit0，证明是位掩码 |
| `loader-ct3-boot.img.gz` | 6.4 MB | `05b3e6de15042f83` | 16 | ct2 + pandora_touch=0xf（全四位） | ★ 触摸通了：TouchComm v1 identify，S3910P，running polling GPIO 158，SWEET HID touch down/up |
| `loader-ct4-boot.img.gz` | 6.4 MB | `9c4dde0a7b71de65` | 16 | ct3 + sweetfb_hz=60 | 刷新率上限升到 60 Hz（present up to 60 Hz every 1 ticks） |
| `loader-p6-boot.img.gz` | 6.4 MB | `f50941f0c7e1dc5c` | 16 | ct4 + P6 GPU 留痕（externalMethod ENTRY / PROBE cave） | PANDORA GPU ENTRY 与 PROBE 均 0 次 → externalMethod 从未被调用；插件探完三个子类型后放弃 |
| `loader-ct5-boot.img.gz` | 6.4 MB | `1745d730ad2db289` | 16 | ct4 + 插件 `clientDevice` 两处机器码（`0x5bc4` nop、`0x5bcc` 换成 `/private/var/tmp/`）+ 新 trust cache | ★★ GPU 真正提交：`PANDORA GPU SUBMIT 1000 submissions avg 41 us`，DCVS busy 0%→17%，`WATCH submit 1325 / fence 1328`，Metal draw/pipe/tex/viewport 全跑；闪屏消失 |

## 完整 sha256（原始 img，非 .gz）

```
df1625dc884a4e0ae44444f3547ce7a91e7b7c01671b938e13a123597b2768c8  loader-ct1-boot.img
45e2a82e680bbfaf93d69073c37ff23993c836469759335ebd74024f65c041f0  loader-ct2-boot.img
05b3e6de15042f83c6f3bb606af0166a8768e36e67cc716c2804e962aebe85b4  loader-ct3-boot.img
9c4dde0a7b71de65fcf05d5b24748e9711ad446573c9b7b480173b44c43adca2  loader-ct4-boot.img
f50941f0c7e1dc5c6be5064a15f3cdb746c8a81686f8f144177db116ee4feef8  loader-p6-boot.img
1745d730ad2db28989a39db60fbf6640bdf47d06733d03876d776e497b7d4cf7  loader-ct5-boot.img
```

## 关键 cmdline 差异

| 镜像 | 差异项 |
|---|---|
| ct1 / ct2 | `pandora_touch=1`（只 probe） |
| ct3 | `pandora_touch=0xf`（probe + L4I_E0 + run + THP） |
| ct4 | ct3 + `sweetfb_hz=60` |
| p6 | ct4 + 内核里 P6 两处 cave（不影响 boot-arg） |
| ct5 | p6 同 cmdline；差异全在 rootfs 与插件，不在 boot-arg |

## 共同的内核补丁

| 标签 | VA | 作用 |
|---|---|---|
| R1 | `0xfffffff0047057b8` | `stp`→`b`，跳过 AMFI CT policy 拒绝打印 |
| R2 | `0xfffffff0047057dc` | `stp`→`b`，跳过 devsig 拒绝 |
| B  | `0xfffffff00780b98c` | `cbz`→`b`，`_ubc_cs_blob_add` 忽略签名校验返回值 |
| C  | `0xfffffff00780dc14` | `b.eq`→`b`，`_ubc_cs_blob_revalidate` 同上 |
| P5 | `0xfffffff007474b94` | `cbnz`→`nop`，SE0 初始化失败也继续 |
| P6a | `0xfffffff007a37ff0` | 跳 CAVE_B，打印 `PANDORA GPU ENTRY` |
| P6b | `0xfffffff007a38048` | 跳 CAVE_A，打印 `PANDORA GPU PROBE` |

## ct5 的插件补丁（不在内核里，在 rootfs 的 `System/Library/Extensions/NativeAdrenoMetal.bundle/NativeAdrenoMetal`）

| 站点 | 原始 | 新值 | 作用 |
|---|---|---|---|
| `0x5bc4` | `0x34000228` `cbz w8,0x5c08` | `0xd503201f` `nop` | `confstr(_CS_DARWIN_USER_TEMP_DIR)` 返回空串时不再放弃 |
| `0x5bcc` | `0x9122bc21` `add x1,x1,#0x8af`（"m2v"） | `0x9121d021` `add x1,x1,#0x874`（"/private/var/tmp/"） | 目录槽从空串变绝对路径，最终拼成 `/private/var/tmp/m2v` |

改完必须同步：
- CodeDirectory page 5 哈希槽 `0x14d628d`（= `CD_abs 0x14d6114` + `hashOff 217` + `5*32`）
- cdhash `4ba3d4ec…` → `446897ac…`，写进 `pandora-native-gpu.tc`

配套 rootfs 文件（缺一不可）：
- `System/Library/Extensions/NativeAdrenoMetal.clients` = `backboardd\n`（**不是**放在 `.bundle/` 里）
- `System/Library/Extensions/NativeAdrenoMetal.publish` **必须不存在**
- `/private/var/mobile/Library/Preferences/com.apple.backboardd.plist` 三键全 `False`（`CAEnableDeepFramebuffer` / `CAEnableFramebufferCompression` / `CAEnableIOSurfaceCompression`）
