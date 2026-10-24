# 04 — 验证流程与判定标准

> 原则：**任何改动先在 PC harness 上验证，再上 Switch**。Switch 侧只能看到日志和画面，
> 反馈周期以分钟计；PC 侧一次编译+运行 20 秒。

---

## 1. 分三层验证

| 层 | 在哪 | 能验证什么 | 周期 |
|---|---|---|---|
| L1 协议层 | Ubuntu 测试机 `host_rdp` | 握手、画面、键盘、鼠标 | ~20 s |
| L2 平台层 | Ryujinx | 帧缓冲、hid、SD 卡、libnx API | ~1 min |
| L3 实机 | Switch | USB 键盘、性能、稳定性 | 分钟级 |

## 2. L1：协议层验证（最常用）

### 2.1 画面

```powershell
# 12 秒，1280x720
python -c "import sys;sys.path.insert(0,r'<repo>\tools');import rsh;print(rsh.run('cd /tmp/rt && RDP_W=1280 RDP_H=720 timeout 40 ./host_rdp 192.168.1.10 user password 12 /tmp/rt/v.ppm 2>&1 | tail -5',120)[1])"
```

**判定**：日志出现 `[host] N updates, writing ...`，且 N 在几十以上；
PPM 转 PNG 后能看到 Windows 桌面（壁纸、图标、任务栏时钟）。

**看画面是否正确的三个"指纹"**（踩过的坑都能一眼看出）：

| 现象 | 含义 |
|---|---|
| 每条 64 行色带上下镜像 | Planar 上屏多做了一次垂直翻转 |
| 画面全黑/只有 0x202020 | 没有收到位图更新（看 `incoming=` 计数） |
| 窗口内容左右撕裂/重影 | Order capset 的 `orderSupport` 没清零，服务器在用绘图顺序 |

### 2.2 键盘

```powershell
# 等价于 run_input_test.py
cd /tmp/rt && RDP_INPUT_TEST=e RDP_W=1280 RDP_H=720 timeout 40 ./host_rdp 192.168.1.10 user password 12 /tmp/rt/k.ppm
```

**判定**：画面里出现资源管理器窗口（大量白色像素）。
`RDP_INPUT_TEST=k`（Windows 键 → 开始菜单）也可以。

### 2.3 鼠标

```powershell
cd /tmp/rt && RDP_INPUT_TEST=b ... /tmp/rt/m.ppm      # 双击回收站
```

**判定**：出现"回收站"窗口（标题栏 + "此文件夹为空"）。

> 注意：Windows 的"首次点击只激活窗口"行为会吃掉第一次点击。测试点击类功能时，
> 要么先点一次再点一次，要么确保目标窗口已是前台。

### 2.4 上行数据 PDU 是否被处理（Refresh Rect 探针）

```powershell
cd /tmp/rt && RDP_UTRACE=1 RDP_INPUT_TEST=r ... /tmp/rt/r.ppm
```

看 `[host] u<N> t=<秒>` 的时间戳：发 Refresh 之后 0.2 s 内应出现一串密集更新。
没有 → 上行共享数据头有问题（多半是 `uncompressedLength` 或 shareId）。

### 2.5 逐字节对拍（改协议代码后必做）

编译期把关键 PDU dump 出来，和已验证版本比：

```powershell
RDP_DUMP=1  # PDU 级
RDP_DUMP2=1 # 字段级
```

历史基准值（这台服务器）：

| 项 | 值 |
|---|---|
| X.224 请求 / 应答 | 19 / 19 字节，selected=0x08 |
| MCS Connect Initial | 451 字节 |
| Connect Response | 108 字节，io=1003，4 通道 |
| Attach User Confirm | 7 字节 `02f0802e000008` |
| Channel Join Confirm | 11 字节 `02f0803e00000803f103f1`（首个） |
| Client Info | 62 字节（+4 安全头 = 66） |
| Demand Active | 457 字节，shareId=0x000103EA |
| Confirm Active | 359 字节（335 capset + 24 头/描述符） |
| finalization | 22 / 26 / 26 / 42 / 26 字节 |

