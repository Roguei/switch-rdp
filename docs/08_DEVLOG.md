> **这是开发流水笔记（按时间顺序）**，里面的路径/结论可能已经过时。
> 结论请看 `00_HANDOFF.md` 和 `02_PROTOCOL.md`；工具路径请看 `03_BUILD_TOOLING.md`。

> **项目已迁到工作区内**：`<repo>`
> 交接文档在 `APP\switch_rdp\docs\`（从 `00_HANDOFF.md` 开始读）。
> 本文件是开发流水笔记，结论以 docs 为准。
# Switch Homebrew 开发踩坑记录（switch_rdp 项目实战总结）

> 目的：让后续在本工作区开发 Switch 自制程序的模型/开发者少走弯路。
> 环境：Windows + devkitPro (devkitA64 + libnx) + Ryujinx (Nextendo 分支) 模拟器。

---

## 0. 一句话总结

**Switch homebrew 的核心循环只有三件事：libnx 服务（socket/hid/vi）、帧缓冲、`.nro` 打包。
任何试图把桌面 Linux 生态（FreeRDP/winpr、OpenSSL 全家桶、POSIX 线程/信号/定时器）搬上来的方案，
都会在平台抽象层付出巨大代价——务必优先选“自包含”的实现。**

---

## 1. NRO 打包（最容易卡住的第一关）

Ryujinx 对 `.nro` 格式检查很严格，裸 ELF 会被拒绝：
`System.IO.InvalidDataException: NRO0 magic is incorrect!`

**正确的 NRO 布局：**

| 偏移 | 内容 |
|---|---|
| 0x00 | `NroStart`：u32 分支指令 `0x14000020`（`b #0x80`）、u32 mod_offset、char 模块名[8]（如 `"HOMEBREW"`） |
| 0x10 | `"NRO0"` 魔数（**不是 0x00！**） |
| 0x14 | u32 version = 0 |
| 0x18 | u32 size（镜像总大小） |
| 0x1C | u32 flags = 0 |
| 0x20 | 3 个 segment：`{u32 file_off; u32 size;}` ×3（text/rodata/data，页对齐且连续） |
| 0x38 | u32 bss_size |
| 0x3C | u32 reserved |
| 0x40 | u8 build_id[0x20] |
| 0x60 | u8 padding[0x20] |
| 0x80 | codebin 起始（即 ELF 的 vaddr 0） |
| size 之后 | 可选的 `"ASET"` 资源头（icon/nacp/romfs），**缺了会 NullReferenceException** |

**关键点**：libnx 链接脚本已经把 `NroStart` 放在 vaddr 0，并把 0x10~0x80 留空，
所以“ELF 的扁平镜像 + 在 0x10 处塞入 NRO 头”就是正确的 NRO。
本仓库 `<workdir>\toolchain\build_nro.ps1` 就是干这个的（从 ELF 的 PT_LOAD 生成扁平镜像、算 bss、补 ASET）。

---

## 2. 帧缓冲 / 显示

### 2.1 不要在 framebufferCreate 之前调用 consoleInit
`consoleInit()` 会占用默认 NWindow，之后 `framebufferCreate()` 返回 **0xf59**。
两者只能二选一：先用 console 做 UI，退出（`consoleExit(NULL)`）后再建帧缓冲。

### 2.2 取窗口用 `nwindowGetDefault()`
不要用 `svcGetInfo(&w, 6, 0, 0)` 之类“偏方”取窗口——那是无效句柄（日志里会看到 `GetInfo = InvalidHandle`）。

### 2.3 至少 3 个缓冲
`framebufferCreate(..., num_fbs=1)` 时，**第二次** `framebufferBegin()` 会永久阻塞
（唯一 buffer 还在合成器手里）。用 3（失败再退 2）。

### 2.4 像素字节序
`PIXEL_FORMAT_RGBA_8888` + `framebufferMakeLinear` 的内存顺序是 **R,G,B,A**。
写成 B,G,R,A 会导致颜色错乱（青会显示成黄，橙显示成蓝）。

### 2.5 千万别按 stride 写满整行
`framebufferBegin` 返回的 `stride` 是**对齐后**的行宽，可能大于 `width*4`；
而 `framebufferMakeLinear` 的影子缓冲区只按 `width*height*4` 分配。
按 stride 写满会**每帧越界写约几十 KB 到堆里**，导致后续 `malloc/calloc` 随机崩溃
（表现：`memset` 访问地址 0）。正确做法：每行只写 `min(width*4, stride)` 字节。

---

## 3. 线程与 TLS（最隐蔽的坑）

- libnx 用 `tpidrro_el0` 保存 `ThreadLocalRegion`；newlib 的 TCB 指针存在 `[TP + 504]`。
- **不能写 `tpidrro_el0`**：EL0 无权写（只读），Ryujinx 直接抛
  `NotImplementedException: Unknown MSR 0xD51BD076`。
- `threadCreate()` 创建的线程，其 newlib TLS 槽可能为空 → 线程里第一次 `malloc` 就崩。
- 结论：**优先单线程**。必须多线程时，避免在新线程里做分配；或提前在主线程把要用的内存都分配好。

---

## 4. 文件/日志调试通道

- `sdmc:/` 在 Ryujinx 里映射到 `portable/sdcard/`，可以直接在主机上读，是很好的调试输出通道。
- 但注意：**FreeRDP 初始化之后**，`fsdev` 写入会崩（互斥量指针变 NULL）；控制台刷新同样会崩。
  所以日志只能打在“危险调用”之前。
- 更稳的做法：**用颜色/文字分阶段显示**（见第 7 节）。

---

## 5. 第三方库：FreeRDP 的教训（重要）

我们尝试把 FreeRDP 3.x 交叉编译到 Switch，结论是**不推荐**：

- winpr 需要大量 POSIX 特性：`sigaction/SA_RESTART`、`timer_create`、`dlfcn`、
  `termios`、`pipe2`、`pthread_mutex_timedlock`、`getpwuid_r`、`fork/waitpid`……
  libnx 的 newlib 大多没有，只能靠垫片头 + 空实现糊过去。
- CMake 特性检测在交叉编译下会“全部通过”，生成不存在的 `WINPR_HAVE_*`，
  然后编译期才发现 `sys/un.h`、`termios.h` 找不到。
- FreeRDP 之后 libnx 的 console/fsdev 会莫名崩溃，运行期行为不可控。

**若仍要编译 FreeRDP，务必：**
1. 复用现成的垫片目录（本机是 `C:\freerdp_build\stubs`），并把它放在 `-I` 的最前面。
2. 用 `-D_GNU_SOURCE`，加 `-DWITH_UNICODE_BUILTIN=ON`（否则要 ICU）、`-DWITH_KRB5=OFF`。
3. 用 mbedTLS 后端替代 OpenSSL：`-DWITH_OPENSSL=OFF -DWITH_MBEDTLS=ON -DMBEDTLS_ROOT=...`。
4. 关掉不需要的通道：`-DCHANNEL_URBDRC=OFF`（要 libusb）、`-DWITH_SMARTCARD=OFF`。
5. CMake 用 msys 的 cmake 配置，但**编译要用原生 GNU make**（`<workdir>\toolchain\make.exe`）；
   msys make 处理 `C:/` 路径的依赖文件会报 “multiple target patterns”。
