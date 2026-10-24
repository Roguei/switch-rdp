# 06 — 排错手册

出问题的第一动作：**先看日志**。

- PC harness：直接 stdout（`| tail -40`）
- Switch / 模拟器：`sd:/switch_rdp_log.txt`

日志一行一个阶段（`stage_cb` → `tlog`），最后一行就是现场。

---

## 1. 症状 → 原因 → 修法

### 1.1 连不到 / 立刻断

| 日志现象 | 原因 | 修法 |
|---|---|---|
| `TCP connect failed (errno 111/61)` | 服务器没开机 / 端口错 | 先 `Test-NetConnection 192.168.1.10 -Port 3389` |
| `x224_connect` 之后 `server refused negotiation` | 服务器强制 NLA，我们 offer 里没给 HYBRID | offer 必须 `0x0B` |
| TLS 握手失败 `MBEDTLS_ERR_SSL_...` | mbedTLS 配置/熵源问题 | 确认用的是自带的 `rdp_rng`，不要用 `mbedtls_entropy` |
| TLS 握手**成功**但立刻 `PEER_CLOSE_NOTIFY` | 服务器不喜欢我们的 MCS Connect Initial | 见 1.2 |
| `NLA result != 0` | CredSSP 失败 | `RDP_NLA_DEBUG=1` 打十六进制，和 Python 版逐字节比 |

### 1.2 MCS 阶段断线（最难的一类，全是"静默"的）

| 日志现象 | 原因 |
|---|---|
| `MCS connect-initial` 发出后服务器 TLS close | ① CS_CORE 里 `serverSelectedProtocol` 没填；② BER INTEGER 没补符号字节；③ GCC 块顺序错；④ channel options 不是 `0x80800000` |
| attach confirm 读不到 | AttachUserRequest 类型写错（必须是 `0x28`，不是 `0x08`） |
| join 回复全是**同样的 9~11 字节** | initiator 用了通道号而不是原始 userId；或 join 类型不是 `0x38` |
| join 都成功，但发完 Client Info 服务器**一个字节都不回** | 还是 initiator 错（`user_channel - 1002` 这种），服务器把 Client Info 丢了 |

**判定 initiator 是否正确**：`RDP_DUMP=1`，看
`MCS attached, user channel 1009 (initiator 8, reply 7 bytes)` —— `initiator` 必须等于
服务器 Attach Confirm 里的原始 userId（本例 8）。

### 1.3 连上了但没画面

| 日志现象 | 原因 | 修法 |
|---|---|---|
| 卡在 `client info sent` 之后 | 服务器没发 Demand Active | 见 1.2 最后一行 |
| `demand active` 之后没有 `confirm active` | 解析 `lsd/lcc/numberCapabilities` 偏移错 | 见 02_PROTOCOL §6 |
| 有 `frame N` 但 `incoming=0` | 服务器在等 finalization | 检查 5 个 PDU 是否都发了、`uncompressedLength` 是否 >0 |
| `SET_ERROR_INFO 0x000010EA` | Confirm Active 里某个 capset 声称的能力"太少" | 常见：`orderFlags` 被清零（只清 `orderSupport`）、关闭位图压缩、Input capset 写成 8 字节 |
| 有画面但**撕裂/镜像** | Planar 上屏翻转错 | `blit()` 用 `dy = dt + y` |
| 有画面但**窗口花屏/重影** | Order capset 的 `orderSupport` 没清零 | 清零，但保留 `orderFlags` |
| 画面偶尔有**残影** | Surface Commands（code 4）未实现 | 见 05_ROADMAP P1-3 |

### 1.4 键盘/鼠标不生效

| 现象 | 原因 |
|---|---|
| 按键完全没反应，也没有断线 | 走的是慢速输入 PDU（`0x1C`）—— 这台服务器**完全忽略**它，必须用 FastPath |
| 发送后服务器立刻断开（SIGPIPE / rc=141） | FastPath `eventCode` 写错（键盘=0、鼠标=1），服务器解析失败 |
| 鼠标移动有效，点击无效 | 按键事件没带当前坐标（给了 0,0），Windows 把指针拉到左上角后点击 |
| 点击有效但"没反应" | 目标窗口不在前台：Windows 的首次点击只激活窗口 |

**判定方法**：`RDP_DUMP2=1` 打印我们发出的输入 PDU：

```
CLI body type2=0x1C ...     ← 慢速（本服务器无效）
```

