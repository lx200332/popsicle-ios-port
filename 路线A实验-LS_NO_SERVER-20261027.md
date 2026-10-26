# 路线 A 实测记录：`LS_NO_SERVER=1` —— 失败，但锁定了闸门

> **一句话结论：路线 A 无效且有害。它把 `_LSDefaults+0x4c` 从 1 翻成 0，
> SpringBoard 立刻抛 `-[__NSSetM removeObject:]: object cannot be nil`，
> 陷入 54 次崩溃重启。已完整回滚。**
>
> **但这次失败有决定性的副产品：它证明 `_LSDefaults+0x4c` 确实是 SpringBoard
> 行为的关键闸门 —— 只是方向与变量名暗示的相反。**

日期：2026-10-27 00:23–01:00
上机镜像：`loader-ct5-boot.img` sha256 `1745d730ad2db28989a39db60fbf6640bdf47d06733d03876d776e497b7d4cf7`

---

## 1. 假设

`LS_NO_SERVER` 环境变量会让 LaunchServices 认为「没有服务端」，从而让客户端自己建库。
如果 SpringBoard 自己建库，就不再需要等 lsd，`ulock_wait` 卡点消失。

## 2. 做法

### 2.1 静态确认（改之前）

反汇编 `_LSDefaults` 的两处：

**设置侧** `0x181bf3a84`：
```
0x181bf3a84  adrp+add x0 = "LS_NO_SERVER"      ; 0x181d3f86b
0x181bf3a8c  bl   getenv
0x181bf3a90  cbz  x0, 0x181bf3aa8              ; 未设 → 查另一个变量
0x181bf3a94  adrp x8, #0x1dcdd2000
0x181bf3a98  mov  w9, #1
0x181bf3a9c  strb w9, [x8, #0x218]             ; 全局字节 [0x1dcdd2218] = 1
0x181bf3aa0  ret
```

**消费侧** `0x181bfbee4`：
```
0x181bfbee4  adrp x8, #0x1dcdd2000
0x181bfbee8  ldrb w8, [x8, #0x218]             ; 读全局字节
0x181bfbeec  mvn  w8, w8                       ; w8 = ~flag
0x181bfbef0  and  w8, w8, #1                   ; w8 = !flag
0x181bfbef4  strb w8, [x19, #0x4c]             ; _LSDefaults+0x4c = !flag
```

**⚠ 这里我犯了一个错**：`mvn` + `and #1` = **取反**。
- 未设变量 → `flag=0` → `+0x4c = **1**`
- 设 `LS_NO_SERVER=1` → `flag=1` → `+0x4c = **0**`

我当时把 `+0x4c` 理解成「isServer」并以为设变量会把它变 1，实际是把它变 0。
**这是一个「看对了指令、推错了语义」的典型错误。**

### 2.2 改动内容

两处（launchd 读缓存不读源 plist，必须同时改）：

| 文件 | 原 sha256 | 新 sha256 |
|---|---|---|
| `/System/Library/LaunchDaemons/com.apple.SpringBoard.plist` | `91a4f457…` | `de3cdcc3…` |
| `/System/Library/xpc/launchd.plist` | `026c4e99…` | `44baa3c3…` |

`EnvironmentVariables` 从
```
{CA_ASSERT_MAIN_THREAD_TRANSACTIONS: '1', LS_DATABASE_DIR: '/private/var/db/lsd'}
```
变成
```
{CA_ASSERT_MAIN_THREAD_TRANSACTIONS: '1', LS_DATABASE_DIR: '/private/var/db/lsd', LS_NO_SERVER: '1'}
```

**写入前的完整性门**（`sb_noserver.py`）：
- plistlib 二进制往返，源 plist `2344 → 2361 B`，缓存 `796182 → 796203 B`
- 逐键比对：源 plist 除 `EnvironmentVariables` 外所有键值不变
- 逐 job 比对：缓存 532 个 job 中只有 SpringBoard 那条的 `EnvironmentVariables` 变
- 顶层 6 个键全不变
- **两项都是 `往返完整性: OK`**

预检脚本 `preflight_sbA.py` 17 项 ALL PASS（含 dyld 三处补丁、插件两处补丁、
`.clients`/`.publish`/backboardd prefs、lsd plist 未被误动）。

镜像 sha256：`cd4b0945…`（ct5）→ `ed5b450f…`（+路线 A）

### 2.3 上机

```
00:30:14  push 16 GiB  352.5 MB/s / 46.5s  → PUSH OK
00:30:14  rawwrite all --yes  wrote 4194304 blocks  → RAWHASH OK
00:36:45  fastboot boot  Sending OKAY 0.920s / Booting OKAY 0.140s  rc=0
00:44:25  设备回 adb（约 460s 后）
```

## 3. 结果：彻底失败

### 3.1 判据统计（`plog-sbA-clean.txt`，27,100 行 / 1,773,053 B / `seq=318`）

