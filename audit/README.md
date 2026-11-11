# 驱动审计 · 索引

这六份报告是同一天（2026-11-11）对 26 个自研驱动做的一次只读全量审计。
方法：静态源码 + 反汇编内核 + 真机 DTS + klog 实测 + 本地 OTA 固件解包，
六路并行（一份串行主报告 + 五路子智能体分报告）。**除注明处外没有修改任何被审计文件。**

| 文件 | 内容 |
|---|---|
| `驱动审计-总报告-20261111.md` | 主报告。链接层 / 地址层 / 运行层四档分类、被推翻的旧结论、阻断修复优先级 |
| `gpu-审计-20261111.md` | `pandora_gpu.c`（166 KB）的 15 级初始化、CSFW/GPI 有无调用、DCVS 停在 222 MHz 的原因 |
| `touch-审计-20261111.md` | 四套触摸（`fuxi_touch` / `pandora_touch` / `popsicle_syna` / `popsicle_goodix`）+ `sweet_touch` + redwood 队列，谁在跑、有无冲突 |
| `fuxi_wlan-审计-20261111.md` | `fuxi_wlan.c`（105 KB）的 PCIe 主机桥能力矩阵，13 个窗口的地址来源 |
| `zero-审计-20261111.md` | audio / camera / fp / sensors / amp 五个「0 行日志」驱动：是门禁没开还是静默失败 |
| `需要手机什么-20261111.md` | 「只有真机才能提供」的数据清单，P0/P1/P2 分级，附可执行的 adb 命令 |

## 一句话结论

**26 个驱动全部真实编进了内核（`undefined` = 0、入口符号全 `S`），45/45 个 MMIO
基址命中真机 DTS —— 但「编进去」和「能用」是三件事：11 个实测跑通、3 个刚写完
没上机、12 个只有门禁或只读巡检、以及 4 类依赖在树里根本不存在，补不掉。**

## 三条会反复咬人的教训

1. **`bsd/conf/files` 里列了 ≠ CI 会编它。** 11-03 那批 CI 对象里
   `pandora_bt/modem/thermal/sensors/idle/mem` + 全部 `popsicle_*` 是 ABSENT，
   `qcom/` 子目录整个不存在，`sweet_ufs.o` 还是旧版 —— 六个驱动因此白等了八天。
   建议在 CI 加一道门禁：反汇编 `_sweet_ufs_init` 提 `bl` 列表，与 `conf/files` 交叉核对。
2. **旧的 `.o` 比源码老，症状看起来像驱动写错。** AMFI 那次
   （`mac_base.o` / `mac_vfs.o` 缺 0336/0337）和 `sweet_ufs.o` 那次是同一个病根。
   链接前应先比对 mtime。
3. **C 符号带前导下划线。** `grep -w <name>` 会漏，得出「入口是 DEAD」的假阳性。
   用 `llvm-nm-18` 直接看。
