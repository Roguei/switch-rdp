# 02 — 协议字节级笔记（MS-RDPBCGR 实测版）

> 这份文档记录的是**在这台真实 Windows 服务器上抓到的字节**，不是抄规范。
> 每条后面都有"怎么验证的"。改了任何相关代码，照这里的验证方法回归。

目标服务器：`192.168.1.10:3389`（RDP 6 位图服务器）。客户端请求 1280×720 或 1024×768 都被接受。

---

## 0. 一句话流程

```
TCP → X.224(offer 0x0B) → TLS1.2 → CredSSP(v6) → MCS Connect Initial(451B)
    → Connect Response(108B) → Erect Domain → Attach User(0x28) → 6×Channel Join(0x38)
    → Client Info(66B) → [License PDU 丢弃] → Demand Active(457B)
    → Confirm Active(359B) → 5 个 finalization 数据 PDU
    → 服务器开始推 FastPath 位图；键盘/鼠标走 FastPath 输入
```

---

## 1. X.224 / 协议协商

请求（19 字节）：

```
03 00 00 13 0E E0 00 00 00 00 00 01 00 08 00 0B 00 00 00
                                              ^^ offer = 0x0B
```

`0x0B` = `PROTOCOL_HYBRID_EX | HYBRID | SSL | RDP`，与 mstsc 一致。

服务器应答（19 字节）：

```
03 00 00 13 0E D0 00 00 12 34 00 02 1F 08 00 08 00 00 00
                         ^^^^^^ RDP_NEG_RSP，type=0x02，长度 8
                                               ^^^^^^^^^^^ selected = 0x08 (HYBRID_EX)
```

**这台服务器强制 NLA**：offer 只给 `0x00`（标准 RDP 安全）时回 `RDP_NEG_FAILURE`，
`failureCode = 5`（`SSL_REQUIRED_BY_SERVER`）。所以别想着绕过 CredSSP。

解析代码：`rdp_session.c: x224_connect()`，从 `raw[11]` 开始，`raw[11]==0x02` 表示成功、
`0x03` 表示失败，selected = `raw[15..18]`（小端）。

---

## 2. TLS

- mbedTLS，`MBEDTLS_SSL_VERIFY_NONE`（RDP 服务器都是自签）。
- 最低 TLS 1.0，实际协商 TLS 1.2。
- **Switch 上没有可用的熵源**（`svcGetInfo(RandomEntropy)` 在模拟器/部分实机不可靠），
  所以用自带的 xorshift PRNG（`rdp_tls.c: rdp_rng`）。
- BIO 自己做：`poll()` + 阻塞式 send/recv（`tls_send_cb` / `tls_recv_cb`）。

CredSSP 需要**服务器的 RSA 公钥**（pubKeyAuth 要绑 TLS 端点）：
`rdp_tls_server_pubkey()` 从 mbedTLS 证书里取出 `SEQUENCE { INTEGER n, INTEGER e }` 的 DER，
长度用正确的 `0x81/0x82` 编码。**大小必须是 270 字节**（本服务器的 2048 位密钥）。
少 2 字节（268）说明 DER 长度编码写错了。

---

## 3. CredSSP（NLA）

六个来回，`rdp_credssp.c: nla_authenticate()`：

| 步骤 | 内容 | 实测大小 |
|---|---|---|
| 1 | `NEGOTIATE`（NTLMSSP negotiate + 版本/域信息） | 40 B |
| 2 | 收 `CHALLENGE`，构造 `AUTHENTICATE`（NTLMv2 响应 + MIC） | 360 B |
| 3 | 收 `pubKeyAuth` 结果，发 `pubKeyAuth`（含 270B 公钥） | — |
| 4 | 收 `err=0` | 59 B |
| 5 | 发 `TSCredentials` | **49 B** |
| 6 | 收 `err=0` | 4 B |

关键细节：

- 必须支持 **SIGN / SEAL / KEYEXCH**，之后所有 CredSSP 报文用 RC4 加封（`seal`/`unseal`）。
- `pubKeyAuth` 的哈希 = `SHA256("CredSSP Client-To-Server Binding Hash\0" || nonce || pubkey)`。
- `TSCredentials` 必须是完整的三层嵌套：
  `30 2f a0 03 02 01 01 a1 28 04 26 …`，**47 字节是错的（少了 TSPasswordCreds 的 OCTET STRING 包装），49 才对**。
- 这两处都能和 `tools/rdp_client.py` 的输出逐字节对比（`RDP_NLA_DEBUG=1`）。

---

## 4. MCS

### 4.1 Connect Initial（451 字节）