6. 加 `-DCMAKE_DEPENDS_USE_COMPILER=OFF` 避免依赖文件里的盘符冒号问题；
   或在 build.make 里把 `include XXX/compiler_depend.make` 改成 `-include`。

---

## 6. 正确的第三方依赖选择

| 需求 | 推荐 | 说明 |
|---|---|---|
| TLS | **mbedTLS 2.28** | 纯 C、裸机友好。配置：注释掉 `MBEDTLS_FS_IO/TIMING_C/NET_C/HAVE_TIME`，开 `MBEDTLS_NO_PLATFORM_ENTROPY` + `MBEDTLS_ENTROPY_HARDWARE_ALT`，熵源用 `svcGetInfo(InfoType_RandomEntropy=11)`。96 个 .c 全部零错误编译通过。 |
| 加密 | 自己写 | MD5/SHA1/MD4/HMAC/RC4 各几十行，RDP 标准安全层够用；大数运算交给 mbedTLS 的 `mbedtls_mpi_exp_mod`。 |
| RDP 协议 | **自己实现** | 只依赖 socket + 帧缓冲，完全可控。参考 MS-RDPBCGR 与 FreeRDP 源码。 |

---

## 7. 调试方法论（本次最有效的几招）

1. **阶段颜色**：把每个里程碑画成整屏纯色（绿=帧缓冲OK、品红=实例创建OK、橙=连接中、红=失败）。
   比看日志快，而且不依赖任何 IO。
2. **控制台文字**：在还没有帧缓冲时，`printf + consoleUpdate(NULL)` 能显示文字；
   但注意它在某些库初始化之后会崩。
3. **二进制扫描**：`NRO0`、`HOMEBREW`、`ASET` 等魔数用十六进制比对真实 homebrew 的 `.nro`
   （例如从 GitHub releases 下载 `ftpd.nro` 做参照），比猜格式快得多。
4. **反汇编定位**：`aarch64-none-elf-addr2line -f -C -e xxx.elf <偏移>`；
   Ryujinx 打印的 `PC/LR` 是**guest 虚拟地址**，减去镜像基址（0x8500000）得到偏移。
   注意：Ryujinx 的崩溃 PC/LR 有时不可靠（会落在函数入口或前一个调用点）。
5. **僵尸进程**：每次 guest 崩溃，Ryujinx 进程会残留（0 线程、无法 kill）。
   它们不影响新实例，但会越积越多（本次积累了 20 个）。

---

## 8. 模拟器（Ryujinx / Nextendo）使用要点

- 快捷方式：`runas /trustlevel:0x20000 "<Ryujinx.exe>" <nro>`（必须以普通权限运行，不能管理员）。
- 从**提权**的 shell 里用同样的 `runas` 通常能无 UAC 降权启动；
  但**批处理文件若含中文路径，必须用 GBK(cp936) 写盘**，否则 `cd`/路径全乱。
- 日志：`portable/Logs/Ryujinx_<版本>_<日期>.log`
- sdcard：`portable/sdcard/`
- 配置：`portable/Config.json`（本次环境里 `MemoryManagerMode=HostMappedUnsafe`）
- 模拟器会把 homebrew 当成 `SystemApplet` 进程加载；`NPDM file not found, using default values` 是正常提示。

---

## 9. 构建环境要点

- 编译器：`C:\devkitPro\devkitA64\bin\aarch64-none-elf-gcc.exe`（Windows 原生，无需 WSL）
- 链接：`-specs=C:/devkitPro/libnx/switch_fixed.specs`
- `devkitPro` 的 pacman 源可能 403（网络受限时）；GitHub codeload 一般可用。
- 我们的 Makefile 走两条链路：
  1. 应用本体：`aarch64-none-elf-gcc` + `build_nro.ps1` 打包
  2. 第三方库：CMake + msys cmake 配置 / 原生 make 编译

---

## 10. 结论：做 Switch 客户端类应用的推荐架构

```
main.c
 ├── UI（console 或 自绘）→ 采集配置（可用 swkbd 系统软键盘）
 ├── 网络（socketInitializeDefault + nifmInitialize）
 ├── 协议/业务（自己的代码，避免重量级第三方库）
 │     └── 加密：自写 RC4/MD5/HMAC + mbedTLS(大数/TLS)
 └── 显示（consoleExit → nwindowGetDefault → framebufferCreate(3) → 线性缓冲直写）
```

**不要**把桌面 Linux 的程序直接搬过来。宁可自己写 1000~2000 行协议代码，
也不要为了省 2000 行而引入一个需要移植 POSIX 的库——移植和调试成本远高于自己实现。

---

## 11. RDP 协议实现要点（本次实测验证）

做 RDP 客户端时，以下几条是**用真实抓包验证过**的，网上很多资料是错的：

### 11.1 X.224 连接请求
```
03 00 00 13  0E E0 00 00 00 00 00  01 00 08 00  0B 00 00 00
TPKT(19)     LI CR dst   src class   NEG_REQ    protocols
```
- `LI` 必须 = 总长-5（0x0E），写错服务器直接 RST
- `requestedProtocols`：mstsc 用 **0x0B**（HYBRID_EX|HYBRID|SSL|RDP）；只给 0x03 时 Windows/xrdp 会选 HYBRID(0x02)，给 0x0B 则选 HYBRID_EX(0x08)
- 服务器会返回 `RDP_NEG_FAILURE`(type 0x03) + 错误码：5 = HYBRID_REQUIRED（必须 NLA）

### 11.2 CredSSP / TSRequest（最容易写错的地方）
真实 mstsc 的 NEGOTIATE：
```
30 37                       TSRequest
  a0 03 02 01 06            [0] version = 6
  a1 30                     [1] negoTokens
    30 2e                   SEQUENCE        ← 第一层
      30 2c                 SEQUENCE        ← 第二层（关键！）
        a0 2a               [0]
          04 28             OCTET STRING (40)
            <NTLMSSP 裸包>
```
三条铁律：
1. `negoTokens` 是 **[1]{ SEQ { SEQ { [0]{ OCTET } } } } 双层 SEQUENCE**，少一层服务器直接 TLS alert 断连
2. token 里放 **裸 NTLMSSP**，不要套 SPNEGO 的 OID 包装
3. NTLM NEGOTIATE 的 domain/workstation 偏移可以是 0（长度为 0 时）

NTLM NEGOTIATE 参考（mstsc 实测）：
`4e544c4d53535000 01000000 B78288E2 0000000000000000 0000000000000000 0A00F4650000000F`
（flags=0xE28882B7，版本 10.0.26100）

### 11.3 调试方法（本次最有效）
**用 mstsc 走一个 TLS 中间人代理**，直接看 CredSSP 明文：
- `tools/rdp_mitm.py`：TCP 转发 + X.224 透传 + TLS 终止（自签证书，Python `cryptography` 生成）
- mstsc 连 `127.0.0.1:13390`，代理转发到真实服务器，日志 `mitm.txt` 里就是完整明文
- 注意：CredSSP 的 pubKeyAuth 绑定了服务器证书公钥，中间人会导致最终认证失败，但**握手前两步的字节完全可用**

