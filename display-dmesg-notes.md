# popsicle 显示 dmesg 事实摘要

材料：
- `D:\Backup\Downloads\666\tiqu\06-display-touch\dmesg-display.txt`（112,660 B，1007 行）→ 下文简称 **disp:N**（第 N 行）
- `D:\Backup\Downloads\666\tiqu\10-props-logs\dmesg-full.txt`（530,942 B）→ 下文简称 **full:N**（第 N 行）
- 日志窗口：dmesg-display 从 t=15.057s 开始（早期 probe/bring-up 段未捕获）

---

## 面板初始化事实（按时间顺序）

### 主屏 primary = `qcom,mdss_dsi_p2_42_0d_0b_mp_prim_dsc_cmd`

| 事实 | 证据 |
|---|---|
| 分辨率 1200×2608（hactive=1200, vactive=2608） | disp:22、24、82、97、337、703、955；full:759、766、845、916、1928、2472 |
| clk_rate = **1198000000**（1.198 GHz，非 1.1GHz 假设） | disp:22；full:759 |
| mdp_transfer_time = 7300 | 同上 |
| fps 60/120 双档切换，ddic_mode=normal，ddic_min_fps=60（60Hz 档）/ 120（120Hz 档） | disp:22（60fps）、24（120fps）、82、97、337 |
| cell id = <PANEL_ID_A>Q05EB1A | disp:184；full:1473（t=21.167786）|
| **t=54.909369 出现 ddic_mode=auto、ddic_min_fps=1**（异常档位，日志尾段仅此一次） | disp:955；full 尾段 |
| DSI ctrl 中断线：dsi-ctrl-0 → **IRQ 451** | disp:338；full:1929 |
| 上电时序：set_mode → isr_configure → panel_power_on「set gpio 171 high」→ DSI_CMD_SET_MI_ROUND_CORNER_ON → set 51 背光 0 → DSI_CMD_SET_ON → Power mode Doze → DSI_CMD_SET_LP1 | disp:337-346 |
| 下电：DSI_CMD_SET_OFF → panel_power_off「set gpio 171 low」 | disp:319-320（full:1894-1895）、662-663、694-695 |
| 背光「set 51」范围 987–6043（6043 为 HBM 级高值） | disp:27、841 |
| NOLP 命令 DSI_CMD_SET_NOLP 仅在 53.698s 出现一次（Doze→亮屏恢复） | full:2523 |

### 主屏 bring-up 关键序列（disp:337-346，t≈30.38-30.57s）

```
dsi_display_set_mode [primary] fps=120, ddic_mode=normal      (disp:337 / full:1928)
dsi_ctrl_isr_configure dsi-ctrl-0: IRQ 451 registered          (disp:338 / full:1929)
dsi_panel_power_on: set gpio 171 high                          (disp:339 / full:1930)
primary ddic round corner DSI_CMD_SET_MI_ROUND_CORNER_ON       (disp:341 / full:1932)
set 51 backlight 0                                             (disp:343)
primary panel: DSI_CMD_SET_ON                                  (disp:344 / full:1936)
Power mode Doze → primary panel: DSI_CMD_SET_LP1               (disp:345-346 / full:1938)
```

---

## 副屏

### 副屏 secondary = `qcom,mdss_dsi_p2_42_06_0a_sec_dsc_cmd`

| 事实 | 证据 |
|---|---|
| 分辨率 596×976（hactive=596, vactive=976） | disp:21、23、85、96、185、187、342、359、738；full:758、765、849、915、1611、1697、1934、1971、2534 |
| clk_rate = **553000000**（553 MHz） | disp:21；full:758 |
| mdp_transfer_time = 7300（与主屏同） | 同上 |
| ddic_mode=**auto**（常态）、ddic_min_fps=**1**（允许降到 1fps） | disp:21、23、85、96、185、187、359、738 |
| 例外：t=30.554s 一条 ddic_mode=**normal**、ddic_min_fps=120（与主屏 120 档同时切） | disp:342；full:1934 |
| cell id = A32556702KF91 | disp:358；full:1968（t=31.118532） |
| DSI ctrl 中断线：dsi-ctrl-1 → **IRQ 452** | disp:739；full:2535 |
| DSI_CMD_SET_LP2 仅副屏出现（主屏无 LP2） | disp:682、780 |
| 背光「set 51」：上限 1531，下限 100（100 为固定值反复设置） | disp:182 及多处 |

### 副屏 bring-up 关键序列（disp:738-762，t≈53.7-53.8s）

```
set_mode [secondary] fps=120, ddic_mode=auto                   (disp:738 / full:2534)
dsi_ctrl_isr_configure dsi-ctrl-1: IRQ 452 registered          (disp:739 / full:2535)
dsi_panel_power_on: set gpio 171 high                          (disp:740)
secondary ddic round corner feature not enabled                (disp:745)
secondary DSI_CMD_SET_MI_ROUND_CORNER_ON（仍尝试下发）          (disp:747)
DSI_CMD_SET_ON → Doze → DSI_CMD_SET_LP1                        (disp:748-751)
secondary backlight 24 → primary backlight 3394                (disp:760-761)
```

**关键发现：主副屏共用同一个上电 GPIO 171（disp:320/663/695/782 均为「set gpio 171 low」，339/740 为 high）。** 副屏 round corner 功能未启用但命令照发（disp:745/747）。

