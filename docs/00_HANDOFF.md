# 00 — 交接总览（先读这份）

> 最后更新：2026-10-23。这份文件是给"下一个接手的人/模型"看的，目标是**不用重新踩坑**。

---

## 1. 这是什么

Switch 上的独立 RDP 客户端。协议栈全部自己实现，只依赖 libnx 和 mbedTLS：

```
src/rdp_tls.c      TLS 1.2 传输（mbedTLS，不校验证书）
src/rdp_credssp.c  NLA：NTLMv2 + MIC + KEY_EXCH + RC4 + pubKeyAuth + TSCredentials
src/rdp_mcs.c      TPKT / X.224 / MCS（GCC 结构、attach、join、SendData）
src/rdp_session.c  连接编排、共享数据头、确认/收尾、PDU 泵、输入发送
src/rdp_update.c   FastPath 更新解析、分片重组、位图上屏
src/rdp_planar.c   RDP6 Planar 位图解码（远程桌面主用编码）
src/rdp_input.c    FastPath 输入事件编码 + HID→PC 扫描码表
src/main.c         UI、配置、帧缓冲显示、按键/摇杆
```

## 2. 完成度

| 功能 | 状态 | 证据 |
|---|---|---|
| TCP/X.224/TLS/CredSSP | ✅ | 服务器返回 err=0 |
| MCS 连接/attach/join/Client Info | ✅ | 6 个 join 全部 result=0 |
| Demand/Confirm Active + finalization | ✅ | 服务器开始推流 |
| FastPath 位图（分片 + Planar） | ✅ | 1280×720 桌面、资源管理器窗口画面正确 |
| 键盘 | ✅ | Win+E 打开资源管理器（**PC harness** 实测画面变化） |
| 鼠标（移动+按键） | ✅ | 双击回收站图标打开窗口（**PC harness** 实测） |
| Switch 侧 hid 输入 | ⏳ 未验 | 模拟器只确认了画面，键盘/摇杆待实机或模拟器补测 |
| Ryujinx 模拟器运行 | ✅ | SD 日志 `frame 50 ... rects=1821` |
| **实机（真 Switch）** | ⏳ 待测 | — |
| 鼠标光标显示 | ❌ 未做 | 见 05_ROADMAP P0 |
| Surface commands | ❌ 忽略 | 偶发残影 |
| 剪贴板 / 声音 / 磁盘重定向 | ❌ 未做 | 通道已 Join，未实现协议 |

## 3. 必须守住的不变量（改代码前务必确认）

这些每一条都是拿几小时换来的，破坏任何一条都会**静默失败**（不报错、没画面、或服务器直接断线）：

1. **MCS 上行 PDU 的 initiator 必须是原始 userId（本例 8）**，不是通道号。
   用户通道号 = `userId + 1001`（1009）。见 `rdp_mcs.c: MCS_BASE_CHANNEL_ID = 1001`。
2. **AttachUserRequest = 0x28，ChannelJoinRequest = 0x38**，每个 join 必须读一次确认再发下一个。
3. **CS_CORE 里必须填 `serverSelectedProtocol`**（X.224 协商结果，本例 0x08），否则服务器收到
   Connect Initial 直接 TLS close。
4. **BER INTEGER 高位为 1 时要补 0x00**（`0xFFFF` 写成 `02 03 00 FF FF`）。
5. **GCC user data 块顺序** core → security → net → cluster。
6. **Channel options = 0x80800000**。
7. **共享数据头 18 字节**，`uncompressedLength` 可以写 body 长度（FreeRDP 风格）或整包长度
   （服务器自己写整包），两者都被接受；**不要写 0**。
8. **FastPath 输入事件码：键盘=0，鼠标=1（0x20）**。写反会被服务器断开（SIGPIPE）。
9. **按键事件必须带当前指针坐标**，否则 Windows 把指针拉到 (0,0) 再点。
10. **Order capset 只能清 `orderSupport[32]` 和 `orderSupportExFlags`，绝对不能清 `orderFlags`**
    （清了会 `SET_ERROR_INFO 0x10EA` 断线）。
