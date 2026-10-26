# popsicle iOS port

把 **Xiaomi 17 Pro Max（`popsicle` / 2509FPN0BC / SM8850 `canoe`）** 通过
[ios-pandora](https://github.com/ShueiTsuki/data-a-live) 路线启动 **iOS 16.7.16（20H392）**
的工作区：文档、驱动源码、构建与分析工具链。

**全程只用 `fastboot boot` 内存启动，不写任何分区。**

## 当前状态

| 里程碑 | 状态 |
|---|---|
| 内核启动到 `exec launchd` | ✅ |
| bypass AMFI / CT policy / dyld 缓存签名 | ✅ |
| `Early boot complete` | ✅ |
| SpringBoard 起来 + 锁屏 + 设置向导 | ✅ |
| 物理按键（电源 / 音量±） | ✅ |
| 触摸（Synaptics S3910P，`pandora_touch=0xf`） | ✅ |
| 刷新率上限 60 Hz | ✅ |
| **GPU 提交（Metal → AGX）** | ✅ `PANDORA GPU SUBMIT 1000 submissions avg 41 us`，DCVS busy 0%→17% |
| 桌面切换不闪屏 | ✅ 深帧缓冲 `b3a8` 被 nocopy 转成 `fmt 204` |
| SpringBoard 完全启动 / 应用可用 | ❌ fence 回收异常（7001+ 次 timeout）+ 464 次 CRASH |
| WiFi / 基带 | ❌ 内核无对应驱动 |

## 目录

```
*.md                     进度与结论文档（30 篇，建议先读 STATUS-进度总结）
popsicle_*.c             驱动源码：audio / camera / fp / goodix / modem / syna / wifi
dts/                     设备树（merged dtb / overlay / canoe base）
boots/                   真机验证过的 fastboot boot 镜像（.img.gz）+ 里程碑索引
UniXNU-popsicle/         另一条路线（UEFI + BootShim）的板级包
tools/                   构建、FDT 合并、固件解析脚本
panels/                  面板初始化序列
popsiclePkg/             空占位（实际包在 UniXNU-popsicle/firmware 下）
```

## 不入库的内容

`extract/`、`artifacts/`、`fw/`、`_checktest/` 都是可复现的提取物与构建产物，
分别从 Apple IPSW、小米官方包、EDK2 固件卷重新生成即可，故被 `.gitignore` 排除。

需要时从 Release 下载打包好的 `popsicle-assets-*.tar.zst`。

## 关键文档索引

| 文档 | 内容 |
|---|---|
| `STATUS-进度总结-20261025.md` | 全阶段进度总览 |
| `全量审查-20261025.md` | 29 项 bug / 缺陷逐条审查 |
| `项目交接文档.md` | 从零接手所需的全部环境事实 |
| `CT放行与ct1构建-20261026.md` | CT policy 拒绝分支的精确补丁点 |
| `GPU提交打通-20261026.md` | GPU 首次真正提交：confstr 空目录根因与两处插件机器码 |
| `死锁收口与dyld补丁-20261025.md` | lsd / SpringBoard 冻结链与三处 dyld 补丁 |
| `gpuplk-三连实验-20261025.md` | GPU 三轮上机实验记录 |
| `手机实测SOP.md` | 上机操作标准流程 |
| `触控移植笔记.md` | S3910P 触摸从 stopped 到 running |

## 环境要点

- 构建在 WSL（Ubuntu 24.04）+ GitHub Actions（`macos-14`）上完成
- 设备侧 adb 命令一律走主机侧；`fastboot` 必须在 WSL 侧跑
- `pinfile` 在 `su` 的 PATH 里不存在，必须用绝对路径 `/data/local/tmp/pinfile`
- `usbipd` 的 BUSID 会变（历史上出现过 `4-2` 与 `5-2`），脚本必须动态取

## 免责

仅用于自有设备的移植研究。所有 iOS 组件版权归 Apple Inc.，本项目不分发任何 Apple 二进制。