| 判据 | 路线 A | ct5（对照） | 说明 |
|---|---|---|---|
| `exec SpringBoard` | **54** | 2 | **崩溃重启循环** |
| `CRASH SpringBoard` | **216** | 0 | 直接证据 |
| `CRASH`（全部） | 896 | 464 | — |
| `PANDORA GPU SUBMIT` | **0** | 有（1000 submissions） | 没有渲染需求，GPU 不提交 |
| `ulock_wait` | 0 | 有 | 原卡点被崩溃取代 |
| `DCVS: busy` | 39 | 19 | 有上报但无提交 |
| `LS_NO_SERVER` | **0** | 0 | 环境变量不会打印，正常 |
| `csstore` | 0 | 0 | 仍然没建库 |
| `exec lsd` | 1 | 1 | lsd 仍只启动一次 |
| `running, polling` | 1 | 1 | 触摸正常 |

### 3.2 ★ 崩溃原因（决定性）

```
|SpringBoard| 1969-12-31 16:02:18.-40 SpringBoard[26:259]
|SpringBoard| *** Terminating app due to uncaught exception 'NSInvalidArgumentException',
              reason: '*** -[__NSSetM removeObject:]: object cannot be nil'
*** First throw call stack:
(0x1b0ec940c 0x1aa1a5c28 0x1b10583fc 0x1b105e330 0x1b0ec9f50 0x1efb02730 0x1efb00760
 0x1ef8d213c 0x1ef9838c8 0x1cc75713c 0x1cc755288 0x1ef980198 0x1b7e2e4bc 0x1b7e275e8
 0x1ef980110 0x1ef97b7ec 0x1ef959ad4 0x1efe06ff0 0x1efa7f59c 0x1efa7e8ec 0x1efe07378
 0x1b2fbecdc 0x1b304bfb8 0x1b304bac0 0x1cc76d010 0x1c59eeefc 0x1cc73ebcc 0x1cc730
 0x1b7e29d70 0x1cc723c54 0x1cc71eef0 0x1cc742ba8 0x1cc72ddc0 0x1cc7687a4 0x1efb94da8
 0x1efb94a8c 0x1eff03628 0x1efac5b30 0x1efac5428 0x1eff034b8 0x1efb9502c 0x1efb948c0
 0x1eff3daf8 0x1eff3b2c8 0x1ef958cd8 0x1b31886a0 0x1b3188350 0x1b3186dd 0x1b31cd184
 0x1b31cc328 0x1b31cbfc4 0x1ef93d5e8 0x1ce6fc344)
|SpringBoard| libc++abi:
|SpringBoard| terminating due to uncaught exception of type NSException

CRASH SpringBoard[26] sig 6 code 0x0 sub 0x0 slide 0x2a0d8000 mh 0x10049f000
CRASH SpringBoard pc 0x1ec6ef198 lr 0x1fbc7e5f8 sp 0x16f95ca00 fp 0x16f95ca20 far 0x1b7d74d50
CRASH SpringBoard reason ns 2 code 0x6: (none)
```

- **`sig 6` = SIGABRT**，`reason ns 2 code 0x6` = `NSInvalidArgumentException`
- 这是 **Objective-C 异常**，不是之前的内存错误
- 216 次 CRASH 的 **pc 全部相同**（`0x1ec6ef198`），去 slide 后是 `0x1c2a1f198`
- 崩溃点 `far 0x1b7d74d50` 与 `bt[0] 0x1b7d344b8` 同在 `dyld_shared_cache` 的一个区

### 3.3 其他进程状态（说明系统本身是好的）

第二次采样（行 14462）：
```
SMP installd[213]   t0 pc 0x1ec6e9030 (getuid 桩)  bt ... 0x1b0f34148 0x1b0f352e0 0x1b0f39d20 0x1b0f78bec
SMP lsd[98]         t0 pc 0x1ec6e9030 (getuid 桩)  bt ... 0x1b0f34148 0x1b0f352e0 0x1b0f39d20 0x1b0f78bec 0x1abdc4cc0 0x1abdc4bf4 0x1ce6fc344
```

**lsd 和 installd 的主线程栈完全相同**（`0x1b0f34148` = CoreFoundation 的 CFRunLoop 链），
末端 `0x1abdc4cc0` / `0x1abdc4bf4` 正是 `_LSServerMain` 区。
→ **lsd 已经在 `_LSServerMain` 的 runloop 里，和 ct5 一模一样。路线 A 没有改变 lsd 的任何行为。**

th 3 采样时 lsdt3 的 `pc 0x0 lr 0x0 sp 0x0` 是线程正在退出（libdispatch 回收），正常。

## 4. 结论

### 4.1 路线 A 为什么失败

`LS_NO_SERVER=1` → `_LSDefaults+0x4c = 0` → SpringBoard 认为「无 LaunchServices 服务端」→
它在初始化时对某个 nil 集合调 `removeObject:` → 抛 `NSInvalidArgumentException` → SIGABRT → 重启 → 循环。

**54 次 exec 之间没有任何 `|SpringBoard|` 输出**，说明它连沙盒/图形环境都没进到，就在非常早的初始化里崩了。

### 4.2 ★ 但这次失败给出了三个新事实