### 触控联动

- 主屏触控：goodix（touch id 0），副屏：synaptics TCM（touch id 1）
- disp:204：`syna_tcm_set_panel_notifier_status: second panel is off`——触控随面板 notifier 状态联动

---

## 报错与重试

| 报错 | 证据 | 严重性 |
|---|---|---|
| `[MI_TP_E][xiaomi_touch_set_mode_value:791]: touch id 0 don't not support mode 40` | disp:6（t=15.057343，日志首行） | 低（首帧触摸模式不支持） |
| `whether_backlight_zero = ture, timestamp=4294899722`（「ture」为内核原文拼写错误） | disp:286（t=29.707954） | 信息（timestamp 4294899722 为异常大值，疑似回绕） |
| 丢帧：`crtc218 drop frame type:REFRESH fps:120 consecutive:1 cost(us): ... Wait_for_commit_done=36363`（总 cost 42ms） | full:1444（t=21.107864） | 低（单帧 120Hz 下 commit 等待 36ms） |
| SELinux avc denied：joyose 读 `vendor_displayfeature_prop` 被拒（permissive=0） | disp:1005（t=62.936779）；full 对应段 | 低（非显示链路） |
| dmesg-display 全文 grep：`dsc` / `lane` / `error` / `fail` / `retry` / `timeout` **零命中** | — | 无面板层报错 |
| gunyah（虚拟化层）：`gunyah: RM rejected message 51000017. Error: 13`（仅 2 条） | full:1456、1500 | 低（RM 拒绝一条消息） |

---

## 其他子系统关键行（ufs/adreno/固件）

### UFS

| 事实 | 证据 |
|---|---|
| boot_completed 后 init 写 `1d84000.ufshc/clkscale_enable` 失败：`open() failed: Permission denied` | full:908 |
| 同样 `clkgate_enable` 写失败 Permission denied | full:910 |
| `ufs_ffu_service` oneshot 服务退出 status 1（被 SIGKILL） | full:956 |
| UFSSWAP lba range 日志 | full:1530-1532 |

### Adreno / KGSL

- **dmesg-full.txt 中 `adreno` / `kgsl` 关键词 0 命中**——该 dmesg 窗口完全没有 GPU 初始化行（GPU 在此日志窗口之外完成初始化，或日志被裁剪）。

### 固件加载（firmware）

| 事实 | 证据 |
|---|---|
| MHI PCIe `peach/amss20.bin`：首次 direct load 失败（-2），随后经 ueventd 路径成功 | full:263-267 |
| `peach/qdss_trace_config_perf_v2.cfg` 找不到（非致命） | full:360-363 |
| WiFi 平台：peach_v2（cnss-peach） | full 对应行 |
| 音频 codec 机器驱动：canoe-asoc-snd | full:1433 |
| KernelSU hook 存在（qrtr-lookup 相关日志） | full:936 |

### qrtr / smp2p / glink

- `qrtr-lookup` oneshot 退出 status 1（full:933-966 段），伴随 KernelSU hook 日志（full:936）；qrtr 首见 full:343。
- dmesg-full 中 `smem` / `smp2p` / `glink` / `pvmfw` / `hyp` 无实质内容（pvmfw 零命中；hyp/gunyah 见上文报错段）。

---

## 与「主屏 1200×2608 4-lane DSC cmd」假设的对比

**一致项：**
1. 分辨率 1200×2608 ✓（disp:22 等多处）
2. 副屏 596×976 ✓（disp:21 等多处）
3. cmd mode ✓（drm-card0-DSI-1/modes.txt：`1200x2608x120cmd`，DSC command mode）
4. 双屏独立 DSI ctrl：主屏 IRQ 451 / 副屏 IRQ 452 ✓

**矛盾/新增项：**
1. **clk_rate = 1,198,000,000 Hz（≈1.2GHz）**，高于 4-lane 1.1GHz 的假设值（1198MHz 是 bit clock 或 pixel clock 档位，需与 XML 生成脚本的 clock 参数对齐核对）。
2. **主副屏共用上电 GPIO 171**——XML 中若为两屏各写独立 gpio，需改为共用或确认板级映射。
3. **主屏尾段出现 ddic_mode=auto + ddic_min_fps=1**（disp:955，t=54.909369）——主屏常态为 normal/60 或 120，auto 档仅出现一次，可能是小米显示特性服务（displayfeature）动态切换，移植时需保留该模式支持。
4. dmesg 窗口未覆盖初始 panel probe（日志从 15.057s 开始），**无 lane 数打印、无 DSC 配置打印**——lane 数（4 lane）与 DSC 参数只能从 DTB/XML 侧确认，dmesg 无法交叉验证这两项。
5. 副屏无 LP2 之外主屏命令集更全（NOLP 仅主屏）；副屏 round corner 未启用但命令照发。
6. 背光：主屏可达 6043（HBM），副屏上限 1531、下限固定 100——两屏背光 scale 不同。
7. UFS clkscale/clkgate 写权限失败与 ufs_ffu_service 被杀，均为小米系统服务非面板问题，不影响显示移植；GPU 日志缺失说明 dmesg 窗口不含 GPU 段。