BER：`7F 65 <len> 04 01 01 04 01 01 01 01 FF <domainParameters×3> 04 <gcc>`。

- `domainParameters` 三份（min/max/target），每个都是 `30 …` 的 SEQUENCE。
- **BER INTEGER 高位为 1 时必须补 0x00**：`0xFFFF` 要写成 `02 03 00 FF FF`。
  写成 `02 02 FF FF` 会被当成 -1，GCC 整包被服务器丢掉（表现为 TLS close）。

GCC `ConferenceCreateRequest`：

```
00 05 00 14 7C 00 01              h221 头（"Duca" 之前的固定部分）
00                                choice conferenceCreateRequest
08                                userData 存在
00 10                             numeric string "1"
00                                padding
01                                1 个 user data set
C0                                h221NonStandard
00 "Duca"                         客户端→服务器的 key
<PER 长度> <user data blocks>
```

user data 块（顺序**必须**是 core → security → net → cluster）：

| 块 | tag | 内容要点 |
|---|---|---|
| CS_CORE | 0xC001 | 230 字节。版本 `0x00080004`、桌面宽高、`0xCA01/0xAA03`、clientName(32B UTF-16)、键盘布局、bpp、`earlyCapabilityFlags`、**`serverSelectedProtocol = 0x08`**、`desktopPhysicalWidth/Height`、缩放 100/100 |
| CS_SECURITY | 0xC002 | `encryptionMethods=0, extEncryptionMethods=0`（8 字节） |
| CS_NET | 0xC003 | 通道数 + 每通道 8 字节名 + **options `0x80800000`** |
| CS_CLUSTER | 0xC004 | `flags=1`（REDIRECTION_VERSION? 这里给 1）+ `RedirectedSessionId=0` |

`serverSelectedProtocol` 漏填 = 服务器直接断（这是最难查的一个）。

### 4.2 Connect Response（108 字节）

```
02 F0 80 7F 66 66 0A 01 00 02 01 00 30 1A 02 01 22 02 01 03 02 01 00 02
01 01 02 01 00 02 01 01 02 03 00 FF F8 02 01 02 04 42 00 05 00 14 7C 00
01 2A 14 76 0A 01 01 00 01 C0 00 4D 63 44 6E 2C 01 0C 10 00 0D 00 08 00
0B 00 00 00 04 00 00 00 03 0C 10 00 EB 03 04 00 EC 03 ED 03 EE 03 EF 03
02 0C 0C 00 00 00 00 00 00 00 00 00
```

能读出来的：

- `io_channel = 0x03EB = 1003`
- `channelCount = 4`，通道号 1004/1005/1006/1007（顺序对应我们请求的 rdpdr/cliprdr/rdpsnd/drdynvc）
- `rsa_bits = 0`（走 TLS，不加密钥交换参数）

### 4.3 小 PDU

```
Erect Domain Request: 03 00 00 0C 02 F0 80 04 01 00 01 00
Attach User Request : 03 00 00 08 02 F0 80 28
Attach User Confirm : 02 F0 80 2E 00 00 08      ← 最后两字节 = userId = 8
Channel Join Request: 03 00 00 0C 02 F0 80 38 <init lo hi> <chan lo hi>
Channel Join Confirm: 02 F0 80 3E 00 00 08 03 F1 03 F1   (result=0, initiator=8,
                                                          requested=1009, channel=1009)
```

**join 的 initiator 是原始 userId（8），不是通道号**；目标通道用**绝对通道号**（1009/1004…/1003）。
每个 join 必须读一次确认再发下一个（连发会错位）。

### 4.4 Send Data（上行）

```
03 00 <total> 02 F0 80 64 <init lo hi> <chan lo hi> 70 <PER 长度: 0x80|hi, lo> <payload>
```

`0x64` = `SendDataRequest`（下行是 `0x68` = Indication）。
PER 长度：1 字节（≤0x7F）或 `0x80|hi, lo` 两字节；客户端**一直用两字节形式**是合法的
（Client Info 就是这么发的，服务器接受）。

---

## 5. Client Info PDU

特殊：**带 4 字节安全头（`40 00 00 00`），没有共享数据头**。

```
40 00 00 00                      TS_SECURITY_HEADER（flags=0x40 SEC_INFO_PKT）
09 04 00 00 10 00 00 00          codePage=0x0409, flags=0x10 (INFO_UNICODE)
<5×u16 长度>                     domain, user, pass, shell, workingDir（UTF-16 + 双 0 结尾）
<TS_EXTENDED_INFO>               地址族 2 / cbClientAddress 2 / "" / cbClientDir 2 / ""
```

