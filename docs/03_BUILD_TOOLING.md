# 03 — 编译与工具链

## 0. 目录约定（重要）

本项目在**工作区里**：

```
<repo>\
├─ src\            源码
├─ docs\           文档（本目录）
├─ tools\          验证脚本 + Python 原型
├─ ref\            FreeRDP 源码摘录
├─ third_party\    mbedTLS 源码/构建/安装
├─ build\          ELF / NRO（**注意**：这里还放着用户自己的一个大 .nsz，别整体删）
├─ dist\           交付产物（.nro 快照 + 基准截图）
├─ out\            PPM/PNG 中转
├─ scratch\        一次性实验产物，可随时删
├─ Makefile
└─ README.md
```

### ⚠ 中文路径 + make 的坑（必读）

工作区路径含中文（`<工作区>`）。**make 的 recipe 走 cmd.exe，不认 UTF-8 中文路径**，
所以 Makefile 里任何绝对路径都必须是 **8.3 短路径**（纯 ASCII）：

```
MBEDTLS = third_party/mbedtls-install
```

症状：`fatal error: mbedtls/bignum.h: No such file or directory`，而文件明明存在。

如果目录被移动/重命名，短路径会变，重新取一次：

```powershell
$f = New-Object -ComObject Scripting.FileSystemObject
$f.GetFolder("<repo>").ShortPath
```

（Python 脚本不受影响，Python 3 用宽字符 API；`tools\*.py` 里可以直接写中文路径。）

## 1. 目标构建（Switch NRO）

### 依赖（已装在这台机器上）

| 组件 | 路径 | 备注 |
|---|---|---|
| devkitA64 GCC | `C:\devkitPro\devkitA64\bin\aarch64-none-elf-gcc.exe` | |
| libnx | `C:\devkitPro\libnx` | 链接时用 `libnx/switch_fixed.specs` |
| mbedTLS 2.28.x（交叉编译产物） | `<repo>\third_party\mbedtls-install` | `include/` + `lib/` |
| GNU make | `<workdir>\toolchain\make.exe` | 不在 PATH 里，要用全路径 |
| NRO 打包脚本 | `<workdir>\toolchain\build_nro.ps1` | 由 Makefile 调用 |

### ⚠ 三个"必须"，否则真机崩（2026-10-23 实机排障）

真机报 `2168-0002 (0x4a8)`、PC 落在 `_dynProcessRela`，就是下面这条被破坏了：

| 必须 | 原因 |
|---|---|
| 编译加 **`-fPIE`**（CFLAGS） | 不加 → 带指针的 const 数据被放进 `.rodata` |
| mbedTLS 必须用 **`-fPIC`** 重编（`tools/build_mbedtls.py` 已带） | 不加 → mbedTLS 的 cipher/md/curve 常量表全部在 `.rodata`，一个 ELF 能带 700+ 个只读段重定位 |
| 链接必须用 **libnx 自带的 `nx/switch.specs`**（里面有 `-z text`） | 它会在链接期直接报 `read-only segment has dynamic relocations`，把这个坑挡在编译阶段 |

只读段里有重定位 → 真机把 ro 段映射成只读，`__nx_dynamic()` 写重定位时数据中止；
**Ryujinx 把段全映射成可写，所以模拟器永远看不出来**。

自查命令：

```
python tools/rela_scan.py build/switch_rdp.elf
# 期望输出：relocations into the read-only segment: 0
```

`nx/switch.specs` 里用的是 `%:getenv(DEVKITPRO ...)`，所以 Makefile 里 `export DEVKITPRO`。

### 编译

```powershell
cd <repo>
<workdir>\toolchain\make.exe -j4
# 产物：build/switch_rdp.elf → build/switch_rdp.nro（约 1.12 MB）
```

### 这个 Makefile 的三个坑

1. **Makefile 必须是 UTF-8 无 BOM**。用 `Set-Content -Encoding UTF8`（Windows PowerShell 5.1）
   会写 BOM，make 报 `missing separator`。用
   `[System.IO.File]::WriteAllText($p, $s, (New-Object System.Text.UTF8Encoding($false)))`。
2. **`mkdir` 必须写成 `if not exist build mkdir build`** —— make 的 recipe 走 cmd.exe，
   `mkdir -p` 不存在。
3. 改 `SRCS` 时注意：`rdp_nla.c` / `rdp_proto.c` / `posix_stubs.c` **不在**编译列表里
   （历史遗留，`rdp_credssp.c` 取代了 `rdp_nla.c`）。

