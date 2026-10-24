# 07 — Python 原型（`tools/rdp_client.py`）使用与差异说明

> 这份 1661 行的脚本是**先打通协议、再移植到 C** 的那一版。它现在仍然是
> "协议流程参考实现"，但**不是** C 版的标准答案 —— 两者在能力集选择上有差别。
> 用途：核对握手流程、看某个 PDU 的期望形态、快速试验服务器行为。

## 1. 运行

```powershell
# 默认参数都在 run_win.ps1 里（连 192.168.1.10，RDP_SEC=0 → 不带安全头）
powershell -File <repo>\tools\run_win.ps1 -Mode full -Secs 15

# 直接跑（自己指定环境变量）
$env:RDP_HOST="192.168.1.10"; $env:RDP_USER="user"; $env:RDP_PASS="password"
& python <repo>\tools\rdp_client.py
```

依赖：`paramiko`、`cryptography`（都装了）。

产物：PNG（路径在 `Client.save_png()` 里，工作目录下 `out_win.png`）。

## 2. `RDP_MODE`

| 值 | 行为 |
|---|---|
| `full`（默认） | 完整：Confirm Active → finalization → loop，最后存图 |
| `wait` | 发完 Client Info 就只收不发（用来看服务器第一反应） |
| `confirm` | 只发 Confirm Active，不发 finalization |
| `dump` | finalization → probe → 原始字节转录 |
| `paced` | 慢速发 finalization（排查时序） |
| `react` | 响应式（服务器要什么给什么） |

## 3. 环境变量速查

### 连接

| 变量 | 默认 | 说明 |
|---|---|---|
| `RDP_HOST` / `RDP_PORT` | 192.168.1.10 / 3389 | 服务器 |
| `RDP_USER` / `RDP_PASS` | user / password | 凭证 |
| `RDP_SECS` | 25 | loop 持续秒数（收图时间） |

### 安全头 / 通道

| 变量 | 默认 | 说明 |
|---|---|---|
| `RDP_SEC` | `0` | `1` = 上/下行都带 4 字节安全头；`0` = 不带（**本服务器用 0**） |
| `RDP_TXCHAN` | `global` | 数据 PDU 发在 `global`(1003) 还是 `user`(1009) 通道 |
| `RDP_PSRC` | — | `03ea` = pduSource 固定 0x03EA（实验） |
| `RDP_STREAM` | `1` | 共享数据头的 streamId |
| `RDP_HDR` | `after` | `pduType2` 放在 compressedType 之前(`after`)还是之后(`spec`) |

### 能力集（Confirm Active）

| 变量 | 默认 | 说明 |
|---|---|---|
| `RDP_CAPS` | `echo`（run_win 里） | `echo` = 回显服务器 capset；`built` = 自己构造 14 个标准 capset |
| `RDP_SKIP` | `0x001D` | 回显时丢掉的 capset 类型（逗号分隔） |
| `RDP_DROP` | — | `built` 模式下丢掉的类型 |
| `RDP_EXTRA` | `0` | `1` = 额外加 6 个客户端专属 capset |
| `RDP_SORTCAPS` | `0` | `1` = capset 按类型升序排列 |
| `RDP_ECHOORDER` | `0` | `1` = 用服务器的 Order capset（stub） |
| `RDP_ORDERFIX` | `0` | `1` = 发完整 88 字节 Order capset，`orderSupport` 取 `RDP_ORDERSUP` |
| `RDP_ORDERSUP` | `0xFF` | 上面那个 orderSupport 的填充值 |
| `RDP_BMFIX` | `0` | `1` = 强制改写 Bitmap capset 的 bpp/宽高 |
| `RDP_BMCOMP` | `1` | `0` = 关闭位图压缩（**会让服务器 0x10EA 断线，别用**） |
| `RDP_BPP` | `24` | 请求的色深（CS_CORE） |
| `RDP_CAPFLAGS` | `0x0001` | CS_CORE 的 `earlyCapabilityFlags` |
| `RDP_SRC` | `rdp` | 源描述符：`rdp` → `"RDP\0"` |

### 流程开关

| 变量 | 默认 | 说明 |
|---|---|---|
| `RDP_FINAL` | `1` | 是否发 5 个 finalization PDU |
| `RDP_PROBE` | `1` | 是否发 Refresh Rect + 鼠标移动（触发重绘） |
| `RDP_NOFASTPATH` | `0` | `1` = 在 General capset 里关掉 FastPath 输出 |
| `RDP_NOCHAN` | `0` | `1` = 不请求虚拟通道 |
| `RDP_NODVC` | `1` | `1` = 不做 drdynvc 初始化 |

### 调试输出

| 变量 | 说明 |
|---|---|
| `RDP_RAW` | `1` = 每个 MCS PDU 打印通道/长度 |
| `RDP_DUMPCAPS` | `1` = 打印 Demand Active 里每个 capset 的十六进制 |
| `RDP_TILEDUMP` | `1` = 把 tile 数据写 `tiles.bin`（配合 `tools/planar.py` 用） |

## 4. 与 C 版的差异（重要）

| 项 | Python | C（当前） |
|---|---|---|
| Planar 上屏 | `dy = dt + (h-1-y)`（因为 `planar.py` 返回 bottom-up） | `dy = dt + y`（`rdp_planar.c` 内部已翻转） |
| Order capset | 原样回显（窗口会花屏，早期没发现） | `orderSupport` 清零、`orderFlags` 保留 |
| 输入 | 慢速 `0x1C`，且 numberEvents/pad 位置写反了（**没验证过**） | FastPath，字节与 xfreerdp 抓包一致 |
| Client Info 安全头 | 固定带 `40 00 00 00` | 同 |
| finalization 的 `uncompressedLength` | body 长度 | body 长度 + 18（整包）；两者服务器都接受 |

> 结论：**协议流程/字段偏移以 Python 为准，能力集与输入编码以 C 为准。**

## 5. 其它 Python 工具

| 文件 | 用途 |
|---|---|
| `tools/planar.py` | Planar 解码器 Python 版（和 C 版做过逐字节对拍） |
| `tools/rlelib.py` | 早期 RLE 实验库（已证明不适用，留作参考） |
| `tools/rdp_mitm.py` | TLS 终止型 MITM（**带 NLA 时无法完成 CredSSP**，只能看握手前几帧） |
| `tools/rdp_probe.py` / `rdp_explore.py` / `rdp_full.py` | 早期探索脚本，已被 `rdp_client.py` 取代 |
| `tools/an_xf.py` / `an_cf.py` | 解析 FreeRDP 抓包日志（见 03） |
