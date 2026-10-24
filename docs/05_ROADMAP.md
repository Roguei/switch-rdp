# 05 — 待办清单（按优先级）

每个条目都写了"改哪里 / 怎么验证"，可以直接开工。

---

## P0-1 实机验证（阻塞其它一切）

硬件到货后第一件事。清单见 [`04_TESTING.md`](04_TESTING.md) §4。

重点关注：

- `sd:/switch_rdp_log.txt` 里 `sdmc mount` 的返回值（模拟器是 0x559 表示已挂载，实机应为 0x0）；
- `hidInitializeKeyboard()` 是否成功；
- USB 键盘插在底座上时 `hidGetKeyboardStates()` 能否读到。

**如果键盘没反应**，先加诊断（`main.c: run_desktop`）：

```c
HidKeyboardState ks;
int n = hidGetKeyboardStates(&ks, 1);
tlog("hid poll n=%d keys=%llx %llx", n,
     (unsigned long long)ks.keys[0], (unsigned long long)ks.keys[1]);
```

同时可以枚举设备：`hidEnumerateKeyboardDevices()`（libnx，返回 HID 设备列表）。

---

## P0-2 鼠标光标显示（体验提升最大）

**现状**：远端指针完全不可见，只能看点击后的画面变化。FastPath 里其实一直在推光标更新
（code 5/6/8/10/11），我们全部丢弃了。

**要做的**：

1. `rdp_update.c: rdp_update_fastpath()` 里目前 `if (code != UPD_CODE_BITMAP) continue;`
   —— 加一个指针分支。
2. 光标状态机（新增 `src/rdp_cursor.c/.h` 更清爽）：

```
typedef struct {
    int visible;              /* code 5 -> 0, code 6/11 -> 1 */
    int x, y;                 /* code 8 */
    int have_image;           /* code 11 收到过图 */
    int width, height, hot_x, hot_y;
    uint32_t pixels[32*32];   /* ARGB，够用 */
    uint32_t cache[8][32*32]; /* code 10 复用；cacheIndex 0..7 */
} rdp_cursor;
```

3. 各 code 的载荷（紧跟 1 字节 updateCode，**无 header**）：

| code | 载荷 |
|---|---|
| 5 PTR_NULL | 空（隐藏） |
| 6 PTR_DEFAULT | 1 字节 `cacheIndex`（用内置默认箭头） |
| 8 PTR_POSITION | `xPos(2) yPos(2)`，小端 |
| 10 PTR_CACHED | `cacheIndex(2)` |
| 11 PTR_POINTER | 见下 |

code 11 的 `TS_POINTER_ATTRIBUTE`：

```
cacheIndex(2)  hotSpotX(2) hotSpotY(2)  width(2)  height(2)  lengthAndMask(2)  lengthXorMask(2)
AND mask[lengthAndMask]      XOR mask[lengthXorMask]
```

- `lengthAndMask != 0` → 单色光标：1bpp 掩码，AND/XOR 合成；
- `lengthAndMask == 0` → 彩色光标：XOR 掩码是 32bpp ARGB（每行 width*4 字节）。
- 32×32 的情况最常见；也可能是 48×48（Win11 大光标），缓冲按最大值开。

4. 画的时候**不要写进 surface**（会和后续更新打架），而是在 `main.c: fb_blit()` 之后
   叠加一次：用保存的背景或用"重画脏矩形"方式。最省事的做法是：
   - `blit` 时记录"被光标覆盖过的矩形"，光标移动前先让这个矩形重画（发 Refresh Rect
     或把 surface 里存一份 backup）；
   - 或者干脆每帧把光标画进 framebuffer 的副本（surface 不动），最简单，但要 3.7 MB 额外拷贝。

**建议实现顺序**：先只做 code 8（位置）+ 一个硬编码的箭头图（软件光栅化），
能在画面上看到一个会动的指针就够用了；再补 code 11 的完整解析（真实光标形状）。

**验证**：PC harness 里鼠标移动后，PPM 上应该能看到箭头；或者直接看日志里
`cursor x=.. y=..` 在变化。

---

## P0-3 脏矩形上屏（性能）

**现状**：`main.c: fb_blit()` 每帧整屏 memcpy 3.7 MB（1280×720×4）。Switch 上内存带宽有限，
拖动窗口时会明显掉帧。

**做法**：

1. 在 `rdp_surface` 里加 `int dirty_x0, dirty_y0, dirty_x1, dirty_y1;`
   （`rdp_bitmap.h` 里就有 `rdp_surface` 定义）。
2. `rdp_update.c: blit()` / `blit_raw()` 每次写入时扩展这个包围盒。
3. `rdp_conn_pump()` 返回 1 时置位；`fb_blit()` 只拷 `dirty` 区域：

```c
for (y = y0; y < y1; y++)
    memcpy(dst + y*stride + x0*4, src + (y*w + x0)*4, (x1-x0)*4);
```

4. **注意**：`framebufferBegin()` 返回的 buffer 不保证还是上一帧的内容（三缓冲轮转），
   只拷脏矩形会留下"旧帧残影"。两个解法：
   - 在 framebuffer 里也做一份软件 back buffer（3.7 MB，拷贝一次全屏 → 仍然省了 GPU 上传？不省）；
   - 或者保留"本帧有没有全屏重绘"的标志：服务器一旦发整屏更新就全拷，否则只更新脏区
     并且**同时**把脏区记录累积到下一次全屏（这是常见做法，验证过再说）。

**验证**：`RDP_UTRACE=1` 观察每帧耗时；在 Switch 上拖动窗口看是否跟手。

---

## P1-1 连接健壮性

