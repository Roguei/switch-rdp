# 01 — 架构与源码地图

## 1. 分层

```
┌──────────────────────────────────────────────────────────┐
│ main.c        UI（收件箱/表单）、SD 配置、帧缓冲上屏、输入 │
├──────────────────────────────────────────────────────────┤
│ rdp_session  rdp_conn：连接编排 + 共享数据 PDU + 收包泵   │
├────────────┬──────────────┬───────────────┬──────────────┤
│ rdp_credssp│ rdp_mcs      │ rdp_update    │ rdp_input    │
│ NLA        │ MCS/X.224    │ 更新解析/重组 │ 输入事件编码 │
├────────────┴──────────────┴───────┬───────┴──────────────┤
│ rdp_tls (mbedTLS)                 │ rdp_planar (RDP6)    │
├───────────────────────────────────┴──────────────────────┤
│ libnx: socket/poll · framebuffer · hid · fsdev           │
└──────────────────────────────────────────────────────────┘
```

**线程模型：单线程。** `rdp_conn_connect()` 阻塞跑完整套握手，之后主循环里
`rdp_conn_pump()` 阻塞读一个 PDU（TLS 层用 `poll()` 等，最多 8 s），返回后立刻上屏。
没有后台线程、没有锁。

## 2. 源码地图

| 文件 | 行数 | 职责 | 关键函数 |
|---|---:|---|---|
| `src/main.c` | 467 | 控制台 UI、SD 配置、帧缓冲、按键/摇杆 → RDP | `run_session` `run_desktop` `load_sd_config` `fb_blit` `tlog` |
| `src/rdp_session.c` | 1100 | 连接编排、共享数据头、Confirm Active、收尾、泵、输入封装 | `rdp_conn_connect` `send_confirm_active` `send_data_pdu` `send_finalization` `recv_pdu` `rdp_conn_pump` |
| `src/rdp_mcs.c` | 1478 | TPKT/X.224/MCS、GCC 组包解析、SendData 组包解包 | `rdp_mcs_build_connect_initial` `rdp_mcs_parse_connect_response` `rdp_mcs_build_attach_user` `rdp_mcs_build_channel_join` `rdp_mcs_build_send_data` `rdp_mcs_extract_send_data` |
| `src/rdp_credssp.c` | 700 | CredSSP v6 客户端（NTLMv2/MIC/RC4/pubKeyAuth/TSCred） | `nla_authenticate` |
| `src/rdp_crypto.c` | 370 | MD4/MD5/HMAC-MD5/RC4/DES 等 NTLM 需要的算法 | — |
| `src/rdp_tls.c` | 261 | mbedTLS 封装、阻塞式 BIO、服务器公钥导出 | `rdp_tls_new` `rdp_tls_handshake` `rdp_tls_read/write` `rdp_tls_server_pubkey` |
| `src/rdp_update.c` | 241 | FastPath 更新头解析、FIRST/NEXT/LAST 重组、位图上屏、裸位图 | `rdp_update_fastpath` `rdp_update_bitmap` `blit` `blit_raw` |
| `src/rdp_planar.c` | 255 | RDP6 Planar（YCoCg）解码 | `rdp_planar_decode` |
| `src/rdp_input.c` | 422 | FastPath 键盘/鼠标事件；HID usage / ASCII → PC set-1 扫描码 | `rdp_input_keyboard` `rdp_input_mouse_move` `rdp_input_mouse_button_at` `rdp_scancode_from_hid` `rdp_scancode_from_ascii` |
| `src/rdp_bitmap.[ch]` | 739 | 帧缓冲表面的类型定义（早期位图实验遗留，当前只用 `rdp_surface`） | — |
| `src/rdp_nla.c` / `rdp_proto.c` / `posix_stubs.c` | — | **不参与编译**（历史遗留，Makefile 里没有） | — |

`rdp_conn` 结构（`rdp_session.c` 顶部）里的关键字段：