FastPath 输入不打日志（它是裸载荷），需要抓包或对照 02_PROTOCOL §11 的基准字节。

### 1.5 Switch 侧专属

| 现象 | 原因 | 修法 |
|---|---|---|
| 模拟器里绿屏后立刻崩（`Invalid memory access at 0x0`） | `consoleExit()` 之后仍有 printf/consoleUpdate | `rdp_console_live = 0`（已修，别再破坏） |
| 崩溃地址能反查到 libnx `console_sw.c` | 同上 | 同上 |
| 黑屏无日志 | `sdmc` 没挂载 → `tlog_open` 失败 | 检查 `fsdevMountSdmc()` 返回值；日志第一行会打印 |
| 日志停在 `nifm 0x...` 非 0 | 网络服务初始化失败 | 实机要保证有网络配置（不需要能上网，但要有一条连接） |
| 画面卡顿 | 每帧全屏 memcpy | 见 05_ROADMAP P0-3 |

### 1.5b 真机专属：一启动就崩（2168-0002）

| 现场 | 原因 | 修法 |
|---|---|---|
| 报错屏 `Error Code: 2168-0002 (0x4a8)`，`Program: 010000000000100D`，Backtrace 里有 `_dynProcessRela` | ELF 里有**指向只读段的重定位**（mbedTLS 或本工程没按 PIC 编译） | CFLAGS 加 `-fPIE`；mbedTLS 用 `tools/build_mbedtls.py` 重编（自带 `-fPIC`）；链接用 `nx/switch.specs`（`-z text` 会在链接期报错）。自查 `python tools/rela_scan.py build/switch_rdp.elf` 应为 0 |
| 同上但 reloc 自查是 0 | 看 PC 偏移：`PC - 模块基址` 后用 `addr2line` 反查 | `aarch64-none-elf-addr2line -f -C -e build/switch_rdp.elf 0x<偏移>` |
| 模拟器能跑、真机崩 | 模拟器把所有段映射成可写，掩盖只读段重定位 | 同上，以 `rela_scan.py` / `-z text` 为准，别信模拟器 |

### 1.6 构建

| 现象 | 原因 |
|---|---|
| `missing separator` | Makefile 被写进了 BOM |
| `Error occurred while processing: build` | recipe 里用了 `mkdir -p`（走 cmd.exe） |
| `undefined reference to mbedtls_*` | `third_party/mbedtls-install/lib` 路径或库名不对 |
| 链接后跑起来立刻崩 | `switch_fixed.specs` 没带（LDFLAGS） |

## 2. 调试配方

### 2.1 反查崩溃地址

```powershell
& C:\devkitPro\devkitA64\bin\aarch64-none-elf-addr2line.exe -f -C -e build\switch_rdp.elf 0x23670
# 输出示例：framebufferBegin  C:/devkitPro/libnx/nx/source/display/framebuffer.c:156
```

模拟器崩溃日志里的 `switch_rdp:0xNNNNN` 就是 ELF vaddr（基址 0x8500000）。

### 2.2 看服务器的 PDU 流

```powershell
RDP_DUMP=1  ./host_rdp ...      # PDU 级 + capset 列表 + join 回复
RDP_DUMP2=1 ./host_rdp ...      # TPKT 头、FastPath 头、数据 PDU 头字段、输入 body
```

### 2.3 看某个区域有没有被重绘

```powershell
RDP_UTRACE=1 RDP_INPUT_TEST=r ./host_rdp ...   # 每帧一行 u<N> t=<秒>
```

发 Refresh Rect 后应该 0.2 s 内出现密集更新。

### 2.4 和 Python 版对拍

```powershell
# Python：RDP_DUMP=1 打印 PDU/connect-response/client-info 的 hex
powershell -File tools\run_win.ps1 -Mode full -Secs 15
# C：同样的 RDP_DUMP 输出，逐行 diff
```

### 2.5 抓标准客户端字节

见 03_BUILD_TOOLING §5（xfreerdp + Xvfb + xdotool + SWRDPDUMP/SWRDPIN 补丁）。

### 2.6 确认是不是服务器侧的问题

用同一台机器上的 mstsc 连一次（`mstsc /v:192.168.1.10`）。mstsc 正常 → 服务器没问题。

## 3. 已知的"看起来像 bug 但不是"