- **SIGPIPE**：服务器断线时 host 版直接收到 SIGPIPE（rc=141）。加 `signal(SIGPIPE, SIG_IGN)`
  （非 Switch 分支），并在 `tr_send` 失败时返回 -1 而不是让进程死。
- **断线提示**：`run_desktop()` 里 `r < 0` 现在只是涂红然后 break → 卡在死循环。
  应该：显示"连接已断开，按 Y 重连"并回到主循环。
- **超时**：`tls_recv_cb` 里 poll 8 s 超时返回 WANT_READ → `rdp_tls_read` 返回负数 →
  上层当错误。要区分"超时"和"断开"（返回 0 表示超时，-1 表示断开）。
- **重连**：保存 `rdp_cfg`，重连时重建整个 `rdp_conn`。

## P1-2 设置界面

现在只有 IP/端口/用户/密码四个字段 + 自动连接 cfg。可加：

- 分辨率选择（1024×768 / 1280×720 / 1920×1080）；
- "保存到 SD 卡"按钮（写 `sdmc:/switch_rdp.cfg`）；
- 连接失败时显示错误码（现在只显示一句失败）。

## P1-3 Surface Commands（code 4）

**作用**：服务器把"整块大位图"用 SURFCMDS 的 `STREAM_BITMAP` 发过来（大窗口/视频更常见），
我们忽略会留下**残影**（已观察到：旧窗口标题栏残留）。

**载荷**：`numberSurfaceCommands(2)` + 每条 `cmdType(2) + 命令数据`。
常见 `cmdType`：`SET_SURFACE_BITS(0x0001)`、`FRAME_MARKER(0x0004)`、`STREAM_BITS(0x0006)`。

`SET_SURFACE_BITS`：

```
surfaceId(2)  cmdType(2)=1  destLeft(2) destTop(2) destRight(2) destBottom(2)
bpp(1)  codecId(1)  width(2) height(2)  bitmapDataLength(4)  data
```

`codecId` 常见 `0x01`（RDP6 位图）/`0x03`（远程FX，未实现）/`0x00`（原始）。

MVP：只处理 `codecId == 0x01`（当普通 planar 位图解）和 `FRAME_MARKER`（可忽略）。
这块能明显减少残影。

## P2 虚拟通道（需要先实现通道数据收发）

`rdp_mcs` 已经 join 了 4 个通道（rdpdr/cliprdr/rdpsnd/drdynvc），但目前**只收发静态通道 1003**。
要支持虚拟通道，需要：

1. `recv_pdu` 返回 `chan`（已经有了），按通道分发；
2. 每个通道自己的分包：`CHANNEL_PDU_HEADER {length(4), flags(4)}`，`CHANNEL_FLAG_FIRST/LAST`；
3. 各通道协议：
   - **cliprdr**（剪贴板）：`CLIPRDR_HEADER {msgType(2), msgFlags(2), dataLen(4)}`，
     先 `CB_MONITOR_READY` → 服务器发 `CB_FORMAT_LIST` → 按需 `CB_FORMAT_DATA_REQUEST`/`CB_FORMAT_DATA_RESPONSE`（文本用 CF_UNICODETEXT=13）。**性价比最高**。
   - **rdpdr**（磁盘重定向）：协议较大（设备 I/O 请求 IRP），要挂一个虚拟盘符。
   - **rdpsnd**（声音）：`RDPSND_MSG`，PCM 直接喂 Switch audout/audren。
   - **drdynvc**：动态通道，实现后才能用 `Microsoft::Windows::RDS::Graphics`（需要 H.264 解码，不建议）。

## P3 其它

- **键盘布局**：目前固定 US 布局（扫描码与布局无关，但特殊字符依赖远端布局）。
- **IME / Unicode 输入**：`INPUT_FLAG_UNICODE` 已声明，但只发扫描码。要输入中文得发
  `INPUT_EVENT_UNICODE`（慢速）/ FastPath UNICODE 事件。
- **多显示器 / 动态分辨率**：`desktopWidth/Height` 目前固定，可用 `Client Control` 的
  `CTRLACTION_...`/`Desktop Resize` 实现，非必需。
- **凭证安全**：`switch_rdp.cfg` 是明文密码。可以加一层 obfuscation，或交给
  sysmodule 的密钥库；至少文档里要提醒用户。
- **性能**：Planar 解码是纯 C 逐像素，Switch 上 1280×720 全屏重绘约 100 万像素，
  必要时可以：
  - 用 `-O3` / NEON intrinsics 改写 `rdp_planar_decode` 的 YCoCg 转换；
  - 或者干脆跳过 Co/Cg 的 2×2 展开，直接按 2×2 块算。

## b23 已完成（2026-10-24）

- ~~P0-2 鼠标光标~~：本地画 12×19 箭头，脏矩形重绘（b21/b22）。
- ~~P1-2 设置界面~~：主界面 + 会话菜单都能改，改完直接落盘。
- ~~P0-3 脏矩形上屏~~：只拷脏框 + 持久线性 buffer。
- 新增：会话菜单、系统输入法送字、魔法代码直连、配置文件自动生成、TCP_NODELAY。

## 现在的优先级

1. **真机回归**（只有用户能做）：ZL/ZR、右摇杆方向、L+R 菜单、Y 输入法、
   魔法代码、摇杆速度手感。
2. 服务器端光标（FastPath PTR_POSITION / PTR_COLOR）—— 现在是自己画的箭头，
   遇到 I-beam/手型光标会不对。
3. 摇杆加速度曲线（现在固定 16 px/帧，慢速时不好微调）。
4. Surface Commands（code 4）与高质量压缩（RFX 太重，可以先只做 NSCodec 判定）。
5. 剪贴板 / 声音 / 磁盘重定向（都要先有虚拟通道收发）。