### 11.4 模拟器（Ryujinx）的硬限制
在 Nextendo 1.10.0 上实测：
- **阻塞 socket 不可用**（日志明确警告），必须非阻塞 + poll
- 连续创建/关闭 socket 有概率让模拟器宿主直接崩（`Fatal error 0xC0000005 at ARMeilleure`），无 guest 异常记录
- mbedTLS 的 `svcGetInfo(RandomEntropy)` 熵源路径容易触发崩溃 → 改用用户态 PRNG（`mbedtls_ssl_conf_rng` 自定义）
- 结论：**协议层问题应在 PC 上用 Python 验证**（同一套字节），模拟器只做最终 UI 验证；真机才是可靠测试环境

## RDP client: protocol facts proven against a real Windows server (2026-10-22)

Target used for validation: `192.168.1.10:3389` (Windows, user/password).

### What works end to end in `<repo>\tools\rdp_client.py`
X.224 (selected=0x08) -> TLS 1.2 -> CredSSP v6 (NTLMv2 + MIC + pubKeyAuth +
TSCredentials) -> MCS connect -> 4 channel joins -> Client Info -> Demand Active
-> Confirm Active accepted -> Grant Control -> Font Map -> Save Session Info.

### Non-obvious wire details (these cost the most time)
1. **Confirm Active must echo the server's own capability sets.** A hand-built
   capset list is answered with an immediate TCP RST by Windows. Echoing the
   server's capsets is accepted. `use_sec_header` must be False (the server's
   Demand Active carries no security header).
2. **Client share data PDUs put `pduType2` at offset 14, not 17.** Layout that
   Windows accepts from a client:
   `totalLength(2) pduType(2)=0x0017 pduSource(2)=userId shareId(4)
    pad1(1)=0 streamId(1)=1 uncompressedLength(2)=total
    pduType2(1) compressedType(1)=0 compressedLength(2)=0 body`.
   With `pduType2` at 17 the server *silently ignores* every finalization PDU,
   so Grant Control and the Font Map never arrive.
3. `pduSource` in client PDUs = the MCS userId; but the **MCS channel is always
   `MCS_GLOBAL_CHANNEL_ID` (1003)**. Sending on the user channel gets no reply.
4. MCS user-data length has **two encodings**: one byte when < 0x80, otherwise
   `0x80 | (len >> 8), len & 0xFF`. Getting this wrong shifts every following
   PDU by a byte or two.
5. `CTRLACTION_COOPERATE = 0x0004`, `CTRLACTION_REQUEST_CONTROL = 0x0001`.
6. The finalisation batch is **five** PDUs, in this order:
   `0x1F` synchronise, `0x14` cooperate, `0x14` request-control,
   `0x2B` persistent key list (24 zero bytes: 5 numEntries + 5 totalEntries +
   bBitMask + padding), `0x27` font list. `0x28` is the server-only Font Map.
   The persistent key list is required because the server advertises Bitmap
   Cache Host Support; without it no bitmap data is ever sent.
7. The server's own PDUs use a **different** share-data-header field order
   (`pduType2` at 14) than what it accepts from the client. Do not copy the
   server's layout into client PDUs.

### Where it currently stops
Windows accepts the session and grants control, then pushes the framebuffer
through **Fast-Path surface/orders updates**, not legacy Bitmap Updates.
Verified with `xfreerdp` on the same host: 13 `Surface` mentions, zero
`Bitmap Update`. Fast-path framing so far confirmed:

```
00 11 | 00 0c 00 02 00 36 00 00 00 00 36 01 00 00 00   len 17 = hdr(2)+upd(1)+size(2=12)+12
00 05 | 05 00 00                                        len 5  = PTR update, size 0
00 99 | fc 0b f6 19 20 00 02 00 ... (151 bytes)         <- not yet decoded
```

so the frame header is `(action<<14) | length`, length **includes** the 2 header
bytes, and the payload is a bare `(updateHeader, updateSize, data)*` sequence
with **no numberOfUpdates byte**. The third frame onward does not fit that
model yet (its first payload byte 0xFC would be updateCode 12, which is not in
MS-RDPBCGR), which is the exact remaining blocker.

### Reference harness (kept for future work)
`xfreerdp` is installed on the Ubuntu box and works against the same server:
`DISPLAY=:0 XAUTHORITY=/home/user/.Xauthority xfreerdp /v:192.168.1.10
 /u:user /p:password /cert:ignore /size:1024x768 /log-level:TRACE`
FreeRDP 3.5.1 source is cloned at `/tmp/frd` on that box (buildable with
`-DWITH_DEBUG_RDP=ON` to hexdump sent PDUs).

### Bitmap decoding side is ready
`Client.on_update()` decodes 15/16/24/32 bpp bottom-up rectangles into an RGB
framebuffer and `Client.save_png()` writes a PNG without any external library,
so as soon as a legacy Bitmap Update or an uncompressed Surface Bits command
can be framed, an image appears immediately.

### Progress update: handshake fully solved (reverse-engineered with a patched xfreerdp)

Built FreeRDP 3.5.1 on the Ubuntu box with `fprintf` hexdumps injected into
`libfreerdp/core/rdp.c` (outgoing PDUs) and `transport.c` (incoming bytes), plus
`fastpath.c` (parsed update headers). That produced the ground truth the MS
docs alone could not settle.

**The last real bug was `uncompressedLength` in client share-data PDUs.**
It is the length of the PDU *body*, not of the whole PDU:

```
client Synchronize : totalLength=0x16 pduType=0x17 pduSource=userId shareId
                     pad1=0 streamId=1 uncompressedLength=4  pduType2=0x1F ...
client Control     : ... uncompressedLength=8  pduType2=0x14 action=4 ...
client Font List   : ... uncompressedLength=8  pduType2=0x27 entrySize=0x32
```

With the wrong value Windows silently discards the whole finalisation batch, so
Grant Control and the Font Map never arrive. After fixing it our client gets:

```
<- server synchronize
<- server control 04 00 00 00 ...        Cooperate
<- server control 02 00 ef 03 ...        Grant Control, grantId = our userId
<- server font map
<- SET_ERROR_INFO err=0x00000000
<- Save Session Info x2
```