本服务器实测 **62 字节 TS_INFO + 4 = 66 字节**。发在**通道 1003**、initiator 8。

---

## 6. Demand Active

先来一个 **License PDU**，必须跳过：

```
80 00 00 00 FF 03 10 00 07 00 00 00 02 00 00 00 04 00 00 00
```

然后是 Demand Active（457 字节），`TS_DEMAND_ACTIVE_PDU`：

```
totalLength(2) pduType(2)=0x0011 pduSource(2)
shareId(4)                          ← 本服务器 = 0x000103EA
lengthSourceDescriptor(2)
lengthCombinedCapabilities(2)
sourceDescriptor[lsd]
numberCapabilities(2)  ← 偏移 14+lsd
pad2Octets(2)
capabilitySets         ← 偏移 18+lsd
```

**注意：Demand Active 没有 `originatorId`**（那是 Confirm Active 才有的字段）。
我在这里错过两次：把 lsd/lcc 读反、把 numberCapabilities 读成 16+lsd。

`pduType` 的编码：**低 4 位是类型，高 4 位是版本**，所以 Demand=0x11、Confirm=0x13、
Deactivate All=0x14、Data=0x17。

本服务器的 capset 列表（435 字节）：

```
0x0009(8) 0x0001(24) 0x0014(12) 0x000A(8) 0x0008(10) 0x001B(6) 0x000E(8)
0x0002(28) 0x001D(96) 0x0003(88) 0x0012(8) 0x000D(88) 0x0017(8) 0x0018(11)
0x001A(8) 0x001C(12) 0x001E(8)
```

两个容易搞错的：

- **28 字节的 `0x0002` 才是 TS_BITMAP**（bpp=32、1024×768）。
- **88 字节的 `0x000D` 才是 TS_INPUT**，flags=0x0375。
  （8 字节的 `0x000D` 才是经典 Input 结构；这里加了键盘描述字段所以 88 字节。
  只按 tag 判断长度会改错东西。）

---

## 7. Confirm Active（我们发 359 字节）

结构（注意比 Demand Active 多一个 `originatorId`）：

```
totalLength(2) pduType(2)=0x0013 pduSource(2)=user_channel(1009)
shareId(4)  originatorId(2)=0x03EA
lengthSourceDescriptor(2)=4  lengthCombinedCapabilities(2)=ncaps+4
"RDP\0"  numberCapabilities(2)  pad2Octets(2)  capsets
```

回显后的尺寸：**335 字节 capset + 24 字节头/描述符 = 359 字节**。

策略：**回显服务器给的所有 capset**，除了：

1. **丢掉 `0x001D`（Bitmap Codecs）** —— 否则服务器想用我们没实现的 codec 画。
2. **`0x0003`（Order，88 字节）**：把 `orderSupport[32]`（capset 偏移 36..67）和
   `orderSupportExFlags`（偏移 70..71）清零，**但保留 `orderFlags`（偏移 34..35）**。
   - 不清 → 服务器用绘图顺序（FastPath update code 0）画窗口，我们只认位图 → 窗口花屏。
   - 连 `orderFlags` 一起清 → `SET_ERROR_INFO 0x10EA` 直接断线。
3. **`0x000D`（Input）**：把 flags OR 上 `0x0001|0x0008|0x0020`
   （SCANCODES + FASTPATH_INPUT + FASTPATH_INPUT2），确保服务器认我们能打字。

对照参考：xfreerdp 3.5.1 的 Confirm Active（541 字节、20 个 capset）可以拿
`tools/an_cf.py` 从抓包日志里还原出来。

---

## 8. 客户端 finalization（5 个数据 PDU）

| pduType2 | body | 说明 |
|---|---|---|
| `0x1F` | `01 00 00 00` | Client Synchronize（targetUser=1） |
| `0x14` | `04 00 00 00 00 00 00 00` | Client Control: Cooperate |
| `0x14` | `01 00 00 00 00 00 00 00` | Client Control: Request Control |
| `0x2B` | 24 字节 0 | Client Persistent Key List |
| `0x27` | `00 00 00 00 03 00 32 00` | Client Font List |

顺序必须是这样。发完服务器会回 `Synchronize` / `Control COOPERATE` / `Control GRANTED_CONTROL`，
然后开始推画面。

### 共享数据头（18 字节，所有上行数据 PDU 都用）

```
totalLength(2) pduType(2)=0x0017 pduSource(2)=user_channel
shareId(4) pad1(1)=0 streamId(1)=1
uncompressedLength(2) pduType2(1) compressedType(1)=0 compressedLength(2)=0
```