11. **Planar 解码结果已经是 top-down，上屏时不要翻转**（`dy = dt + y`）。
12. **`consoleExit()` 之后不能再 printf/consoleUpdate**（`rdp_console_live` 标志必须为 0）。
14. **桌面循环不能阻塞在收包里**。`rdp_conn_pump(timeout_ms)` 的 timeout 只用于**第一个字节**（`rdp_tls_set_timeout`），没数据就返回 0；否则键盘/摇杆采样会被socket 读卡住，快速按键直接丢失（真机上表现为“完全没输入”）。
13. **编译必须 `-fPIE`、mbedTLS 必须 `-fPIC`、链接必须用 `nx/switch.specs`（`-z text`）**。
    任何一条缺失 → ELF 里出现只读段重定位 → 真机 `2168-0002` 崩在 `_dynProcessRela`。
    自查：`python tools/rela_scan.py build/switch_rdp.elf` 必须报 0。

## 4. 目标环境（写死的常量，换服务器要改）

| 项 | 值 |
|---|---|
| 项目目录 | `<repo>` |
| 服务器 | `192.168.1.10:3389`（RDP 6 位图服务器，原生 1024×768，接受 1280×720） |
| 账号 | `user` / `password`，域空 |
| 客户端请求分辨率 | 1280×720（`main.c: RDP_WIDTH/HEIGHT`，正好等于 Switch 屏幕） |
| 协议 | 服务器**强制 NLA**：X.224 offer 0x00 → `RDP_NEG_FAILURE` code 5 |

服务器会按客户端请求的分辨率开桌面（1280×720 实测可行）。

## 5. 三条最容易踩的整体性坑

- **服务器不回任何东西**：先看它到底有没有收到 Client Info（`RDP_DUMP=1` 打印 MCS 层字节），
  99% 是 MCS initiator / attach / join 类型错。
- **有画面但花屏/撕裂**：先确认 Planar 上屏没做垂直翻转；再确认 Order capset 的
  `orderSupport` 已清 0（否则服务器用绘图顺序画窗口，我们只认位图）。
- **按键没反应**：这台服务器**完全忽略慢速输入 PDU（pduType2=0x1C）**，必须走 FastPath；
  FastPath 的 eventCode 也必须对。

## 6. 下一步做什么

见 [`05_ROADMAP.md`](05_ROADMAP.md)。最高优先级是：

1. **实机验证**（USB 键盘是否能出字符）。
2. **鼠标光标**（现在远端指针不可见，盲操很难受）。
3. **脏矩形上屏**（现在每帧整屏 3.7 MB memcpy）。

## 7. 代码里现存的"实验残留"（不是 bug，别当成垃圾清掉）

| 位置 | 是什么 | 说明 |
|---|---|---|
| `rdp_session.c` 里所有 `getenv("RDP_*")` | 调试开关（`RDP_DUMP`/`RDP_DUMP2`/`RDP_ORDERS`/`RDP_SLOWPATH_INPUT`/`RDP_INPUT_ON_USER`） | Switch 上环境为空 → 全部走默认分支（不打印、正常快速输入）。留着方便 PC 侧排查 |
| `rdp_session.c: send_input_event()` + `rdp_input.c: rdp_input_event_*()` | 慢速输入 PDU 构造器 | 本服务器忽略慢速输入，保留作对照（`RDP_SLOWPATH_INPUT=1`） |
| `rdp_update.c: rdp_update_other_codes[16]` | 统计非位图 update code 的出现次数 | 只有 host harness 打印；将来实现光标/Surface 时可以直接用 |
| `src/rdp_nla.c` `rdp_proto.c` `posix_stubs.c` `rdp_bitmap.c` | 早期实现 / 未被 Makefile 编译 | `rdp_bitmap.h` 只提供 `rdp_surface` 类型 |
| `main.c` 里的 `tlog` + `switch_rdp.cfg` | SD 卡日志与免填表直连 | 实机排障全靠它，别删 |