**Fast-Path wire format (confirmed against FreeRDP's own parser):**
* frame header = 2 bytes, length includes those 2 bytes, action in the top 2 bits
* payload = a bare `(updateHeader, [compressionFlags], updateSize, data)*` list,
  there is **no `numberOfUpdates` byte**
* `updateHeader` = `code (4 bits) | fragmentation (2) | compression (2)`
* **when `compression != 0` an extra `compressionFlags` byte precedes
  `updateSize`** -- missing this shifts every subsequent update by one byte and
  was the cause of thousands of "fastpath short" desyncs
* update codes: 0 Orders, 1 Bitmap, 2 Palette, 3 Synchronize, 4 Surface
  Commands, 5 System Pointer Hidden, 6 System Pointer Default, 8 Pointer
  Position, 9 Color Pointer, 10 Cached Pointer, 11 New Pointer, 12 Large Pointer
  (0x0C is a real code -- FreeRDP has `FASTPATH_UPDATETYPE_LARGE_POINTER`)

**Client Confirm Active that Windows accepts**: 20 capability sets, and note
that Windows *rejects* an 88-byte Order capset with `SET_ERROR_INFO 0x10EA`; it
wants the same 28-byte stub the server itself sends. FreeRDP's set, extracted
byte for byte, is: 0001 General, 0002 Order(28B stub), 0003 Bitmap(88B),
0013 BitmapCacheV2(40B), 0008 Pointer, 000D Input, 000F Brush, 0010 GlyphCache,
0014 VirtualChannel(VCChunkSize=1600), 000C Sound, 0009 Share, 000E Font,
0005 Control, 000A ColorCache, 0007 Activation, 001B LargePointer,
001A MultiFragment(0x0009482B), 001C SurfaceCommands, 001D BitmapCodecs,
001E. `sourceDescriptor` = `FREERDP\0` (8 bytes), originatorId 0x03EA.

### What still blocks the image
This Windows host renders the desktop through **RDPGFX** (the
`Microsoft::Windows::RDS::Graphics` dynamic virtual channel) -- confirmed by
`xfreerdp` whose fast-path stream carried only pointer updates (codes 5 and 11)
while its GFX DVC carried the actual frames. A client that does not create the
GFX DVC (ours) is served the legacy path, where the server expresses window
content as **drawing orders** (fast-path update code 0), not as bitmap updates.
`find_bitmap.py` and `find_surface.py` scan a 3 MB capture and find zero
`TS_BITMAP_DATA` and zero `TS_SURFACE_BITS` structures.

So the remaining work is one of:
1. a **drawing-orders renderer** (DSTBLT / PATBLT / SCREENBLT / MEMBLT /
   GLYPH / OPAQUE_RECT ...) -- rdesktop/FreeRDP `libfreerdp/core/orders.c`
   is the reference, several thousand lines, or
2. an **RDPGFX + RemoteFX/AVC decoder** -- heavier, and a poor fit for the
   Switch CPU.

Our bitmap decoder and PNG writer remain ready and unused because the server
never sends bitmap updates on this path.

### Breakthrough: the desktop pixels are now flowing into the client

Three more fixes, each verified against a live Windows server:

1. **Fast-Path frame header is NOT `((b0&3)<<8)|b1`.** FreeRDP's
   `fastpath_read_header_rdp` reads **one flags byte** (bits 7-6 encryption,
   bits 5-2 numberEvents) and then a **PER-encoded length** (one byte, or two
   bytes when the high bit of the first is set). Getting this wrong made every
   frame after the second one desync. `00 99 fc ...` is a **6652-byte** frame,
   not 153.
2. **Fragment enum order** is `SINGLE=0, LAST=1, FIRST=2, NEXT=3` (not the
   intuitive order). Fragmented surface-command updates must be reassembled.
3. **Drop `CAPSTYPE_BITMAP_CODECS` (0x001D) from the Confirm Active.** If the
   client echoes the server's codec list, Windows encodes the desktop as
   NSCodec surface tiles; without it Windows falls back to plain
   **Fast-Path Bitmap updates (update code 1)** which are MS-RLE compressed and
   trivially decodable.

With those, the client receives e.g.

```
<- fp orders size=12
(reassembled code=4 -> 19019 bytes)      surface commands (when 0x001D is sent)
<- bitmap 64x64 bpp=32 COMPRESSED len=193
```

and the tiles arrive as 64x64 RLE bitmaps at destRect (0,0)-(64,64).

### Remaining blocker (very close)
`rle_decode()` in `rdp_client.py` implements the MS-RLE grammar from FreeRDP's
`codec/interleaved.c`. It still overflows on real tiles. Sample tile (193 bytes,
64x64, bpp=32, bitmapLength=193):

```
11f211f211f211f2...  (96 x "11 f2")  ... 0202020202...02 4000
```

`0x11` decodes as REGULAR_BG_RUN length 17, but the following `0xF2` hits
MEGA_MEGA_SET_FG_RUN which wants a 16-bit run length -- impossible here, so the
grammar interpretation is still off for this encoder variant. Note FreeRDP's
`interleaved_decompress` only accepts bpp **8/15/16/24**, never 32, so the
32-bpp instantiation of this codec is not exercised by FreeRDP and the exact
run grammar for 32-bpp tiles needs to be worked out (or the client should be
forced into a 24-bpp session, which also needs the CS_CORE colour fields fixed).

Bitmap-only servers are unaffected: `on_update()` + `save_png()` already decode
uncompressed 15/16/24/32-bpp rectangles correctly.

---

## 2026-10-22 晚（续）：图像仍未出，但把可能范围收敛到"最后一种编码"

上面 `<Remaining blocker>` 里那段 `11f211f2…` 是**我自己的 off-by-2 读错**，
下面全部重写。

### 1. TS_BITMAP_DATA 的真实布局（用 192 个 rect 的完美步长验证过）
```
destLeft(2) destTop(2) destRight(2) destBottom(2)
width(2) height(2) bitsPerPixel(2) flags(2) bitmapLength(2)
[8 字节 TS_CD_HEADER —— 仅当 flags&1 且 !(flags&0x0400)]
bitmapData[bitmapLength]
```
本服务器 `flags = 0x0401` = `BITMAP_COMPRESSION | NO_BITMAP_COMPRESSION_HDR`
⇒ **没有** TS_CD_HEADER，数据紧跟 18 字节头。
验证：一条 update = `4 + 192*(18+193) = 40516` 字节，完全吻合。
（旧代码把第 20..21 字节当 pad2Octets 多跳了 2 字节，所以看到 `11 f2 …`；
真实数据是 `3b f2 11 f2 11 f2 …`。）

### 2. 服务器 Demand Active 的 17 个 capset（RDP_DUMPCAPS=1）
`0009 0001 0014 000A 0008 001B 000E 0002 001D 0003 0012 000D 0017 0018 001A 001C 001E`
* **`0x0002` 才是 28 字节的 TS_BITMAP_CAPABILITYSET**：bpp=0x20=32、receive1/4/8=1、
  desktopWidth=0x0400=**1024**、desktopHeight=0x0300=**768**、resizeFlag=1、
  bitmapCompressionFlag=1、multipleRectangleSupport=1。
  ⇒ 会话被定成 **1024x768 / 32bpp**；CS_CORE 里写 24bpp 被忽略。
* `0x0003` 是 88 字节的 TS_ORDER_CAPABILITYSET（orderSupport[32]）。
  ⇒ 标签和"常识"相反；要改 bpp 得改 `0x0002` 那一个。
* `0x001D`(96B) 带 4 个 bitmap codec（含 NSCodec GUID 8d1bb9a4…e2d6, id=1）。

### 3. 否证 A：不是 interleaved RLE
在 Ubuntu 用 FreeRDP 3.5.1 自带解码器写 harness（`/tmp/rt/rletest.c`）喂真实 193B tile：
```
bpp= 8 -> RleDecompress8to8  failed   (pbSrc+7746 > pbEnd)
bpp=15/16 -> RleDecompress16to16 failed
bpp=24 -> RleDecompress24to24 failed
bpp=32 -> "Invalid color depth 32"   (interleaved_decompress 压根不支持 32bpp)
```
**FreeRDP 自己也解不了同一串字节** ⇒ 不是 RLE。
语法上也是死路：`0xf2` = MEGA_MEGA_FGBG_IMAGE，必然把 `11 f2` 当 16 位长度
= 0x11f2，任何 bpp 都溢出。

### 4. 否证 B：不是 NSCodec
`nsc_process_message` 喂真实 tile：`nsc_stream_initialize: ColorLossLevel=17`
越界 ⇒ 前 20 字节不是 plane 长度表。

### 5. 真实 tile 的特征（1192 个 rect 统计）
* 全部 64x64 / bpp=32 / flags=0x0401。
* **1191 个 tile 首字节都是 0x3b**，只有最大的那个是 0x2b
  ⇒ 首字节跟内容强相关（同一背景色/索引），不是随机数据。
* 最平的 tile（193B）：`3b` + `f2 11`×64 + `02`×64 = 1+128+64，结构极规整。
* 最大的 tile（6146B）：`2b 37 3b 57 44 1c 30 2c 24 27 …`
  **6146 = 2 + 6144 = 2 + 4096(Y) + 1024(Cb) + 1024(Cr)**，字节值平滑(0x1c..0x57)
  ⇒ 很像 YUV 4:2:0 三平面 + 2 字节头（planar 一类）。

### 6. 下一步（按性价比排序）
1. 用 harness 依次试 `planar_decompress` / `progressive_decompress` /
   `zgfx_decompress`（FreeRDP 都有，`/tmp/frd/include/freerdp/codec/`）。
2. 都不行就把 Confirm Active 的 `0x0002` capset 改成 24bpp 再连，
   观察服务器是否换成 24bpp 的 RLE 瓦片（FreeRDP 已验证支持的那条路）。
3. `RDP_BMCOMP=0`（关 bitmapCompressionFlag）**不可行**：服务器回
   `SET_ERROR_INFO 0x10EA` 然后重置连接。

### 7. 本轮新增的工具
* `tools/tiles.bin` —— 40 条 bitmap update 原始 body（自建语料）
* `tools/rlelib.py` —— FreeRDP `interleaved.c` + `include/bitmap.c` 的忠实 Python 移植（可 trace）
* `tools/analyze_tiles.py`、`scan_tiles.py`、`nsc_scan.py` —— 语料解析
* `tools/run_win.ps1` —— 一键跑 Windows 目标（`-Mode/-Secs/-Bpp/-Extra`）
* `ref/codec/{interleaved,bitmap,bitmap_inc,nsc}.c` —— FreeRDP 3.5.1 参考源码
* Ubuntu 侧 `/tmp/rt/rletest.c`、`/tmp/rt/nsctest.c` —— 链接 libfreerdp3 的解码试探程序

---

## 2026-10-22 19:0x：**图像解出来了（真正的 Windows 桌面）**

一句话结论：这些 64x64 tile **不是 MS-RLE，是 RDP6 的 "Planar" 位图编解码器**
（FreeRDP `libfreerdp/codec/planar.c`，即 `planar_decompress`）。
用 FreeRDP 的 planar 解码器解 1192 个 tile → 拼成 1024x768 → 完整的 Win11 桌面
（壁纸、回收站/Edge/服务器图标、任务栏时钟 18:58 2026/10/22 全部正常）。

### 判定过程（都是否证法，很值钱）
| 候选 | 结论 | 依据 |
|---|---|---|
| interleaved RLE | ✗ | FreeRDP 自己的 `interleaved_decompress` 解同一串字节失败；`0xf2`=MEGA 必吞 `11 f2` 当 16 位长度 |
| NSCodec | ✗ | `nsc_stream_initialize` 报 ColorLossLevel 越界 |
| ZGFX | ✗ | `zgfx_decompress` 返回 -1 |
| **Planar** | **✓** | `planar_decompress` OK，拼图 == 真实桌面 |
| 32bpp 的 RLE | ✗ | `interleaved_decompress` 直接拒绝 32bpp |

### 拼图的两个关键点
1. **每个 tile 要竖直翻转**（`dt + (h-1-y)`）。RDP 位图行序是自底向上，
   而 planar 解码出来的缓冲区也是自底向上，直接按 top-down 贴会整块上下颠倒。
   （横向不需要镜像 —— 一开始我把"上下颠倒的中文"看成了镜像。）
2. tile 的 `destLeft/destTop` 直接当目标坐标用即可（1024/64=16，768/64=12，正好 192 tile/帧）。

### Planar 数据的形态（和观察到的字节完全对得上）
* 每个平面开头 1 字节头部：`bit7-6 = nExtraBits`, `bit5-4 = format`, 低 4 位 = RLE 行程数等，
  之后是若干 RLE 段；`PLANE_HEADER` 见 planar.c `planar_read_plane_header`。
* Y/Cb/Cr 三平面，4:2:0 子采样。所以最大的那个 tile
  **6146 = 2 + 4096(Y) + 1024(Cb) + 1024(Cr)**，字节值平滑(0x1c..0x57) —— 完全吻合。
* 平坦 tile 193 字节 = 三平面 RLE 后；首字节 0x3b 是 Y 平面的第一个像素值
  （桌面背景色），所以 1191/1192 个 tile 首字节相同。

### 下一步
1. 把 planar 解码头移植进 `tools/rdp_client.py`（纯 Python），让客户端自己吐 PNG。
2. 再移植到 `src/rdp_*.c` → 重编 `switch_rdp.nro`（Switch 上 1024x768 planar 解码
   是纯整数运算，A57 完全够）。
3. `bitmapCompressionFlag` 必须保持 1（关掉会被服务器以 `SET_ERROR_INFO 0x10EA` 拒绝）。

### 复现命令（Ubuntu 侧）
```
/tmp/rt/tiles2ppm tiles.bin out1.ppm 1     # mode 1 = 每块竖直翻转
```
`tiles.bin` 由 `rdp_client.py` 的 `on_update()` 自动 dump（40 条 update / 1192 个 tile）。

---

## switch_rdp: RDP client working end-to-end (2026-10-22 21:40)

C 版客户端（与 Switch NRO 同一份源码）现在完整跑通 RDP 6 服务器 远程桌面：
TCP → X.224(0x0B) → TLS1.2 → CredSSP v6 → MCS → Client Info → Demand Active
→ Confirm Active → finalization → FastPath 位图更新（RDP6 Planar 解码）
→ 键盘/鼠标 FastPath 输入。1280x720 与 1024x768 都被服务器接受。

### 这一轮修掉的 6 个真 bug（每个都有实机证据）

1. **MCS initiator 用了 user_channel-1002**
   Attach User Confirm 给的是 userId（本例 8），用户通道号 = userId + **1001**（1009），
   而每个上行 PDU 的 initiator 字段必须是**原始 userId（8）**。
   错成 1002 基址 → 服务器拒绝所有 Channel Join（回包全是相同的 9 字节），
   随后 Client Info 被静默丢弃，永远等不到 Demand Active。
   修好后的 join 确认：`02f0803e00000803f103f1`（result=0, initiator=8, ch=1009）。

2. **Demand Active 的解析偏移**
   TS_DEMAND_ACTIVE_PDU 里 shareId 后直接是 lengthSourceDescriptor(2) 和
   lengthCombinedCapabilities(2)，**没有 originatorId**（那是 Confirm Active 才有的）。
   numberCapabilities 在 `14+lsd`，capabilitySets 在 `18+lsd`。

3. **pduType 必须是 0x0011 / 0x0013**
   share control header 的低 4 位是类型、高 4 位是版本：Demand=0x11、Confirm=0x13、Data=0x17。
   另外服务器开头会发一个 `80 00 00 00` 的 License PDU，必须跳过。

4. **FastPath 分片重组缓冲太小**
   服务器把一次整屏刷新拆成 FIRST/NEXT/LAST 三个 ~16 KB 分片（合计 ~47 KB），
   原来只给每个 code 16 KB → 全丢。改成 1 MB 单缓冲后立刻满屏。

5. **Planar 位图不需要翻转**
   rdp_planar_decode 输出已经是 top-down，`dy = dt + y` 即可。
   照抄 Python 的 `(h-1-y)` 会把每个 64 行色带上下镜像（“撕裂”效果）。

6. **键盘/鼠标必须走 FastPath 输入，且 eventCode 是 0/1**
   实测 xfreerdp（本机 Xvfb + 打过 dump 补丁）抓到的真实字节：
   - 键盘 `04 80 05 02 5b`：eventHeader=0x02（extended），eventCode=**0**，keyCode 1 字节
   - 鼠标 `04 80 0a 20 00 08 80 02 90 01`：eventHeader=0x20，eventCode=**1**，
     pointerFlags(2)+x(2)+y(2)，x/y **小端**
   慢速输入 PDU（pduType2=0x1C）在这台服务器上被完全忽略；eventCode 写错会让服务器
   直接断开（SIGPIPE）。
   另外：**按键事件的坐标必须带当前指针位置**，否则 Windows 会把指针拉到 (0,0) 再点。

### 窗口花屏的根因：绘图顺序（orders）

服务器对窗口用的是 FastPath update code **0（TS_ORDER）**，我们只实现位图。
关键点是**不能说“什么顺序都不支持”**——把 orderFlags 一起清掉会让服务器
用 `SET_ERROR_INFO 0x10EA` 直接踢人。
正确做法（已验证）：保留 orderFlags（0x00AA）、只把 `orderSupport[32]` 和
`orderSupportExFlags` 清 0，Windows 就改用纯位图更新下发窗口内容。
Order capset 布局：orderFlags 在 capset 偏移 34，orderSupport 在 36..67，
textFlags/exFlags 在 68..71。

### 其它确认过的字节级事实

- 服务器数据 PDU 的 `uncompressedLength` 写的是**整包长度**（22 而不是 4）；
  客户端写 body 长度或整包长度都能被接受（FreeRDP 写的是 body 长度）。
- X.224 只接受 NLA：offer `0x00` → RDP_NEG_FAILURE code 5 (SSL_REQUIRED_BY_SERVER)。
- 这台服务器的 capset 列表里，88 字节的 `0x000D` 才是 TS_INPUT_CAPABILITYSET
  （flags 0x0375），而 28 字节的 `0x0002` 是 TS_BITMAP。
- 服务器侧 capset 顺序：0x0009 0x0001 0x0014 0x000A 0x0008 0x001B 0x000E 0x0002
  0x001D 0x0003 0x0012 0x000D 0x0017 0x0018 0x001A 0x001C 0x001E（共 435 字节）。

### 文件

- C 源码：`<repo>\src\*.c`，NRO：`<repo>\build\switch_rdp.nro`
- PC 验证用 harness：`tools\host_main.c` + `tools\build_host3.py`（在 192.168.1.17 上编译运行）
- 模拟器：`emulators\Nextendo`（portable），SD 卡目录 `portable\sdcard`
- 自动连接：`sdmc:/switch_rdp.cfg`（四行：host/port/user/password），存在则跳过设置界面
- 运行日志：`sdmc:/switch_rdp_log.txt`
### 实机/模拟器验证结果（b16，2026-10-22 23:40）

Ryujinx 里 **画面已出**，SD 日志：

`
license pdu (20 bytes), ignored
demand active 457 bytes
confirm active: 335 capset bytes, 359 total
finalization sent
SRV synchronize target=1 / COOPERATE / GRANTED_CONTROL
frame 1 (1280x720) incoming=0 rects=0
frame 2 (1280x720) incoming=1 rects=240
SET_ERROR_INFO 0x00000000
frame 50 (1280x720) incoming=49 rects=1821
`

b15 崩溃的根因：`consoleExit()` 之后 `rdp_conn_pump()` 里仍有未加条件的
`LOG()` → `printf`/`consoleUpdate` → libnx console 的 framebuffer 已释放
→ `Invalid memory access at 0x0`（PC 落在 console_sw.c 的 _getFrameBuffer）。
修法：全局 `rdp_console_live`，`consoleExit()` 前置 0，之后只写 SD 日志。

### 已知限制（后续可做）

1. **鼠标光标不显示**：没有处理 TS_POINTER（fastpath code 8/9/10 / slow 0x0005），
   远端指针是不可见的，只能靠点击效果判断位置。
2. **绘图顺序已关闭**：orderSupport 清 0 换来窗口用位图下发（LAN 上带宽可接受，
   ~1.5-3 Mbps），代价是没有 order 优化；orderFlags 不能一起清（会 0x10EA 断线）。
3. **光标更新未实现**：fastpath update code 5/10/11 其实是**指针更新**
   （PTR_NULL / PTR_CACHED / PTR_POINTER），code 4 才是 Surface Commands。
   所以现在远端指针不可见；偶发残影另有原因（大块 blit 用 Surface Commands 下发，code 4）。
4. 剪贴板 / 磁盘重定向（cliprdr、rdpdr 通道已 Join）未实现；声音（rdpsnd）未实现。
5. 每帧整屏 3.7 MB memcpy，实机上建议改成按脏矩形局部上屏。

### 2026-10-23 真机首测：2168-0002 崩溃 → 只读段重定位

现象：真机（Atmosphere 1.12.0 / 19.0.1）启动即报
`Error Code: 2168-0002 (0x4a8)`，PC 落在 libnx `dynamic.c:55 _dynProcessRela`
的 `str x1, [x2, x19]`。

排查路径：
1. `PC - 模块基址(backtrace start address)` 得到偏移 → `addr2line` 定位到 libnx 动态加载器，
   说明崩在 `main()` 之前。
2. 反汇编确认是 `*(base + r_offset) = base + r_addend` 这条存指令。
3. 写 `tools/rela_scan.py` 按段统计 `.rela.dyn`：**1174 个里有 769 个落在只读段**。
4. 根因：`-fPIE` 漏在 CFLAGS 里；更主要的是 `third_party/mbedtls` 当初用
   `-march=armv8-a -mstrict-align -O2`（**没有 -fPIC**）编译，mbedTLS 的常量表里全是函数指针，
   全部落在 `.rodata`。
5. 旁证：同一个 `.nro` 在 Ryujinx 里跑得好好的 —— 模拟器把每个段都映射成可写。

修法：
- CFLAGS 加 `-fPIE`；mbedTLS 用新增的 `tools/build_mbedtls.py`（带 `-fPIC`）重编；
- 链接从自制 `switch_fixed.specs` 换回 libnx 自带的 `nx/switch.specs`（含 `-z text`，
  会把"只读段有动态重定位"变成**链接期错误**）；
- NRO 打包改用 `toolchain/build_nro.py`（Python，段表按 vaddr 间隙算，自带一致性断言）。

修完 `rela_scan` 输出 0，NRO 段表：text 0x0/0xBB000, ro 0xBB000/0x4E000, data 0x109000/0x9000。

（附带教训：中途我误判成"打包脚本把尺寸除以 256"，其实是自己 hex 分组看错了一位；
段表一直是对的。以后先用 `rela_scan` / `-z text` 这类**工具判定**，别靠肉眼数 hex。）

### 2026-10-24 真机：界面正常但桌面里完全没输入

现象：b19 的界面能用（手柄可以选/编辑/连接），按 Y 连上后画面正常，但进了桌面
摇杆不动、触摸无效、USB 键盘也没反应。

根因不在映射，在**阻塞读**：`rdp_conn_pump()` 直接阻塞在 `mbedtls_ssl_read()` 上
（`tls_recv_cb` 里 poll 8 秒），桌面静止时服务器每秒才发一两个 PDU，于是
`run_desktop()` 的循环每秒才转一次 —— 键盘/摇杆/触摸的采样频率被绑在“画面有没有变化”上，
快按一下根本采不到。模拟器里看起来正常只是因为 PDU 更密。

修法：
- `rdp_tls_read()` 增加 `RDP_TLS_AGAIN`；`rdp_tls_set_timeout()` 控制单次 recv 的 poll 预算。
- `rdp_conn_pump(timeout_ms)` 只用这个预算去探**第一个字节**，没数据返回 0；拿到首字节后
  立刻恢复 8 秒预算读完整 PDU（保证不会读到一半丢状态）。
- `run_desktop()` 改成 `pump(3)` + 每轮都采样：键盘（`hidGetKeyboardStates`）、
  触摸（`hidGetTouchScreenStates` → 绝对指针 + 左键）、手柄（右摇杆 → 指针，60 Hz 节流）。
- 顺手补 `-D__SWITCH__`（之前没定义，`#ifdef __SWITCH__` 的 `consoleUpdate` 分支一直没生效）。

## 9. b23（2026-10-24）：菜单、输入法、配置直连，以及一次 devkitPro 事故

### 9.1 起点：编译不过

`make` 报 `sys/socket.h` 找不到 —— `C:\devkitPro\libnx` 被另一项任务更新
devkitPro 时删掉了（源码树 `C:\devkitPro\nx` 还在）。重建过程中又踩了两个坑：
`dynamic.c` 需要 `Elf64_Relr`/`DT_RELR`（本机 elf.h 太老），以及 `bin2s` 因为是
msys 程序、PATH 里没有 `/usr/bin` 而静默失败，导致 `default_font_bin.h` 缺失。
两处都在 03 文档里写了配方。

### 9.2 模拟器彻底变天

升级后的 Ryujinx（Nextendo 1.10.0）有三个新脾气，全部记录在 04 文档：
拒绝提权运行（`runas /trustlevel:0x20000` 才能起来）、要求 NRO 带 ASET 资源段
（否则 `NullReferenceException`）、退出时把 `Config.json` 的
`enable_keyboard`/`enable_mouse`/`start_no_ui` 写回旧值（每次启动前都得重设）。
中途它的更新对话框还抢走键盘，一度让我以为"注入的按键无效"。

### 9.3 功能

- 桌面：右摇杆 y 轴反了 → `my -= ny*16`；ZL/ZR 互换回来 → ZL 左键、ZR 右键。
- L+R 按住 300 ms 打开会话菜单（继续 / 输入文字 / 设置 / 断开 / 退出）。
  菜单复用 console（`fb_teardown` + `consoleInit`），回桌面时
  `framebufferClose` → `framebufferCreate(3)` → 全屏重绘。
- Y 键 → 系统输入法 → 逐字符 UTF-16（FastPath SC_UNICODE）送进远端。
- `sdmc:/switch_rdp.cfg` 首次启动自动生成模板（默认 `Administrator`）；
  主界面按 上上下下左右左右BA 重新读盘并直连 —— 固定默认服务器。
- 流畅度：`TCP_NODELAY`（Nagle 让每个 10 字节的鼠标事件等 40 ms）、
  `blit()` 快路径、`-O3`、没有 PDU 时才 sleep。

### 9.4 教训

- 崩溃/卡死的现场比"功能对不对"更值钱：日志改成追加 + 轮转之后，前两次假死
  的原因（TLS 写无限重试、菜单期 TCP 死锁）才看得见。
- 模拟器的输入通路不可信，判断只能用截图；能确定的都是"远端真的收到了"
  （日志 `kbd usage=...` + 远端搜索框出现字符）。

## 10. b40（2026-10-24）：花屏根因 = interleaved RLE 解错了

### 10.1 证据

b39 加的 `rle dump` 抓到了 RDP 5 位图服务器 的原始载荷：64x64、bpp=16、flags=0x401、
blen=23，而且整屏 220 块**完全一样**：

```
001c840000000000000000f0bc0f840000000000000000
```

用 FreeRDP 的 `RLEDECOMPRESS` 算法在 Python 里跑这 23 字节：正好输出 4096 个像素、
正好把 23 字节读干净、整块纯黑。所以载荷没问题，是旧解码器错了。

旧解码器把流当"颜色+计数"列表读（`0x80` = 重复下一个像素，否则原样像素），
读出来的当然是垃圾；而且它读不满 4096 像素也照样返回成功，于是整屏花屏。

### 10.2 真正的格式（MS-RDPEGDI 的 RLE_BITMAP_STREAM）

不是颜色/计数表，而是**相对上一行**的订单流：

| 订单 | 码 | 语义 |
|------|----|------|
| BG_RUN | 0x00 / 0xF0 | 复制上一行的像素；第一行填黑 |
| FG_RUN | 0x01 / 0xF1 | 上一行 XOR 当前前景色；第一行直接填前景色 |
| FGBG_IMAGE | 0x02 / 0xF2 | 位掩码选前景/背景，同样相对上一行 |
| COLOR_RUN | 0x03 / 0xF3 | 绝对纯色游程 |
| COLOR_IMAGE | 0x04 / 0xF4 | 逐像素原样 |
| LITE_DITHERED_RUN | 0x0E / 0xF8 | 两个像素交替 |
| SET_FG 变体 | 0x0C/0x0D/0xF6/0xF7 | 订单自带一个前景色像素 |
| SPECIAL_* | 0xF9/0xFA/0xFD/0xFE | 固定掩码 / 全白 / 全黑 |

游程长度：regular 取低 5 位（0 → 下一字节 +32）；lite 取低 4 位（0 → 下一字节 +16）；
fgbg 取低 5 位后乘 8（0 → 下一字节 +1）；mega 用后面 2 字节的 16 位小端。
另有"插入前景像素"标志：连续两个 BG_RUN 时，第二个的第一个像素 = 上一行 XOR 前景色。

### 10.3 实现

- `rdp_update.c` 的 `rle_decompress()` 按订单逐条重写：解码进 `s_rle_px[]`（u32，
  stream 顺序），最后统一转 RGB 并**垂直翻转**（流的第一行 = 位图最下面一行）。
- 前景色初值 = 白（不是黑），否则首行里没用 SET_FG 的订单会整块变黑。
- 16bpp 改用 RGB565（raw 路径以前写成 555，绿色少一位）。
- 越界 / 截断 / 保留码一律返回 -1，不 blit：宁可留着旧画面也不画垃圾。
- 超过 128x128 的矩形跳过并打 `rect skip`，用来确认服务器会不会发整屏矩形。

### 10.4 离线验证（不用上机）

`tools/rle_sim.py`（Python，独立实现）+ `tools/_mk_rle_test.py`（把 C 解码器原样
抠出来编译成 host 程序）对同一批向量算同一份 FNV 哈希，10 条全部一致：
抓包载荷、纯色游程、special 首行、XOR 前景、扩展长度 FGBG_IMAGE、32/8/15bpp、
dithered run、截断流（两边都干净失败）。

## 11. b41（2026-10-24）：摇杆/滚轮、中文输入法、隐藏配置

b40 在实机上"完全可用"之后，这一版全部是手感与易用性。

### 11.1 左摇杆指针 + 右摇杆滚轮

- 指针从右摇杆移到**左摇杆**，响应曲线改成平方：小幅度倾斜一次只走 1 像素
  （点图标不飘），推到底约 26 像素/帧。
- 右摇杆上下 = 滚轮。滚轮事件是 FastPath 鼠标事件加 `PTRFLAGS_WHEEL`：
  MS-RDPBCGR 2.2.8.1.1.3.1.1.3 规定 `WheelRotationMask`(0x01FF) 里是**9 位二补码**，
  服务器会做符号扩展，`PTRFLAGS_WHEEL_NEGATIVE` 就是第 8 位。所以一格 = 120：
  上滚 `0x0278`，下滚 `0x0388`（0x200 | 0x188）。
  主机上验证过字节：`04 80 0A 20 78 02 ...` / `04 80 0A 20 88 03 ...`。
- 重复间隔随倾斜量 140 ms → 45 ms，轻推一格一格走，推到底连续滚。

### 11.2 系统键盘能输入中文了

根因不是协议，是 swkbd 的预设：`swkbdConfigMakePresetDefault()` 会把类型钉成
`SwkbdType_QWERTY`（头文件里写得很清楚），所以键盘只给拉丁布局，中文键盘根本不出现。
改成 `swkbdConfigSetType(&c, SwkbdType_All)` 之后 CJK 键盘回来了（地球键切拼音），
文字仍然走 `SC_UNICODE`（UTF-16 码元，逐字符按下+抬起），远端收到的是真正的 `WM_CHAR`。

设置界面里的服务器/端口/账号/密码仍然保持 QWERTY：这些是 ASCII，混进中文输入法反而难打。

### 11.3 硬件键盘（USB / 蓝牙）

- 把 `HidKeyboardState.modifiers`（Shift/Ctrl/Alt/Gui）并进 `keys[3]` 的 bit32..39：
  有的固件只填 modifiers 字段，不并进去就永远发不出组合键。
- 前 24 个键事件写日志：`kbd usage=4 sc=001E ext=0 down=1`。
  这样"USB 键盘没反应"就能一眼分清是**系统根本没上报**（日志一行都没有）
  还是**映射错了**（有 usage 但 sc=0000）。

### 11.4 隐藏配置（秘籍的另一套服务器）

`switch_rdp.cfg` 新增 `alt_server / alt_port / alt_user / alt_pass`：

- `+` 保存可见配置；`+` 和 `-` 同时按 = 把当前字段存进隐藏配置，然后立刻
  `cfg_load()` 把界面恢复成已保存的可见配置（隐藏信息不留在屏幕上，也不用重打）。
- 秘籍用隐藏配置连接；隐藏配置为空就只提示，不再退回默认配置。
- `cfg_write()` 现在会把 `codecs` 一起写回，不会再被保存动作悄悄丢掉。

### 11.5 Makefile：shell 不确定的坑

`if not exist build mkdir build` 只在 cmd.exe 下成立。同一台机器上 PATH 里一旦有
msys2 的 `sh.exe`，make 就改用 sh，于是链接那一步报

```
/opt/devkitpro/msys2/usr/bin/sh: -c: line 2: syntax error: unexpected end of file
```

改成用 `$(PYTHON) -c "import os; os.makedirs(...)"` 建目录、用 Python 删中间文件，
两种 shell 都能跑（`clean` 仍然只删显式文件，绝不 glob `build/*`）。

### 11.6 字体

新增的中文串（"备用配置已保存 已恢复界面配置"等）重新跑了 `tools/gen_ui_font.py`，
281 个字形。**改完界面文字一定要重跑**，否则新字是空白。

## 12. b43（2026-10-24）：起名 RDP、图标、打包、开源

功能到此已经够用（真机实测：左摇杆指针手感、右摇杆滚轮、中文输入法、隐藏配置全部通过），
这一版只做"能发出去"的事。

### 12.1 名字与图标

- 显示名统一成 **RDP**：主界面标题、连接页标题、启动日志、NACP 标题。
  配置和日志文件名仍然是 `switch_rdp.cfg` / `switch_rdp_log.txt`——改名会让老配置失联，
  没必要为了好看让用户重填一遍隐藏配置。
- `tools/gen_icon.py` 画 256x256 JPEG：深蓝圆角砖 + 蓝框显示器 + 桌面色条 + 白色指针 +
  `RDP` 字。图标是**代码生成**的，改一行重跑就有新图，不用外部素材。

### 12.2 NRO 资源段（图标 + NACP）

`tools/build_nro.py` 早就写 ASET 段（magic + version + 3×(u64 off, u64 size)），
这版补了三件事：

- `--name=` / `--publisher=`，标题写进 NACP 的 16 个语言槽（以前硬编码 `switch_rdp`）；
- icon/NACP 之间做 8 字节对齐；
- `find_icon()` 先找仓库里的 `assets/icon.jpg`（以前写死 `../APP/switch_rdp/assets`，
  把打包脚本挪进 `tools/` 之后图标就静默丢了——NRO 尺寸从 1199544 掉到 1190088，
  正是那 15 KB 图标的差距，靠 `tools/check_nro.py` 才一眼看出来）。

`tools/check_nro.py` 是这轮新写的体检脚本：NRO 头 magic/size、三段是否加得起来、
ASET 偏移是否落在文件内、icon 是不是 256x256 JPEG、NACP 是不是 0x4000 且标题非空。
以后每次打包跑一遍，比上机试错便宜太多。

### 12.3 打包与仓库

- `tools/make_release.py b43` 产出 `dist/RDP_b43.nro`（带图标）、
  `RDP_b43_bare.nro`（不带资源段）、`RDP_b43.zip`（解压到 SD 根就是
  `switch/RDP.nro` + 配置模板 + README + LICENSE）。
- 仓库：`git init` 在项目根，`.gitignore` 用**白名单**保留 `tools/` 里 13 个发布用脚本
  （字体/图标生成、NRO 打包与校验、RLE 对拍、安装脚本），
  `out/ ref/ scratch/ shots/ third_party/ docs/baselines/` 全部排除——
  这些是 bring-up 期间的实验产物，不该进历史。
- 发布前把文档里的测试机地址、账号、密码全部换成了示例值。

### 12.4 教训

- **打包脚本一挪位置，相对路径就会静默失效**（图标丢了但构建"成功"）。
  凡是"可选资源"，产物尺寸必须校验，否则丢了都不知道。
- Makefile 里的 `MBEDTLS` / `TOOLCHAIN` / `PYTHON` 改成 `?=` 可覆盖，
  别人的机器不用再改文件就能编。