## 3. L2：模拟器验证

```powershell
# 1) 编译
<workdir>\toolchain\make.exe -j4

# 2) 确认 SD 卡上的配置和旧日志
Get-Content "<workdir>\emulators\Nextendo\portable\sdcard\switch_rdp.cfg"

# 3) 手动：模拟器 File → Load Application → build\switch_rdp.nro

# 4) 看日志（跑 20 秒后）
Get-Content "<workdir>\emulators\Nextendo\portable\sdcard\switch_rdp_log.txt"
```

**成功日志长这样**：

```
switch_rdp b16-fastpath boot (sdmc mount 0x0)
nifm 0x0
socket 0x0
cfg loaded (4 fields): 192.168.1.10 port=3389 user=user
auto-connect with stored credentials
TCP connected
X.224: NLA extended (HYBRID_EX)
TLS established
NLA result 0, continuing to MCS
MCS attached, user channel 1009 (initiator 8, reply 7 bytes)
channels joined (6)
client info sent
license pdu (20 bytes), ignored
demand active 457 bytes
confirm active: 335 capset bytes, 359 total
finalization sent
frame 1 (1280x720) incoming=0 rects=0
SRV synchronize target=1
SRV control COOPERATE (4)
SRV control GRANTED_CONTROL (2)
frame 2 (1280x720) incoming=1 rects=240
frame 50 (1280x720) incoming=49 rects=1821
```

判定标准：**出现 `frame N` 且 `rects` 持续增长**。看到 `frame 1` 就说明画面链路通了。

## 4. L3：实机验证清单

到货后按顺序做：

1. 把 `switch_rdp_b16.nro` 放到 `sd:/switch/`，写 `sd:/switch_rdp.cfg`。
2. 启动 → 看是否出现绿色（连上了）→ 然后是桌面。
3. 看 `sd:/switch_rdp_log.txt`：应该和上面模拟器日志一样，只是 `sdmc mount 0x0`。
4. **USB 键盘**：随便按，看远端是否响应（开个记事本打字最直观）。
   - 没反应：先确认 `hidInitializeKeyboard()` 成功；必要时加 HID 设备枚举日志
     （`hidEnumerateKeyboardDevices`）。
5. 摇杆当鼠标：右摇杆移动、ZR 左键、ZL 右键。
6. 性能观察：拖动窗口是否跟手；不跟手就上脏矩形（见 05_ROADMAP P0-3）。

## 5. 回归基线（改动后跑这一组）

```powershell
# 桌面画面
cd /tmp/rt && RDP_W=1280 RDP_H=720 timeout 40 ./host_rdp 192.168.1.10 user password 10 /tmp/rt/base.ppm
# 键盘
cd /tmp/rt && RDP_INPUT_TEST=e RDP_W=1280 RDP_H=720 timeout 40 ./host_rdp 192.168.1.10 user password 12 /tmp/rt/kbd.ppm
# 鼠标
cd /tmp/rt && RDP_INPUT_TEST=b RDP_W=1280 RDP_H=720 timeout 40 ./host_rdp 192.168.1.10 user password 10 /tmp/rt/mouse.ppm
```

三张图都符合预期，才算没退化。

## 6. 日志基准（docs/baselines）

`docs/baselines/host_run_b16.log` 是一次**成功运行**的完整 `RDP_DUMP=1 RDP_DUMP2=1` 日志
（649 行，26 KB）。改完协议代码后重新抓一份 diff 它，比自己盯着看快得多：