## 8. 本机既有资产（别重复造）

- `tools/rdp_client.py` —— **已验证可用的 Python 原型**（1661 行），协议流程的"参考实现"，
  任何 C 侧疑难都可以拿它对拍。用法见 `docs/04_TESTING.md`。
- `tools/planar.py` —— Planar 解码器的 Python 版，和 C 版做过逐字节对拍（md5 相同）。
- `<repo>\ref\codec\` —— FreeRDP 3.5.1 的相关源码摘录（interleaved/bitmap/planar/nsc）。
- `tools/an_xf.py`、`tools/an_cf.py` —— 从 FreeRDP 抓包日志里解析 Confirm Active / 输入 PDU。
- `docs/08_DEVLOG.md` —— 更早的流水笔记
  （含各种失败尝试），有需要可以查。

## 9. b23（2026-10-24）：菜单 / 输入法 / 配置自动连接 / 手感

构建标签 `b23-menu-type`，产物 `dist/switch_rdp_b23_menu.nro`。

### 9.1 新功能（用户需求的对应实现）

| 需求 | 实现 | 位置 |
|---|---|---|
| 右摇杆上下颠倒 | 屏幕 y 向下，libnx 摇杆 +y 向上，改成 `my -= ny*16` | `main.c` run_desktop |
| ZL/ZR 反了 | ZL = 鼠标左键(1)，ZR = 右键(2) | 同上 |
| L+R 呼出菜单 | 两键同时按住 ≥300 ms；菜单里仍在 pump 连接 | `session_menu()` |
| 菜单改设置 / 断开 / 退出 | 5 项：继续 / 输入文字 / 设置 / 断开 / 退出 | `session_menu()`、`settings_loop()` |
| Y 呼出输入法 | 桌面按 Y → 系统键盘 → 逐字符 UTF-16 送服务器 | `type_from_desktop()`、`send_text_to_remote()` |
| 默认账号 administrator | 编译期默认 + 首次启动写的模板都是 `Administrator` | `main.c` g_user / `cfg_write()` |
| 自动写配置文件 | 首次启动写 `sdmc:/switch_rdp.cfg`（带注释模板） | `cfg_ensure()` |
| 上上下下左右左右BA 直连 | 主界面识别 10 连击 → 重读 cfg → 直接连 | `konami_push()` / main 循环 |

### 9.2 配置文件格式（两种都认，向后兼容）

```
# 注释行以 # 开头
server = 192.168.1.10
port   = 3389
user   = Administrator
pass   = 明文密码
```
也接受老的四行位置格式（server/port/user/pass 各一行）。键名可用
server|host|ip、port、user、pass(password)。改字段会立刻整文件重写。

### 9.3 本版修掉的三个"卡死"

1. **按一个键就整个画面冻住**：`rdp_tls_write()` 对 `WANT_WRITE` 无界 `continue`，
   而 send 回调里 `poll(POLLOUT)` 一直超时 → 无限循环。现在回调对
   EPIPE/ECONNRESET 立即返回 -1，可写等待最多 4×2 s；上层最多重试 8 次。
2. **菜单里待久了连接死锁**：不读 socket → 服务器发送窗口关闭 → 它也不读我们的
   输入 → 我们的写阻塞。现在菜单循环每轮 `rdp_conn_pump(conn,0)` 最多 16 次。
3. **日志把崩溃现场吃掉**：`fopen("w")` 每次启动截断。改成 `a+`，超过 512 KB
   才轮转成 `switch_rdp_log.old.txt`，每次启动插一行 `==== boot ====`。

### 9.4 需要在真机上确认的（模拟器测不了）

- ZL/ZR 左右键、右摇杆方向（模拟器只转发了部分按键）。
- 桌面按 Y 弹系统键盘 + 打进去的字真的出现在远端（模拟器里键盘抢焦点，
  测不到；`swkbd` 本身在主界面编辑字段时是好的）。
- 摇杆速度/加速度手感（`ny*16` 是固定步长，可调）。

## 10. b24（2026-10-24）：中文界面 + 去掉 console + 连接可取消

构建标签 `b24-ui-cn`。产物：`dist/switch_rdp_b24_ui_cn_hw.nro`（真机，无 ASET 尾段）/ 
`dist/switch_rdp_b24_ui_cn.nro`（Ryujinx 用）。

### 10.1 为什么重写渲染

b23 在 L+R 里做了 `framebufferClose()` -> `consoleInit()` ->（回桌面）`framebufferCreate()`，
在同一个窗口上反复拆建 libnx console 与 framebuffer。真机按 L+R 就崩 2168-0002：

- 模块基址 0x264F1F6000 + 镜像 0x116000 + bss 0x2C5B0 => 模块结束 0x264F3365B0
- 崩溃 PC 0x264F382F84 => 越过模块 3 万多字节，落在堆里（野跳/堆被踩）

b24 **彻底不用 libnx console**：所有界面（主界面、连接中、会话菜单、设置、输入法提示）
都由 `ui_draw.c` 直接画进 framebuffer。这条崩溃路径不存在了。

### 10.2 中文字库

`tools/gen_ui_font.py` 扫描 `src/*.c` 里出现的所有非 ASCII 字符，用微软雅黑渲染成
1bpp 字形（ASCII 8x16、中文 16x16），生成 `src/ui_font.c`（269 个字形，约 7 KB）。
改界面文字后**必须重跑**该脚本，否则新字显示成 `?`。缺字会由脚本自动纳入。

### 10.3 连接体验

- `connect()` 非阻塞 + 6 秒超时，**全程按 B 可取消**（`rdp_cancel_hook`）。
- 失败原因分开显示：地址无效 / 超时 / 端口被拒 / 不可达 / TLS / 认证失败 / MCS。
- **NLA 认证失败现在是硬失败**：原来 `nla_authenticate()` 的返回值被忽略，密码错了
  照样往下走，最后 MCS 断线只报"未知错误"。现在直接报"账号或密码错误"。
- 连接中界面只显示"正在连接 ip:port"、进度点和"按 B 取消"；`LOG()` 不再重绘桌面
  （`g_connecting` 门控）。

### 10.4 秘籍不显示

主界面不再写"上上下下左右左右BA"，配置文件模板注释里也删了。
秘籍只存在于 `docs/` 里，界面/配置文件都不再提示。

### 10.5 RDP 5 位图服务器 黑屏

`rdp_update.c` 的未压缩位图行按 **4 字节对齐**读取（`TS_BITMAP_DATA` 行有 padding，
不对齐会让每行错位成斜条纹/花块）。另外对 `BITMAP_COMPRESSION`（RLE）加了检测点，
如果日志里出现说明服务器发了压缩位图，需要补 RLE 解码。

### 10.6 工具链注意

devkitPro 已被另一项任务升级到 **GCC 15.2.0**，且它的 `cc1.exe` 依赖 mingw64 的
运行库，所以编译时 PATH 必须包含：

```
C:\devkitPro\devkitA64\bin;C:\devkitPro\msys2\mingw64\bin
```

否则 `gcc -c` 直接 0xC0000135（STATUS_DLL_NOT_FOUND）。libnx 已按 GCC 15 重装。
另外 binutils 现在会真的执行 specs 里的 `-z pack-relative-relocs`（生成 `.relr.dyn`），
本工程在 LDFLAGS 里加了 `-Wl,-z,nopack-relative-relocs` 保持已验证的 `.rela.dyn` 路径。