| 现象 | 解释 |
|---|---|
| `SET_ERROR_INFO 0x00000000` | 正常（无错误） |
| 服务器先发一个 20 字节的 `80 00 00 00 ...` | License PDU，本该忽略 |
| 会话里 `rsa_bits = 0` | 走 TLS，不需要密钥交换参数 |
| 画面里任务栏时钟在动，但壁纸区域从不重绘 | 正常，Windows 只发变化区域 |
| 断开后 host 版 rc=141 | SIGPIPE，见 05_ROADMAP P1-1 |

### 1.7 b23 新遇到的三个"假死"

**症状：桌面里随便按一个键，画面和日志一起停住，几秒后仍不恢复。**
原因：`rdp_tls_write()` 里对 `MBEDTLS_ERR_SSL_WANT_WRITE` 无界 `continue`，
而 send 回调在 `poll(POLLOUT, 8000)` 失败时也返回 WANT_WRITE → 8 秒一轮的无限重试，
主循环完全停摆。修法：回调区分 errno（EPIPE/ECONNRESET 立即 -1），可写最多等
4×2 s；上层最多重试 8 次后返回 -1。

**症状：进菜单（L+R）待一会儿再回来，输入/画面都没反应。**
原因：菜单期间完全不读 socket，服务器发送窗口填满后就不读我们的输入，于是
客户端的写操作阻塞 —— 典型 TCP 死锁。修法：菜单循环每轮 pump（最多 16 次），
解码结果丢弃，回到桌面时全屏重绘。

**症状：崩溃后看 `sdmc:/switch_rdp_log.txt`，最后几行不见了。**
原因：每次启动 `fopen("w")` 截断，且崩溃会丢掉用户态缓冲。修法：改成 `a+`
追加 + 512 KB 轮转（`switch_rdp_log.old.txt`），每次启动写 `==== boot ====`。

**症状：Ryujinx 里点开 NRO 直接回游戏列表，日志有 `OpenNroAssetSection` 的
`NullReferenceException`。** 那不是我们的 bug，是新版 Ryujinx 要求 NRO 带
ASET 资源段。`toolchain/build_nro.py` 现在会自动追加（见 03 文档第 6 节）。

## 4. USB 键盘在桌面里没有反应（b41 实测：一个事件都没有）

现象：b41 在桌面按 USB 键盘，日志里 **0 条** `kbd usage=`。这不是映射表的问题
（映射错会打出 `usage=` 但 `sc=0000`），而是 HID 侧根本没有把键盘状态交给我们。

b42 加了三个诊断点：

1. 启动时一行
   `applet type=%d focus=%d aruid=%016llx kbd_count=%d`
   —— 当前是 applet 还是 application、有没有焦点、aruid、键盘 lifo 里已有几条。
2. 每 5 秒一行
   `kbd states=%d ev=%llu mods=%016llX keys=%016llX`
   —— `states` 是 `hidGetKeyboardStates()` 的返回值：
   **0 = lifo 里一条状态都没有**（系统没把它当键盘，或者没路由给这个 aruid），
   1 = 有状态（这时 `keys` 就能看出按了什么）。
3. 前 24 个键事件 `kbd usage=… sc=… ext=… down=…`（验证映射表本身）。

现场判读表：

| 日志 | 含义 | 下一步 |
|---|---|---|
| `states=0` 一直不变 | 系统没上报键盘 | 先按 Y 用系统键盘敲一下这个 USB 键盘；打得出来说明设备没问题，问题在路由（applet 模式） |
| `states=1`、`keys=0` | lifo 在，但设备没送键 | 换 USB 口 / 换键盘 / 确认不是 Hub |
| 有 `kbd usage=` 但 `sc=0000` | 映射表缺键 | 在 `rdp_scancode_from_hid()` 里补 |

两个二分试验：

1. **标题覆盖模式启动**：主菜单按住 `R` 点任意游戏图标 → 进 hbmenu → 选 switch_rdp。
   这时进程是 Application 而不是 LibraryApplet，日志里 `applet type` 会变；
   如果键盘这样就正常，那就是 applet 模式的输入路由限制，只能写进使用说明。
2. **系统键盘验证设备**：按 Y 打开系统键盘，用 USB 键盘打字。能出字 = 系统认得设备。

即使硬件键盘完全不通，文字输入仍然可用 `Y`（系统键盘，含中文），
方向键 / 回车 / Esc 在桌面里由十字键 / A / B 代劳。