- `uncompressedLength`：服务器自己发的是**整包长度**（22 而不是 4），客户端写 body 长度
  （FreeRDP 风格）或整包长度都被接受。**写 0 或乱写会被静默丢弃**。
- 本会话**没有** 4 字节安全头（有安全头时 PDU 以 `00 00 00 00` 开头）。

---

## 9. FastPath 下行（画面）

外层：

```
1 字节: bits7-6 加密, bits5-2 numberOfEvents, bits1-0 action(0=fastpath)
PER 长度: 1 字节，或 0x80|hi, lo（长度含这 1+length 字节自身）
```

更新项（**没有 numberOfUpdates 字节**，就是连续排列）：

```
updateHeader(1): code(0-3) fragmentation(4-5) compression(6-7)
[compressionFlags(1)]   仅当 compression != 0
size(2, 小端)
data
```

`fragmentation`：`SINGLE=0, LAST=1, FIRST=2, NEXT=3`。

**一次整屏刷新会被拆成 3 个 ~16 KB 的分片**（FIRST → NEXT → LAST），合计 ~47 KB。
重组缓冲必须 ≥ 64 KB（本项目给了 1 MB）。实测分片头示例：

```
21 61 3F   →  0x21 = code 1(bitmap) | frag 2(FIRST)；size = 0x3F61 = 16225
31 61 3F   →  0x31 = code 1 | frag 3(NEXT)；     size = 16225
11 xx yy   →  0x11 = code 1 | frag 1(LAST)
```

`code`（MS-RDPBCGR 2.2.9.1.2.1）：

| code | 含义 | 我们 |
|---|---|---|
| 0 | Orders（绘图顺序） | 不处理；靠 capset 让服务器不发 |
| 1 | Bitmap | ✅ |
| 2 | Palette | 忽略（真彩色会话用不到） |
| 3 | Synchronize | 忽略 |
| 4 | Surface Commands | ❌ 忽略（进阶优化用） |
| 5 | Pointer: NULL（隐藏光标） | ❌ 忽略 → 光标不可见 |
| 6 | Pointer: Default（默认箭头） | ❌ 忽略 |
| 8 | Pointer: Position（只移动） | ❌ 忽略 |
| 10 | Pointer: Cached（复用缓存图） | ❌ 忽略 |
| 11 | Pointer: Pointer（新光标图） | ❌ 忽略 |

**注意**：实际会话里 5/10/11 出现得很频繁 —— 那是**光标更新**，不是 surfcmds。
实现光标见 05_ROADMAP P0-2。

---

## 10. 位图数据（TS_BITMAP_DATA）

每个矩形 18 字节头：

```
destLeft(2) destTop(2) destRight(2) destBottom(2)
width(2) height(2) bitsPerPixel(2) flags(2) bitmapLength(2)
```

- 本服务器 `flags = 0x0401`（`BITMAP_COMPRESSION | NO_BITMAP_COMPRESSION_HDR`）
  → **没有 TS_CD_HEADER**，压缩数据紧跟 18 字节头。
- 一个矩形实测 `4 + 192*(18+193) = 40516` 字节（192 个 64×64 平铺块）。
- 平铺块 64×64，`bpl=32`（每行 32 字节？按平面算），最小块 193 字节：
  `3B` + `F2 11`×64 + `02`×64；最大块 6146 字节 = 2 + 4096(Y) + 1024(Cb) + 1024(Cr)。

### 10.1 RDP6 Planar 解码（`rdp_planar.c`）

块首字节 `fmt`：

```
bits0-2 CLL（色损等级）  bit3 CS（4:2:0 色度子采样）
bit4 RLE                 bit5 NA（1 = 无 alpha，即不传 alpha 平面）
```

平面顺序：`[alpha] Y Co Cg`。每个平面自己带长度（RLE 自定界）。

RLE 编码（`rle_plane()`）：

```
控制字节: 低 4 位 = run（重复次数），高 4 位 = raw（原样字节数）
  run==1 → run = raw + 16, raw = 0
  run==2 → run = raw + 32, raw = 0
第 0 行: raw 字节是绝对值
第 y 行: raw 字节是**相对上一行同列的有符号 delta**（zigzag: dv&1 → -((dv>>1)+1) 否则 dv>>1）
         run 部分沿用该行最后一个 delta
```

YCoCg→RGB（`shift = CLL-1`）：

```
cg = (int8)(Cg << shift); co = (int8)(Co << shift);
t  = Y - cg;  R = t - co;  G = Y + cg;  B = t + co;
```

**坐标系**：平面行是**底朝上**的，解码器在转换时已经翻好（`h-1-y`），
所以 `rdp_update.c: blit()` 直接 `dy = dt + y`。**不要再翻一次**（会得到"每条 64 行色带上下镜像"的花屏）。