```c
rdp_cfg cfg;            // host/port/user/pass/domain/width/height
int sock; rdp_tls_t* tls;
rdp_mcs_info mcs;       // channels[], io_channel(1003), user_channel(1009), rsa_bits
rdp_surface surface;    // pixels = framebuffer, RGBA8888
uint32_t share_id;      // 0x000103EA（来自 Demand Active）
int use_sec_header;     // 该会话是否需要 4 字节安全头（本服务器：不需要）
uint8_t* frag;          // 1 MB，FastPath 分片重组缓冲
int mouse_x, mouse_y;   // 最近一次指针位置（按键事件必须带上）
```

## 3. 连接时序（`rdp_conn_connect`）

```
tcp_connect()
x224_connect()               → 选协议（0x08 = HYBRID_EX）
rdp_tls_new + handshake      → TLS 1.2
nla_authenticate()           → CredSSP，err=0
rdp_mcs_build_connect_initial + tr_send
tr_recv_tpkt → rdp_mcs_parse_connect_response   → io_channel / channels[]
erect domain
attach user request → 读确认 → user_channel = userId + 1001
6 次 channel join（自己的 user channel、4 个虚拟通道、io channel）
send_client_info()
```

之后进入 `rdp_conn_pump()`：

```
recv_pdu()   → 先读 1 字节：
               0x03 → TPKT（慢速路径）→ 剥 X.224(3B) → 0x68 SendData
                        → 剥 initiator(2)+channel(2)+flags(1)+PER长度
               其它 → FastPath（1 字节头 + PER 长度）→ 后续都是 update
分发：
   慢速 + License PDU(80 00 00 00) → 丢弃
   可选 4 字节安全头（00 00 00 00 开头且不是 00 00 11 00）
   pduType 0x0011 → Demand Active → send_confirm_active + send_finalization
   pduType2 0x0002 → Update（位图）
   pduType2 0x001F / 0x0014 / 0x002F / 0x0028 → 同步/控制/错误/字体表
   FastPath → rdp_update_fastpath(ctx)（code 1 = 位图）
```

## 4. 数据流（画面）

```
TLS record → 一个或多个 FastPath update
  updateHeader: code(0-3) frag(4-5) comp(6-7)
     code==1 位图：
        SINGLE  → rdp_update_bitmap()
        FIRST/NEXT/LAST → 拼进 c->frag（1 MB）再 rdp_update_bitmap()
rdp_update_bitmap()
  循环 numberRectangles 个 TS_BITMAP_DATA（18 字节头）
  压缩(flags&1) → rdp_planar_decode(tile, w, h, s_tile) → blit(dl, dt, w, h)
  未压缩      → blit_raw()（行是 bottom-up，需要翻转）
blit()  把 s_tile（top-down，w*h*3）拷进 surface（RGBA，stride = w*4）
```

**注意**：`s_tile` 是 `static uint8_t[128*128*3]`，所以单个矩形最大 128×128。
服务器实际发的是 64×64 的 tile。

## 5. 数据流（输入）

```
main.c run_desktop()
  hidGetKeyboardStates() → 每个变化的键 → rdp_scancode_from_hid() → rdp_conn_key()
  右摇杆 → rdp_conn_mouse_move()；ZR/ZL → rdp_conn_mouse_button()
rdp_conn_key()/mouse_*() → rdp_input_*() 生成**完整 FastPath PDU** → tr_send()
```

FastPath 输入是**裸 TLS 载荷**，没有 TPKT、没有 MCS 头、没有共享数据头。

## 6. 显示

`main.c`：

1. 连接成功后 `consoleExit()`（**先 `rdp_console_live = 0`**）。
2. `framebufferCreate(&g_fb, nwindowGetDefault(), 1280, 720, RGBA8888, 3)` → 失败退 2 buffer。
3. `framebufferMakeLinear`。
4. 每帧 `fb_blit(surf->pixels, w, h)`：逐行 memcpy 到 `framebufferBegin` 拿到的线性缓冲。

## 7. 内存占用（Switch 侧，实机需留意）

| 项 | 大小 |
|---|---|
| 帧缓冲 surface（1280×720×4） | 3.5 MB |
| framebuffer 3 buffer | 10.5 MB |
| FastPath 分片重组缓冲 | 1 MB |
| Planar tile 临时区（static） | 48 KB |
| `rdp_conn.rx`（PDU 缓冲） | 64 KB |
| 栈 | 默认 |