### 打包细节（`toolchain/build_nro.py`）

从 ELF 的 program header 重新组 NRO：`NroHeader + .text + .rodata + .data`，
bss 记在 header 里。输出类似：

```
LOAD vaddr=0x0      off=0x10000 filesz=0xB8B10
LOAD vaddr=0xB9000  off=0xC9000 filesz=0x50D38
LOAD vaddr=0x10A000 off=0x11A000 filesz=0x4390
build/switch_rdp.nro -> 1122360 bytes (image 0x112000 + ASET 0x38) bss=0x2C560
```

**NRO 里模块基址 = 0x8500000**（Ryujinx 加载时），崩溃日志里的 `switch_rdp:0x23670`
就是 ELF 的 vaddr `0x23670`，可以直接：

```powershell
& C:\devkitPro\devkitA64\bin\aarch64-none-elf-addr2line.exe -f -C -e build\switch_rdp.elf 0x23670
```

## 2. 部署到 Switch

```
sd:/switch/RDP.nro                        ← 主程序（hbmenu 里显示的名字和图标来自 NACP）
sd:/switch_rdp.cfg                        ← 可选，存在则跳过设置界面自动连接
sd:/switch_rdp_log.txt                    ← 运行日志（轮转，超过 512 KB 换新文件）
```

`RDP.nro` 尾部带 ASET 资源段（icon.jpg + 0x4000 字节 NACP，标题 `RDP`）：
hbmenu 用它显示图标与名称，Ryujinx 没有这一段会直接抛 `NullReferenceException`。
`make hw-bare` 产出不带资源段的裸镜像，只在排查加载器问题时用。

`sd:/switch_rdp.cfg` 是 `key = value` 文本（PC 上编辑，UTF-8）：

```
server = 192.168.1.10
port = 3389
user = user
pass = password
alt_server = 10.0.0.5          # 隐藏配置：秘籍（上上下下左右左右 BA）用它连接
alt_port = 3389
alt_user = administrator
alt_pass = 你的密码
codecs = drop                  # drop / empty / keep，0x001D 能力集的处理方式
```

- 界面上按 `+` 保存上面四项；把隐藏服务器的信息填进界面后**同时按 `+` 和 `-`**，
  当前字段会存进 `alt_*`，界面随即恢复成已保存的可见配置（不会把隐藏信息留在屏幕上）。
- 秘籍连接时用 `alt_*`；`alt_server` 为空则只提示，不会退回去用可见配置——
  秘籍本来就不该拿默认配置去连。
- 老的"四行纯文本"仍然能读（没有 key 名时按 server/port/user/pass 顺序解析）。

## 3. 模拟器（Ryujinx / "Nextendo" 1.10.0）

| 项 | 路径 |
|---|---|
| 可执行 | `<workdir>\emulators\Nextendo\Ryujinx.exe` |
| portable 目录 | 同目录 `portable\` |
| **虚拟 SD 卡** | `portable\sdcard\`（`sdmc:/` 就是这里） |
| 日志 | `portable\Logs\Ryujinx_*.log` |

用法：启动模拟器 → `File → Load Application` → 选 `.nro`。
**这版没有命令行启动参数**（试过把路径当 argv 传，它只开游戏列表），所以只能手动加载。

`sdmc:/switch_rdp.cfg` 已经写在 `portable\sdcard\switch_rdp.cfg`，加载后会自动连。

> 注意：模拟器实例是**多开**的（同一时间可能挂着十几个 Ryujinx 进程）。
> 用 `Get-Process Ryujinx | Where-Object MainWindowTitle` 找到有窗口的那个。
> 不要盲目 `Stop-Process`，会杀掉用户在用的实例。

## 4. PC 验证 harness（强烈建议先在这里验协议）

因为 Switch 上调试困难，协议代码全部可以在 Ubuntu 测试机上用普通 gcc 编译运行，
输入输出换成 PPM 文件。

### 测试机

| 项 | 值 |
|---|---|
| 地址 | `192.168.1.17`，用户 `user`，密码 `password`（SSH 开着） |
| 编译器 | gcc 15 |
| mbedTLS | `libmbedtls-dev` 2.28.8 |
| FreeRDP 3.5.1 | 源码 `/tmp/frd`，ninja 构建目录 `/tmp/frd/build`，xfreerdp 在 `/tmp/frd/build/client/X11/xfreerdp` |
| 图形工具 | `Xvfb` / `xdotool` / `scrot` 都有 |
| 工作目录 | `/tmp/rt`（`/tmp/rt/src` 放源码，`/tmp/rt/host_rdp` 是产物） |

### 上传/编译/运行

```powershell
# 1) 全量重建（会先把 src/*.c 传上去）
python tools\build_host3.py