未压缩矩形（`flags & 1 == 0`）走 `blit_raw()`，那里的行**确实是** bottom-up，需要翻转。

### 10.2 走过的弯路（别再试）

| 尝试 | 结果 |
|---|---|
| 交错 RLE（interleaved） | FreeRDP 自己的 `interleaved_decompress` 也在同样字节上失败（`pbSrc+7746 > pbEnd`）；`0xF2`（MEGA_FGBG_IMAGE）永远要求 16 位长度 |
| NSCodec | `nsc_stream_initialize: ColorLossLevel=17` 越界 |
| ZGFX | `zgfx_decompress` 返回 -1 |
| 禁用位图压缩（capset 里把 compression flag 清 0） | 服务器 `SET_ERROR_INFO 0x10EA` 断线 |

---

## 11. FastPath 上行（键鼠）

裸载荷，**没有 TPKT / X.224 / MCS / 共享数据头**：

```
1 字节: action(低2位)=0 | (numberEvents << 2)
PER 长度: 0x80|hi, lo（长度含自身）
事件...
```

### 键盘

```
04 80 05 02 5B        Win 按下
04 80 05 03 5B        Win 抬起
04 80 05 00 12        E  按下
04 80 05 01 12        E  抬起
        ^  ^
        |  eventHeader：bit0 = RELEASE，bit1 = EXTENDED，bit2 = E1 前缀
        eventCode = 0（键盘）—— 就是 0，别加 0x20
             keyCode(1 字节) = PC set-1 扫描码
```

### 鼠标

```
04 80 0A 20 00 08 80 02 90 01
            ^  ^^^^^  ^^^^^  ^^^^^
            |  flags  x=0x0280 y=0x0190   ← x/y 小端
            eventHeader = 0x20（eventCode 1）+ flags(0)
pointerFlags: 0x0800 MOVE, 0x1000/0x2000/0x4000 左/右/中键, 0x8000 DOWN
```

**两个致命细节**：

1. **eventCode 写反会被断开**（keyboard=1 / mouse=0 会让服务器解析失败，随后写 socket 收到 SIGPIPE）。
2. **按键事件必须携带当前指针坐标**（不要给 0,0），否则 Windows 先把光标拉到左上角再点，
   表现为"点击完全没反应"。

### 慢速输入没人理

`pduType2 = 0x1C` 的慢速输入 PDU（`numberEvents(2) pad2Octets(2) event(12)`）在这台服务器上
**完全被忽略**（发了 20 多次，画面毫无反应）。必须走 FastPath。

---

## 12. 其它已验证事实

- **服务器数据 PDU 的 `streamId` = 2**；客户端用 1 没问题。
- 服务器会主动发 `0x001F`(sync) / `0x0014`(control) / `0x0028`(font map) / `0x002F`(error info)，
  都是慢速路径、带共享数据头。
- `SET_ERROR_INFO 0x00000000` 是正常的（表示没有错误），别当失败。
- 客户端的 `pduSource` 一律填 `user_channel`（1009），与 FreeRDP 的 `mcs->userId` 一致。
- 服务器接受 1280×720 的会话尺寸（CS_CORE 请求多大就开多大）。

## 10. FastPath 输入事件：UNICODE（SC_UNICODE，eventCode = 4）

用于把系统输入法的字符原样送进远端（不依赖本地键盘布局）。

```
偏移 字节        含义
0    1           fpInputHeader：action=FASTPATH(0)，numberOfEvents=1 → 0x04
1..2 2           长度（0x80 | hi, lo），整个 PDU 6 字节
3    1           eventHeader = eventFlags | (eventCode << 5)
                 eventCode 4 → 0x80；按下 flags=0x00，抬起 flags=0x01
4..5 2           unicodeCode（UTF-16 码元，小端）
```

- 参考来源：FreeRDP `libfreerdp/core/fastpath.h` 的
  `FASTPATH_INPUT_EVENT_SC_UNICODE = 0x4`，以及 `fastpath_input_event` 的
  unicode 分支（payload 2 字节）。
- 一个字符 = 一次按下 + 一次抬起（两个 PDU）。增补平面（> U+FFFF）拆成
  UTF-16 代理对，每个码元各一对按下/抬起。
- 回车送 `0x000D`（不是 `0x000A`），否则记事本之类不换行。

实现在 `rdp_input_unicode()`（编码）+ `rdp_conn_unicode()`（发送）+ 
`main.c:send_text_to_remote()`（UTF-8 → UTF-16 拆分）。