1. **`_LSDefaults+0x4c` 确实是关键闸门** —— 一个字节就把 SpringBoard 从「死等」变成「立刻崩」。
2. **`+0x4c = 1` 是 SpringBoard 正常工作所必需的** —— 默认值不能动。
3. **崩溃点在 SpringBoard 的极早期初始化**（`bt` 链全是 SpringBoard.framework 内部 + `0x1ce6fc344` 收尾），
   而不是在 `CCSModuleRepository` / `SBApplication` 那条晚期的路上。

### 4.3 下一步该动什么（修正后的方向）

| 候选 | 理由 | 风险 |
|---|---|---|
| **`_LSDefaults+0x4b`** | `0x181c952e4 ldrb w8,[x8,#0x4b]` 在目录解析器里，决定走 `systemgroup.com.apple.lsd` 还是 TCC/BSupport 桩路径。它与 `+0x4c` 是两个独立字段。 | 中（需改 dyld + 重算页哈希） |
| **`_LSDefaults+0x4c` 改成 0 之外的值** | 已知 0 会崩、1 是默认，没有第三个有意义的取值 | — 放弃 |
| **让 lsd 走 `systemSessionDaemon`** | 10-25 已证伪（服务名 `.system` 前缀配不上），**不要重试** | — 放弃 |
| **直接给 SpringBoard 造一个 `.csstore`** | 需要知道格式，但可以从真机 dump 或其他 iOS 系统里找 | 中，但可能绕过整个 XPC 环节 |

**我倾向于先做最后一件事**：既然 `LIVE` 镜像（真机启动过的）里 `.csstore` 也是 0，说明这条路在本机
从未成功过；但如果能找到一个**现成的 `.csstore` 样本**（哪怕来自别的 iOS 16 设备），
放进 `~/Library/Caches/com.apple.LaunchServices-501-v2.csstore`，
SpringBoard 的 `_LSDatabaseCreateFromPersistentStoreIgnoringRecoveryFile` 就可能直接打开它而不是重建。

## 5. 回滚记录

```
00:5x  备份 <WORK>/bak_sbA/{SpringBoard.plist,launchd.plist}
       sha256 91a4f457… / 026c4e99…（与原始一致）
       cp 回两处 → REVERT OK（LS_NO_SERVER 消失，532 job 不变）
       恢复 ro 挂载
       镜像 sha256: ed5b450f… → 57799040…
01:00:14  push 16 GiB  366.7 MB/s / 44.7s  → PUSH OK
01:00:14  rawwrite all --yes  → RAWHASH OK（57799040…）
01:00:14  清 plog 头
01:00:20  fastboot boot  Sending OKAY 0.827s / Booting OKAY 0.140s  rc=0
```

预检复核：17/17 中 15 PASS，2 项 FAIL 是「路线 A 改动检查」反向剩余
（`LS_NO_SERVER` 不在 = 期望值），即回滚成功。dyld 三处补丁、插件两处补丁、
`.clients`/`.publish`/prefs 全部 PASS。

**镜像 `57799040…` = ct5 + 去掉路线 A**。这不是回到 `cd4b0945…`，
因为 `cd4b0945` 是推送 ct5 时的字节态，而 `57799040` 是「写完再删掉」的字节态。
两者在**内容上等价**（plist 恢复原文），但 **HFS+ 的 B-tree 结构可能不同**。
如需严格回到 `cd4b0945`，从 `<CI_CACHE>/stage/iosroot-tweak.img` 的旧副本恢复。

## 6. 本轮脚本

| 脚本 | 作用 |
|---|---|
| `sb_inspect.py` | 源 plist + 缓存里 SpringBoard job 全文 |
| `sb_cache1.py` | 只读缓存那一条（确认与源一致） |
| `sb_noserver.py` | 路线 A 主脚本（`--dry` / `--apply`），含往返完整性门 |
| `apply_sbA.sh` | rw 重挂 + 跑 sb_noserver + 恢复 ro + 复核 |
| `preflight_sbA.py` | 17 项上机前复核 |
| `push_sbA.sh` / `push_now.sh` | 推镜像（后者通用，从文件实算哈希） |
| `grab_sbA.sh` | 等设备 + 抓日志 + 路线 A 判据 |
| `revert_sbA.sh` | 从备份恢复两份 plist + 校验 |

## 7. 教训

1. **`mvn` + `and #1` 是取反，不是直传。** 看到 `ldrb` → `mvn` → `and` → `strb` 的链，
   实际语义是 `dst = !src`，变量名里的「NO」不提示方向。
2. **改 plist 类改动没有「试一下就知道」的余地** —— 一个字节让系统进不去桌面。
   必须（a）先备份到镜像外，（b）写前后做逐 job 比对，（c）准备一键回滚脚本。
3. **`sig 6` + `reason ns 2 code 0x6` = Objective-C 异常（SIGABRT）**，与内存错误（`sig 11`）要分开看。
   ObjC 异常会打印完整 `First throw call stack`，这比 CrashLog 更直接。
4. **`exec X` 次数是崩溃循环的强指标**：ct5 是 2 次（正常），路线 A 是 54 次。
5. **`ulock_wait` 计数归零不一定代表好转** —— 可能是进程根本没活到那一步。