# 2) 跑 8 秒，输出 PPM
python -c "import sys;sys.path.insert(0,r'<repo>\tools');import rsh;print(rsh.run('cd /tmp/rt && ./host_rdp 192.168.1.10 user password 8 /tmp/rt/out.ppm',90)[1])"

# 3) 取回并转 PNG
python -c "import sys;sys.path.insert(0,r'<repo>\tools');import scp_get;scp_get.get('/tmp/rt/out.ppm',r'<repo>\out\out.ppm')"
python tools\ppm2png3.py <repo>\out\out.ppm <repo>\out\out.png
```

**坑**：`build_host3.py` 只上传 `src/` 里的文件，**不上传 `tools/host_main.c`**。
改了 harness 本身要先手动传：

```powershell
python -c "import sys;sys.path.insert(0,r'<repo>\tools');import scp;scp.put(r'<repo>\tools\host_main.c','/tmp/rt/src/host_main.c')"
```

### harness 的命令行与环境变量

```
./host_rdp <host> <user> <pass> [秒数] [输出.ppm]
```

| 环境变量 | 作用 |
|---|---|
| `RDP_W` / `RDP_H` | 请求的桌面尺寸（默认 1024×768） |
| `RDP_DUMP=1` | 打印每个 PDU 的类型/长度、capset 列表、join 回复全文 |
| `RDP_DUMP2=1` | 更细：TPKT 头、FastPath 头、服务端/客户端数据 PDU 头字段、输入 PDU body |
| `RDP_UTRACE=1` | 每次更新打印行号+时间戳（用来看"刷新矩形有没有被处理"） |
| `RDP_INPUT_TEST=k` | 3 秒后发 Windows 键（开开始菜单） |
| `RDP_INPUT_TEST=m` | 3 秒后点开始按钮 |
| `RDP_INPUT_TEST=e` | 3 秒后 Win+E（开资源管理器）——**最常用的键盘验证** |
| `RDP_INPUT_TEST=b` | 3 秒后双击回收站图标（鼠标验证） |
| `RDP_INPUT_TEST=x` | 3 秒后点窗口关闭按钮 |
| `RDP_INPUT_TEST=t` | 3 秒后 Win+R + 输入 `notepad` + 回车 |
| `RDP_INPUT_TEST=r` | 3 秒后发 Refresh Rect（验证上行数据 PDU 是否被处理） |
| `RDP_ORDERS=1` | 恢复"回显 order 支持"（会花屏，仅用于对照） |
| `RDP_SLOWPATH_INPUT=1` | 用慢速输入 PDU（本服务器忽略，仅用于对照） |

`RDP_INPUT_TEST=e` + 判图：PPM 里出现大量白色（资源管理器窗口）即成功。

## 5. 抓真实客户端字节（FreeRDP）

要确认"标准客户端到底发什么"，用测试机上的 xfreerdp + 无头 X：

```bash
Xvfb :99 -screen 0 1280x800x24 &
export DISPLAY=:99
/tmp/frd/build/client/X11/xfreerdp /v:192.168.1.10 /u:user /p:password \
    /cert:ignore /size:1280x720 2>/tmp/rt/xf.err &