```powershell
# 新抓一份
python -c "import sys;sys.path.insert(0,'<repo>/tools');import rsh;print(rsh.run('cd /tmp/rt && RDP_DUMP=1 RDP_DUMP2=1 RDP_W=1280 RDP_H=720 timeout 40 ./host_rdp 192.168.1.10 user password 8 /tmp/rt/base.ppm > /tmp/rt/base.log 2>&1',120)[1])"
python -c "import sys;sys.path.insert(0,'<repo>/tools');import scp_get;scp_get.get('/tmp/rt/base.log','<repo>/out/new.log')"

# 只比"结构性"行（忽略计数/时间戳）
code-diff docs\baselines\host_run_b16.log out\new.log   # 或 fc / Beyond Compare
```

基准日志里的关键行（照这个顺序）：

```
MCS attached, user channel 1009 (initiator 8, reply 7 bytes)
  join 1009 (init 8) reply 11: 02f0803e00000803f103f1
client info sent
license pdu (20 bytes), ignored
demand active 457 bytes
confirm active: 335 capset bytes, 359 total
CLI body type2=0x1F total=22: 01000000
CLI body type2=0x14 total=26: 0400000000000000
CLI body type2=0x14 total=26: 0100000000000000
CLI body type2=0x2B total=42: 00000000000000000000000000000000
CLI body type2=0x27 total=26: 0000000003003200
finalization sent
SRV control COOPERATE (4)
SRV control GRANTED_CONTROL (2)
```

## 7. 截图基准（docs/images）

| 文件 | 含义 |
|---|---|
| `desktop_1280x720.png` | C 客户端渲染的 RDP 6 位图服务器（正确态） |
| `window_explorer.png` | Win+E 打开的资源管理器（键盘验证） |
| `window_recycle.png` | 双击回收站打开的窗口（鼠标验证） |
| `stale_titlebar.png` | 已知小瑕疵：旧标题栏残影（Surface commands 未实现） |

## 6. b23 的验证结果（2026-10-24，Ryujinx "Nextendo" 1.10.0）

### 6.1 已经验证到的

| 项 | 证据 |
|---|---|
| 启动、UI 渲染、cfg 模板生成/读取 | `shots/20_ui.png`，日志 `cfg written: ... user=Administrator` |
| 魔法代码直连（10 连击） | 日志 `konami -> quick connect` → `desktop loop start` |
| 真连上远端并出画面 | `shots/21_desktop.png`（远端 Edge 桌面 + 本地箭头指针） |
| L+R 打开会话菜单 | 日志 `menu opened`，`shots/22_menu.png` |
| 菜单上下选择 | 日志 `menu: alive sel=1`（DOWN 之后） |
| 菜单 → 设置页 | `shots/08_settings.png`（服务器/端口/账号/密码，含 `saved to switch_rdp.cfg`） |
| 菜单 → 继续 → 桌面完整恢复 | `shots/10_desktop_back.png`（`fb create` + 全屏重绘） |
| 输入真的到远端 | 日志 `kbd usage=8 down=1 sc=0x12`，远端搜索框出现 `eu` |

### 6.2 模拟器测不了的（必须真机）

- 系统键盘（Y / 菜单"输入文字"）：模拟器里 host 键盘被它的 UI 抢走，
  `swkbd` 弹不出来也点不到。`swkbd` 本身在主界面编辑字段时是验证过的。
- ZL/ZR、右摇杆方向、摇杆速度：模拟器的键盘→手柄映射只转发了 L/R/X 等少数键。
- 流畅度手感：模拟器的 `avg_blit` 只有参考意义（6~7 ms/次，`avg_pump` 1 ms）。

### 6.3 驱动模拟器的三个前置条件（踩过的坑）

1. **必须降权启动**：Ryujinx 检测到管理员令牌会直接退出
   （`Ryujinx is not intended to be run as administrator`）。命令行参数也被忽略。
   可靠做法是写一个只含 8.3 短路径的 .bat：

   ```bat
   @echo off
   runas /trustlevel:0x20000 "C:\...\Ryujinx.exe C:\...\switch_rdp.nro"
   ```

   注意 `runas` 只接受**一个**带引号的命令行字符串（程序 + 参数放同一对引号里）。
