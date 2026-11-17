# wifi-extract — popsicle WiFi 真机提取物

来源：`adb -s 6f17201d`（Xiaomi 17 Pro Max / `popsicle` / SM8850 `canoe` / Android 17 SDK 37，KernelSU root），
提取时间 2026-11-12。所有文件都是**只读快照**，可重跑重取。

对应结论文档：[`../STATUS-20261117-WiFi真机数据提取与XNU适配.md`](../STATUS-20261117-WiFi真机数据提取与XNU适配.md)

## 目录

```
popsicle_wifi_names.h   C 常量表（166 个 #define）：PCI 身份、中断向量、电源轨、时钟、GPIO、
                        PCIe 六段寄存器窗、BHI/BHIE/MHI 寄存器偏移、固件投递参数、EE 状态机
tools/                  解析脚本（都可独立重跑）
  gen_header.py         从本目录的提取物重新生成 popsicle_wifi_names.h
  parse_cfg.py          解析端点 4 KiB PCI config space（需 ep-cfg.bin，见下）
  elfinfo.py            解析固件 ELF 头（amss20.bin 是两层 ELF，这是 M2 的关键证据）
  decode_cnss.py        解析解包后的 cnss-peach DT 节点（需 dtb 解包目录）
pci/                    PCI 侧快照
  ep-cfg-parsed.txt      端点 config space 结构化解析结果（能力链、MSI/MSI-X、PCIe cap、AER）
  pci-all-4k.txt         端点 4 KiB config space 十六进制全量
  pci-config-full.txt    lspci -xxx 输出
  pci-resource.txt       BAR/资源分配
  pci-modalias.txt / pci-uevent.txt / pci-vv.txt / pci-00-00-0-4k.txt / pci-01-00-0-4k.txt
state/                  运行态快照
  clk_summary / regulator_summary     时钟树与电源轨（需先 mount debugfs）
  interrupts.txt        /proc/interrupts（MSI-X 向量↔用途对照的来源）
  iomem.txt / cmdline.txt / socinfo.txt / board-id.txt
  tlmm-pinmux-pins.txt / tlmm-pinconf-pins.txt / pmh0104-*.txt   pinctrl 实况
  mhi-list.txt / mhi-states.txt / mhi-detail.txt   MHI 设备枚举
  cnss-cma.txt / msi-irqs.txt
logs/
  dmesg-wifi.txt        完整 dmesg 里与 wifi/pcie/mhi/cnss/qmi 相关的 435 行（原始 3994 行未入库）
```

## 不入库的部分（体积大 / 可复现）

| 内容 | 体积 | 取法 |
|---|---|---|
| `firmware/peach/`（100 个文件，含 `amss20.bin` 7.3 MB、`bd_p2.elf`、`regdb_xiaomi.bin`、18 个 BDF 变体） | 21 MB | `/vendor/firmware_mnt/image/peach/` 整目录 `adb pull` |
| `dt/fdt.dtb` | 1.8 MB | `adb shell su -c 'cat /sys/firmware/fdt'`（真机 live FDT） |
| `modules/`（`cnss2.ko` / `mhi.ko` / `qrtr-mhi.ko` / `wlan_firmware_service.ko`） | 1.5 MB | `/vendor/lib/modules/` 经 `su -c cp` 到 `/data/local/tmp/` 再 pull |
| `pci/ep-cfg.bin`、`pci/*.cfg` | 各 4 KiB | `adb shell su -c 'dd if=/sys/bus/pci/devices/0000:01:00.0/config of=/data/local/tmp/ep-cfg.bin bs=4096 count=1'` |
| `logs/dmesg.txt`（完整 3994 行） | 519 KB | `adb shell su -c dmesg`（本目录只保留 wifi 过滤后的 435 行） |

## 重跑脚本

```bash
# 1) config space 解析（需要 ep-cfg.bin）
python tools/parse_cfg.py ep-cfg.bin

# 2) 固件 ELF 头（需要 amss20.bin / bd_p2.elf / phy_ucode20.elf）
python tools/elfinfo.py amss20.bin bd_p2.elf phy_ucode20.elf

# 3) cnss-peach DT 节点（先把 fdt.dtb 用 dtc -I dtb -O dts 反编译，或解包成目录树）
python tools/decode_cnss.py <解包目录>/soc/qcom,cnss-peach@b0000000

# 4) 重新生成 C 常量表
python tools/gen_header.py && mv popsicle_wifi_names.h .
```

## 几个采集时的坑（重跑会踩到）

1. **MSYS 会改写设备路径**：`adb shell su -c 'cat /data/local/tmp/x'` 在 Git Bash 里必须
   `export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'`，否则 `/data/...` 被转成 `C:/Users/.../data/...`，
   报 `remote secure_mkdirs() failed`。
2. **debugfs 会自己掉挂**：抓 `regulator_summary` / `clk_summary` / pinctrl 前先确认，
   掉了就 `mount -t debugfs none /sys/kernel/debug`。
3. **`/dev/mem` 不可用**：本机内核无 `CONFIG_DEVMEM`，`devmem`/`dd /dev/mem` 全部失败 ——
   寄存器只能通过内核驱动或 `devinfo` 读，用户态直接读 `resource0`/`resource2` 是 `I/O error`
   （iATU 出站窗口未配 + BAR 未映射）。
4. **MSI-X 的 IRQ 号每次开机都变，向量号固定**：比对两次采样，`msi_irqs` 从 382–413 变成 363–394，
   但 vec0=BHI / vec1-2=MHI / vec4-11=CE1..8 / vec13=wake / vec14-31=DP0..17 的映射一直一致。
   **记录时只记向量号。**