sleep 8
WID=$(xdotool search --onlyvisible --class xfreerdp | head -1)
xdotool key --window $WID super+e
scrot /tmp/rt/xf.png
```

FreeRDP 源码里被打了两个 dump 补丁（**在测试机上，不在本仓库**）：

- `libfreerdp/core/rdp.c` 的 `swrdp_dump()`：每次发 PDU 打印 `SWRDPDUMP send <len> <hex>`
- `libfreerdp/core/fastpath.c` 的 `fastpath_send_multiple_input_pdu()` 里：打印 `SWRDPIN <len> <hex>`

重新打补丁/重编：

```powershell
python tools\patch_frd_fp.py                     # 打 fastpath 输入 dump 补丁
python -c "import sys;sys.path.insert(0,r'<repo>\tools');import rsh;print(rsh.run('cd /tmp/frd/build && ninja xfreerdp',1200)[1])"
```

抓下来之后用现成解析器：

```powershell
python tools\an_xf.py      # 列出所有上行 PDU，找 pduType2 / 通道号
python tools\an_cf.py      # 从抓包里还原 Confirm Active 的 20 个 capset
```

`tools/run_xfreerdp_capture.py` 是上面那一整串的一键版。

## 6. Python 原型（协议参考实现）

`tools/rdp_client.py`（1661 行）是**先用 Python 打通、再移植到 C** 的那份。
它现在还能跑，可以用来对拍任何 C 侧疑难：

```powershell
powershell -File tools\run_win.ps1 -Mode full -Secs 15
# 环境变量默认值都写在这个脚本里（RDP_HOST/PORT/USER/PASS/SEC/CAPS/...）
```

产物：`out_win.png` 之类的 PNG（`Client.save_png()`）。

**注意**：Python 版的 Confirm Active / 输入编码和 C 版**不完全一样**（Python 是早期探索版），
只把它当"协议流程参考"，别把它的 capset 选择当标准答案。

## 7. 本机其它接口

| 脚本 | 用途 |
|---|---|
| `tools/rsh.py` | `rsh.run(cmd, timeout)` 在 192.168.1.17 上执行命令 |
| `tools/scp.py` | `scp.put(local, remote)` 上传 |
| `tools/scp_get.py` | `scp_get.get(remote, local)` 下载 |
| `tools/ppm2png3.py` | PPM→PNG（**带命令行参数**，用这个；`ppm2png.py` 是写死路径的老版本） |

`tools/` 下还有一批 `_*.py`（下划线开头）和一次性脚本（`fp_*.py`、`xf_*.py`、`nsc_*.py` 等），
是开发过程中用来验证具体假设的**临时工具**，不保证还能直接跑；其中 `_fp_patch.py`
被 `patch_frd_fp.py` 引用（给 FreeRDP 打输入 dump 补丁），别删。

## 5. 2026-10-24：devkitPro 被更新后 libnx 消失的修复（务必按此恢复）

现象：`make` 报 `src/main.c:20:10: fatal error: sys/socket.h: No such file or directory`，
`C:\devkitPro\libnx` 整个目录不见了，只剩源码树 `C:\devkitPro\nx`
（同级另一项任务更新 devkitPro 时带走了安装目录）。

恢复步骤（`scratch/rebuild_libnx.sh`，用 msys2 的 bash 跑）：

```bash
export DEVKITPRO=/c/devkitPro
export PATH=$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:/usr/bin:/bin:$PATH
cd /c/devkitPro/nx
make -j8 lib/libnx.a          # 只编 release，debug 版用不上
```

两个坑，都必须先处理，否则编译不过：

1. **`Elf64_Relr` / `DT_RELR` 未定义**（这个 elf.h 是 binutils 2.38 之前的版本，
   但这棵 libnx 源码已经用 RELR 了）。在 `source/runtime/dynamic.c` 顶部加：

   ```c
   #if !defined(DT_RELR)
   #define DT_RELR    36
   #define DT_RELRSZ  35
   #define DT_RELRENT 37
   #define LIBNX_LOCAL_RELR
   #endif
   #ifdef LIBNX_LOCAL_RELR
   typedef Elf64_Xword Elf64_Relr;
   #endif
   ```

2. **`default_font_bin.h: No such file`**：`bin2o` 规则靠 `bin2s` 生成头文件，
   而 `bin2s.exe` 是 msys 程序，PATH 里没有 `/usr/bin` 时它找不到
   `msys-2.0.dll`，静默失败（`as` 收到空输入，只留下一个 656 字节的空 .o）。
   手工补一次即可（`release/` 里）：

   ```bash
   cd /c/devkitPro/nx/release
   bin2s -a 64 -H default_font_bin.h /c/devkitPro/nx/data/default_font.bin \
     | aarch64-none-elf-as -o default_font.bin.o
   ```

安装布局就是标准布局（本工程 Makefile 现在按这个写）：

```bash
mkdir -p /c/devkitPro/libnx/lib
cp -r include                 /c/devkitPro/libnx/
cp -r external/bsd/include/*  /c/devkitPro/libnx/include/   # sys/ netinet/ arpa/ net/
cp switch.specs switch.ld switch_rules default_icon.jpg /c/devkitPro/libnx/
cp lib/libnx.a /c/devkitPro/libnx/lib/
```

于是 Makefile 里只有：

```
LIBNX   = $(DEVKITPRO)/libnx
INCLUDES = -I$(LIBNX)/include -I$(MBEDTLS)/include -Isrc
NORMAL_LIBS = -L$(LIBNX)/lib -L$(MBEDTLS)/lib -lnx -lm ...
LDFLAGS = -specs=$(LIBNX)/switch.specs
```

（旧的 `$(LIBNX)/nx/include` 两级布局已经废弃，别再改回去。）

## 6. NRO 现在带 ASET 资源段（模拟器加载必需）

新版 Ryujinx（1.10.x）的 `ProcessLoader` 会无条件调用
`Nro.OpenNroAssetSection()`，没有资源段的 homebrew NRO 会直接
`NullReferenceException`（日志里 `at LibHac.Tools.Ro.Nro.OpenNroAssetSection`
+ `at Ryujinx.HLE.Loaders.Processes.ProcessLoader.LoadNxo`），表现为"点开就回列表"。

`toolchain/build_nro.py` 现在总是追加：

- `ASET` 头（0x38 字节：magic、version、三段 `u64 offset, u64 size`）；
- JPEG 图标（优先 `APP/switch_rdp/assets/icon.jpg`，否则 devkitPro 的
  `libnx/default_icon.jpg`）；
- 0x4000 字节 NACP（magic `NACP`、16 国语言名 + 发行商）。

**注意偏移基准**：资源段放在镜像（`header.size`）之后，段内偏移相对 ASET 头本身。
`NroAssetSection` 是 **u64** 不是 u32 —— 写错会让 Ryujinx 报
`count ('-520103681') must be a non-negative value`。真机 hbmenu 不要求这个段，
但写了也无害。

## 7. 其它编译期变化

- `-O2` → `-O3`（planar 解码和 blit 是热点）。
- `rdp_update.c` 的 `blit()` 增加"整块都在画面内"的快路径，去掉每像素三次裁剪比较。
- `rdp_tls.c` 的 send 回调错误处理见 06 文档"按一个键就冻住"。

## 8. 两个产物：模拟器版 / 真机版（2026-10-24）

`make` 现在可以出两种 NRO，镜像本体（0x116000 字节）**完全一致**，只差尾部：

| 命令 | 产物 | 用途 |
|---|---|---|
| `make` | `build/switch_rdp.nro` | 带 ASET+NACP+图标，**只给 Ryujinx**（1.10 起没有 ASET 会 NRE） |
| `make hw` | `build/switch_rdp_hw.nro` | 纯镜像，`file size == header.size`，和能启动的 b22 形状一模一样，**给真机 SD 卡** |

`toolchain/build_nro.py <elf> <nro> [--no-assets]` 控制要不要尾部资源段。
真机 hbmenu/Atmosphere 不读 ASET，但既然 b23 在相册里没出现，先用"和 b22 同形状"
的真机版排除变量；真机版校验过镜像字节与模拟器版逐字节相同（sha256 一致）。

发布：`dist/switch_rdp_b23_menu_hw.nro` → 拷成 `SD:/switch/switch_rdp.nro`。

## 9. 装机脚本 `tools/install_to_sd.ps1`

插上 SD（或 Hekate 的 USB SD / MTP 挂成盘符）后：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\install_to_sd.ps1
# 只看不动：打印盘符、hbmenu.nro 是否存在、switch 目录里现有 NRO、要拷的文件大小
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\install_to_sd.ps1 -Apply
# 真的拷：<SD>:\switch\switch_rdp.nro，并在缺省时写一份 switch_rdp.cfg，
# 拷完再读回文件头校验 magic/size/trailing
```

它会拒绝在不认识的卷上写入；配置文件已存在时**不覆盖**。

## 10. 真机"相册里没出现"的排查顺序

1. 文件必须正好在 **`SD:/switch/switch_rdp.nro`**（`switch` 是 SD 根目录下的一级
   文件夹，不能再套子目录；大小 1,138,688 字节，即 `dist/switch_rdp_b23_menu_hw.nro`）。
2. 相册要**按住 R 再启动相册**才会进 hbmenu（Atmosphère 的 loader 接管），
   普通相册什么都不多。
3. **`SD:/hbmenu.nro` 必须存在**，否则按 R 也只是普通相册 —— 这是"重启也没用"
   最常见的根因。
4. 文件传输是否完整（FTP/MTP 中断会留下 0 字节或半截文件；`install_to_sd.ps1`
   会校验大小并读回文件头）。
5. 以上都对仍然不出现，才怀疑 NRO 本身：用 `build_nro.py` 打印的 header 自检
   （magic `NRO0`、brand `HOMEBREW`、三段页对齐、`file size == header.size`）。