2. **关掉更新提示**：它的更新对话框会抢键盘（并可能触发下载），而且 Ryujinx
   退出时会把 `Config.json` 里的 `enable_keyboard` / `enable_mouse` /
   `start_no_ui` 写回它内存里的值（常常是 false）。所以**每次启动前**改，
   不要指望改一次一直有效：

   ```python
   c["start_no_ui"] = True; c["enable_keyboard"] = True; c["enable_mouse"] = True
   c["update_checker_type"] = "Never"; c["check_updates_on_start"] = False
   ```
3. **日志可能丢行**：模拟器的 SD 写盘在高频 `fflush` 下偶尔丢几行。判定状态
   优先看截图，日志只作辅助。

### 6.4 现成的驱动脚本

- `tools/ryu_test.ps1`：启动 + 按计划发键 + 截图 + 读日志
  （`-Plan "wait:9000;seq:UP,UP,...;shot:xx;log"`）。
- `tools/test_b23.ps1`：b23 全流程（写 cfg → 启动 → 魔法代码 → 菜单 → 输入法）。
- `tools/test_b23b.ps1`：纯截图判定版。
- 键位映射（Ryujinx 默认）：方向键 = D-pad，`Z`=A、`X`=B、`C`=X、`V`=Y、
  `E`=L、`U`=R、`Q`=ZL、`O`=ZR、`I/J/K/L`=右摇杆。

PowerShell 脚本一律用
`powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\xxx.ps1` 跑
（本机执行策略禁止直接运行脚本），并且脚本里不能出现中文（5.1 按 ANSI 读 .ps1，
中文路径会变乱码，所以项目路径用 `$MyInvocation` 推出来）。

## 7. 真机安装（b23 之后）

```powershell
# SD 插在 PC 上时（会先做一次只读检查）
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\install_to_sd.ps1 -Apply
```

拷的是 **`dist/switch_rdp_b23_menu_hw.nro`（真机版，无 ASET 尾段）**，
落到 ` SD:\switch\switch_rdp.nro`。模拟器用的带资源段版本不要往真机 SD 上放
（能跑，但先排除变量；两者镜像字节相同，只差 22 KB 尾部）。

## 8. interleaved RLE 解码器离线对拍（b40 新增）

不用上机就能确认解码器没写错：

```
cd APP/switch_rdp
python tools/_mk_rle_test.py
Get-Content tools/rle_vectors.txt | Where-Object { $_ -match '^\s*\d' } | & .\tools\_rle_test.exe
Get-Content tools/rle_vectors.txt | Where-Object { $_ -match '^\s*\d' } | & python tools/_rle_sim_batch.py
```

两边每行 `rc= hash=` 必须一致。`_mk_rle_test.py` 是把 `src/rdp_update.c` 里的解码
函数**原样抠出来**编译的，所以对拍的就是出货代码，不会出现"测试过了、固件没改"的情况。
注意 mingw gcc 打不开含中文的绝对路径，脚本因此切到 tools 目录用相对文件名调用。

## 9. b41 手工验收清单（实机）

| 项 | 预期 |
|---|---|
| 左摇杆 | 指针跟手，小推一格一格挪，推到底跑得快 |
| 右摇杆上下 | 页面/列表滚动，轻推一格，推到底连续滚 |
| ZL / ZR | 左键 / 右键（不变） |
| L+R 300ms | 会话菜单（不变） |
| Y | 系统键盘，按地球键能切到简体中文（拼音），确认后远端出现中文 |
| `+` | 存可见配置 |
| 填隐藏服务器 → `+` 和 `-` | 提示"备用配置已保存 已恢复界面配置"，界面回到原来的服务器 |
| 秘籍 | 用 `alt_*` 连接；`alt_server` 为空时只提示 |
| USB 键盘 | 按键后日志出现 `kbd usage=... sc=...`；远端能收到 |

`alt_*` 可以在 PC 上直接写进 `sd:/switch_rdp.cfg`，不必在机器上敲一遍。
