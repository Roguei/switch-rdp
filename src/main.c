/*
 * RDP - remote desktop client for Nintendo Switch (Atmosphere + Hekate)
 *
 * Self-contained: libnx + mbedTLS only, no FreeRDP, no libnx console.
 * The whole UI is drawn by ui_draw.c into one persistent framebuffer, so there
 * is no consoleInit/framebufferClose dance (that crashed on hardware) and
 * Chinese text is possible (libnx's console font is ASCII only).
 *
 * Screens: main (settings) -> connecting -> desktop <-> session menu, plus a
 * settings screen reachable from the menu.  There is deliberately no way to
 * quit the process: exiting crashed on hardware and "disconnect" is enough.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

#include <switch.h>

#include "rdp_session.h"
#include "rdp_input.h"
#include "ui_draw.h"
#include "rdp_update.h"

#define SWRDP_BUILD "b50"

#define SCR_W 1280
#define SCR_H 720

#define COL_BG     0x12, 0x16, 0x20
#define COL_PANEL  0x1C, 0x22, 0x30
#define COL_SEL    0x2C, 0x5C, 0x9C
#define COL_TEXT   0xE8, 0xEC, 0xF4
#define COL_DIM    0x8A, 0x94, 0xA6
#define COL_ACCENT 0x5C, 0xB0, 0xF0
#define COL_OK     0x64, 0xD0, 0x84
#define COL_WARN   0xFF, 0x7A, 0x7A

/* ------------------------------------------------------------------ */
/* configuration                                                      */
/* ------------------------------------------------------------------ */
/* The four fields the settings screen edits (the visible profile). */
static char g_ip[64]   = "";
static char g_port[16] = "3389";
static char g_user[64] = "Administrator";
static char g_pass[64] = "";

/* Second profile: never shown on screen.  The Konami code connects with it and
 * "+" together with "-" copies the current fields into it (see cfg_write). */
static char g_alt_ip[64]   = "";
static char g_alt_port[16] = "3389";
static char g_alt_user[64] = "Administrator";
static char g_alt_pass[64] = "";

static int g_cfg_fields = 0;
static int g_dirty = 0;          /* edited but not written to the SD card */

typedef struct
{
    char ip[64];
    char port[16];
    char user[64];
    char pass[64];
} profile;


static void profile_get(profile* p, int alt)
{
    memcpy(p->ip,   alt ? g_alt_ip   : g_ip,   sizeof(p->ip));
    memcpy(p->port, alt ? g_alt_port : g_port, sizeof(p->port));
    memcpy(p->user, alt ? g_alt_user : g_user, sizeof(p->user));
    memcpy(p->pass, alt ? g_alt_pass : g_pass, sizeof(p->pass));
}

static void profile_set(const profile* p, int alt)
{
    memcpy(alt ? g_alt_ip   : g_ip,   p->ip,   sizeof(p->ip));
    memcpy(alt ? g_alt_port : g_port, p->port, sizeof(p->port));
    memcpy(alt ? g_alt_user : g_user, p->user, sizeof(p->user));
    memcpy(alt ? g_alt_pass : g_pass, p->pass, sizeof(p->pass));
}

#define CFG_PATH "sdmc:/switch_rdp.cfg"
#define LOG_PATH "sdmc:/switch_rdp_log.txt"

/* ------------------------------------------------------------------ */
/* trace file                                                         */
/* ------------------------------------------------------------------ */
static FILE* g_log = NULL;
static int   g_log_ready = 0;

static void tlog_open(void)
{
    long size = 0;
    if (g_log_ready) return;
    g_log_ready = 1;
    g_log = fopen(LOG_PATH, "a+");
    if (!g_log) return;
    fseek(g_log, 0, SEEK_END);
    size = ftell(g_log);
    if (size > 512 * 1024)
    {
        fclose(g_log);
        remove("sdmc:/switch_rdp_log.old.txt");
        rename(LOG_PATH, "sdmc:/switch_rdp_log.old.txt");
        g_log = fopen(LOG_PATH, "w");
        if (!g_log) return;
        size = 0;
    }
    if (size > 0) fputc('\n', g_log);
    fputs("==== boot ====\n", g_log);
    fflush(g_log);
}

static void tlog(const char* fmt, ...)
{
    va_list ap;
    if (!g_log_ready) tlog_open();
    if (!g_log) return;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* rdp_update.c hands its rectangle log to this hook: stderr does not reach the
 * SD trace file, which is why those lines were invisible for a while. */
extern void (*rdp_update_log_hook)(const char* line);
static void update_log(const char* line) { tlog("%s", line); }

/* ------------------------------------------------------------------ */
/* framebuffer                                                        */
/* ------------------------------------------------------------------ */
static Framebuffer g_fb;
static bool        g_fb_init = false;
static PadState    g_pad;

static uint8_t* g_fbp = NULL;
static uint32_t g_fbstride = 0;

static int screen_lock(void)
{
    if (!g_fb_init) return -1;
    g_fbp = (uint8_t*)framebufferBegin(&g_fb, &g_fbstride);
    if (!g_fbp) return -1;
    ui_target(g_fbp, g_fbstride, SCR_W, SCR_H);
    return 0;
}

static void screen_unlock(void) { framebufferEnd(&g_fb); }

static int fb_setup(void)
{
    NWindow* win = nwindowGetDefault();
    /* 3 slots, never 1: with one slot framebufferEnd() writes past the
     * allocation and the loader faults (heap corruption). */
    Result rc = framebufferCreate(&g_fb, win, SCR_W, SCR_H, PIXEL_FORMAT_RGBA_8888, 3);
    if (R_FAILED(rc))
        rc = framebufferCreate(&g_fb, win, SCR_W, SCR_H, PIXEL_FORMAT_RGBA_8888, 2);
    if (R_FAILED(rc)) { tlog("framebufferCreate failed 0x%x", rc); return -1; }
    {
        Result lrc = framebufferMakeLinear(&g_fb);
        tlog("fb create rc=0x%x stride=%u num_fbs=%u linear rc=0x%x",
             rc, g_fb.stride, g_fb.num_fbs, lrc);
    }
    g_fb_init = true;
    return 0;
}

static void fb_solid(uint8_t r, uint8_t g, uint8_t b)
{
    if (screen_lock() != 0) return;
    ui_clear(r, g, b);
    screen_unlock();
}

/* ------------------------------------------------------------------ */
/* settings file                                                      */
/* ------------------------------------------------------------------ */
static char* field_buf(int i)
{
    switch (i) { case 0: return g_ip; case 1: return g_port;
                 case 2: return g_user; default: return g_pass; }
}
static const char* field_label(int i)
{
    switch (i) { case 0: return "服务器"; case 1: return "端口";
                 case 2: return "用户名"; default: return "密码"; }
}
static size_t field_cap(int i)
{
    switch (i) { case 0: return sizeof(g_ip); case 1: return sizeof(g_port);
                 case 2: return sizeof(g_user); default: return sizeof(g_pass); }
}

enum
{
    CFG_SERVER = 0,
    CFG_PORT,
    CFG_USER,
    CFG_PASS,
    CFG_ALT_SERVER,
    CFG_ALT_PORT,
    CFG_ALT_USER,
    CFG_ALT_PASS,
    CFG_CODECS
};

/* Buffers behind the key = value pairs: 0..3 are the visible profile, 4..7 the
 * hidden one, so one parser fills both. */
static char* cfg_field(int idx)
{
    switch (idx)
    {
        case CFG_SERVER:     return g_ip;
        case CFG_PORT:       return g_port;
        case CFG_USER:       return g_user;
        case CFG_PASS:       return g_pass;
        case CFG_ALT_SERVER: return g_alt_ip;
        case CFG_ALT_PORT:   return g_alt_port;
        case CFG_ALT_USER:   return g_alt_user;
        default:             return g_alt_pass;
    }
}

static size_t cfg_cap(int idx)
{
    switch (idx)
    {
        case CFG_SERVER:     return sizeof(g_ip);
        case CFG_PORT:       return sizeof(g_port);
        case CFG_USER:       return sizeof(g_user);
        case CFG_PASS:       return sizeof(g_pass);
        case CFG_ALT_SERVER: return sizeof(g_alt_ip);
        case CFG_ALT_PORT:   return sizeof(g_alt_port);
        case CFG_ALT_USER:   return sizeof(g_alt_user);
        default:             return sizeof(g_alt_pass);
    }
}

static int cfg_key_index(const char* key, size_t klen)
{
    if (klen == 0) return -1;
    if (!strncmp(key, "alt_server", klen) || !strncmp(key, "alt_host", klen) ||
        !strncmp(key, "alt_ip", klen))   return CFG_ALT_SERVER;
    if (!strncmp(key, "alt_port", klen)) return CFG_ALT_PORT;
    if (!strncmp(key, "alt_user", klen)) return CFG_ALT_USER;
    if (!strncmp(key, "alt_pass", klen)) return CFG_ALT_PASS;
    if (!strncmp(key, "server", klen) || !strncmp(key, "host", klen) ||
        !strncmp(key, "ip", klen))     return CFG_SERVER;
    if (!strncmp(key, "port", klen))   return CFG_PORT;
    if (!strncmp(key, "user", klen))   return CFG_USER;
    if (!strncmp(key, "pass", klen))   return CFG_PASS;
    if (!strncmp(key, "codecs", klen)) return CFG_CODECS;
    return -1;
}

static void trim(char* s)
{
    size_t n = strlen(s);
    size_t i = 0;
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) memmove(s, s + i, n - i + 1);
}

static void set_field(int idx, const char* value)
{
    size_t cap = field_cap(idx);
    size_t n = strlen(value);
    if (n >= cap) n = cap - 1;
    memcpy(field_buf(idx), value, n);
    field_buf(idx)[n] = '\0';
}

/* Same for any of the eight config fields (visible and hidden profile). */
static void set_field_cfg(int idx, const char* value)
{
    char* dst = cfg_field(idx);
    size_t cap = cfg_cap(idx);
    size_t n = strlen(value);
    if (n >= cap) n = cap - 1;
    memcpy(dst, value, n);
    dst[n] = '\0';
}

static int cfg_apply_line(char* line, int* pos)
{
    char tmp[192];
    char* eq;
    int idx;

    strncpy(tmp, line, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    trim(tmp);
    if (tmp[0] == '\0' || tmp[0] == '#') return 0;

    eq = strchr(tmp, '=');
    if (eq)
    {
        *eq = '\0';
        trim(tmp);
        trim(eq + 1);
        idx = cfg_key_index(tmp, strlen(tmp));
        if (idx < 0) return 0;
        if (idx == CFG_CODECS)
        {
            if (!strncmp(eq + 1, "empty", 5)) rdp_codec_mode = 1;
            else if (!strncmp(eq + 1, "keep", 4)) rdp_codec_mode = 2;
            else rdp_codec_mode = 0;
            tlog("cfg: codecs mode %d", rdp_codec_mode);
            return 1;
        }
        set_field_cfg(idx, eq + 1);
        return 1;
    }
    if (*pos >= 4) return 0;
    set_field((*pos)++, tmp);
    return 1;
}

static int cfg_load(void)
{
    FILE* f = fopen(CFG_PATH, "r");
    char line[192];
    int pos = 0, applied = 0;

    if (!f) { tlog("cfg: missing"); return -1; }
    while (fgets(line, sizeof(line), f))
        applied += cfg_apply_line(line, &pos);
    fclose(f);
    tlog("cfg loaded %d field(s)", applied);
    return applied;
}

static const char* k_cfg_header =
    "# switch_rdp settings  (SD:/switch_rdp.cfg)\n"
    "# one key = value per line, edit on a PC\n"
    "# alt_* = backup profile: it is never shown on screen, the Konami code\n"
    "#         connects with it, and + together with - stores the fields\n"
    "#         currently on screen into it\n";

static void cfg_write(void)
{
    FILE* f = fopen(CFG_PATH, "w");
    if (!f) { tlog("cfg: cannot write"); return; }
    fputs(k_cfg_header, f);
    fprintf(f, "server = %s\n", g_ip);
    fprintf(f, "port = %s\n", g_port);
    fprintf(f, "user = %s\n", g_user);
    fprintf(f, "pass = %s\n", g_pass);
    fprintf(f, "alt_server = %s\n", g_alt_ip);
    fprintf(f, "alt_port = %s\n", g_alt_port);
    fprintf(f, "alt_user = %s\n", g_alt_user);
    fprintf(f, "alt_pass = %s\n", g_alt_pass);
    fprintf(f, "codecs = %s\n",
            rdp_codec_mode == 1 ? "empty" : (rdp_codec_mode == 2 ? "keep" : "drop"));
    fclose(f);
    tlog("cfg written (main%s)", g_alt_ip[0] ? " + alt" : "");
}

static void cfg_ensure(void)
{
    FILE* f = fopen(CFG_PATH, "r");
    if (f) { fclose(f); return; }
    tlog("cfg: creating template");
    cfg_write();
}

/* ------------------------------------------------------------------ */
/* on-screen keyboard (edits memory only; + saves)                    */
/* ------------------------------------------------------------------ */
static Result edit_field(int idx)
{
    SwkbdConfig c;
    Result rc = swkbdCreate(&c, 0);
    if (R_FAILED(rc)) return rc;

    if (idx == 1) { swkbdConfigMakePresetDefault(&c); swkbdConfigSetType(&c, SwkbdType_NumPad); }
    else if (idx == 3) { swkbdConfigMakePresetPassword(&c); }
    else { swkbdConfigMakePresetDefault(&c); }

    swkbdConfigSetStringLenMax(&c, (u32)(field_cap(idx) - 1));
    swkbdConfigSetHeaderText(&c, field_label(idx));
    swkbdConfigSetGuideText(&c, field_label(idx));
    swkbdConfigSetInitialText(&c, field_buf(idx));

    rc = swkbdShow(&c, field_buf(idx), field_cap(idx));
    swkbdClose(&c);
    if (R_SUCCEEDED(rc)) { g_dirty = 1; tlog("field %d edited (not saved yet)", idx); }
    return rc;
}

static const char* error_text(int err);
extern int rdp_update_other_codes[16];
static int  session_flow(const char* host, const char* port,
                         const char* user, const char* pass);
static int  run_desktop(rdp_conn* conn);
static int  type_text_dialog(rdp_conn* conn, char* status, size_t cap);
static void settings_screen(const char* status_in);

/* ------------------------------------------------------------------ */
/* screens                                                            */
/* ------------------------------------------------------------------ */
static void row(int i, int sel, int y)
{
    char value[80];
    if (i == 3)
    {
        size_t n = strlen(g_pass), k = 0;
        if (n > sizeof(value) - 1) n = sizeof(value) - 1;
        for (; k < n; k++) value[k] = '*';
        value[k] = '\0';
    }
    else
    {
        snprintf(value, sizeof(value), "%s", field_buf(i));
    }
    if (i == sel)
    {
        ui_rect(56, y - 12, SCR_W - 112, 60, COL_SEL, 255);
        ui_text(80, y, 2, 0xE8, 0xEC, 0xF4, field_label(i));
    }
    else
    {
        ui_text(80, y, 2, 0x8A, 0x94, 0xA6, field_label(i));
    }
    ui_text(360, y, 2, COL_TEXT, value);
}

static void draw_main(int sel, const char* status)
{
    if (screen_lock() != 0) return;
    ui_clear(COL_BG);
    ui_text(60, 40, 3, COL_ACCENT, "RDP 远程桌面");
    ui_text(SCR_W - 210, 56, 1, COL_DIM, SWRDP_BUILD);
    ui_rect(56, 130, SCR_W - 112, 2, COL_PANEL, 255);
    for (int i = 0; i < 4; i++) row(i, sel, 180 + i * 72);
    ui_rect(56, 470, SCR_W - 112, 2, COL_PANEL, 255);
    ui_text(60, 500, 2, COL_TEXT, "上下选择   A 编辑   Y 连接");
    ui_text(60, 545, 2, COL_DIM, "配置文件 SD 卡 switch_rdp.cfg");
    if (g_dirty) ui_text(60, 585, 2, COL_WARN, "未保存 按 + 保存到 SD 卡");
    if (status && status[0]) ui_text(60, 630, 2, COL_OK, status);
    screen_unlock();
}

static void draw_settings(int sel, const char* status)
{
    if (screen_lock() != 0) return;
    ui_clear(COL_BG);
    ui_text(60, 40, 3, COL_ACCENT, "设置 服务器 账号");
    ui_rect(56, 130, SCR_W - 112, 2, COL_PANEL, 255);
    for (int i = 0; i < 4; i++) row(i, sel, 180 + i * 72);
    ui_rect(56, 470, SCR_W - 112, 2, COL_PANEL, 255);
    ui_text(60, 500, 2, COL_TEXT, "上下选择  A 编辑  + 保存  B 返回");
    if (status && status[0]) ui_text(60, 545, 2, COL_OK, status);
    screen_unlock();
}

static void draw_connect(const char* stage, int tick)
{
    static const char* dots[4] = { ".", "..", "...", "...." };
    char line[128];
    if (screen_lock() != 0) return;
    ui_clear(COL_BG);
    ui_text(60, 40, 3, COL_ACCENT, "RDP 远程桌面");
    snprintf(line, sizeof(line), "%s:%s", g_ip[0] ? g_ip : "?", g_port);
    ui_text(60, 200, 2, COL_TEXT, "正在连接");
    ui_text(320, 200, 2, COL_ACCENT, line);
    ui_text(60, 260, 2, COL_DIM, dots[tick & 3]);
    ui_text(60, 340, 2, COL_WARN, "按 B 取消");
    if (stage && stage[0]) ui_text(60, 440, 2, COL_DIM, stage);
    screen_unlock();
}

static void draw_menu(int sel, const char* status)
{
    static const char* items[5] = {
        "继续远程桌面", "输入文字 系统键盘", "设置 服务器 账号", "发送按键 屏幕键盘",
        "断开连接"
    };
    if (screen_lock() != 0) return;
    ui_clear(COL_BG);
    ui_rect(40, 40, SCR_W - 80, SCR_H - 80, COL_PANEL, 255);
    ui_frame(40, 40, SCR_W - 80, SCR_H - 80, COL_ACCENT, 255);
    ui_text(80, 80, 3, COL_ACCENT, "会话菜单");
    ui_rect(80, 150, SCR_W - 160, 2, COL_PANEL, 255);
    for (int i = 0; i < 5; i++)
    {
        int y = 196 + i * 64;
        if (i == sel)
        {
            ui_rect(70, y - 14, SCR_W - 140, 60, COL_SEL, 255);
            ui_text(100, y, 2, 0xE8, 0xEC, 0xF4, items[i]);
        }
        else ui_text(100, y, 2, 0x8A, 0x94, 0xA6, items[i]);
    }
    ui_rect(80, 540, SCR_W - 160, 2, COL_PANEL, 255);
    ui_text(100, 570, 2, COL_TEXT, "上下选择  A 确认  B 返回");
    if (status && status[0]) ui_text(100, 630, 2, COL_OK, status);
    screen_unlock();
}

static void draw_typing_hint(void)
{
    if (screen_lock() != 0) return;
    ui_clear(COL_BG);
    ui_text(60, 80, 3, COL_ACCENT, "系统键盘已打开");
    ui_text(60, 240, 2, COL_TEXT, "按 + 确认后发送 按 B 取消 不会发送");
    screen_unlock();
}

static void draw_error(int err)
{
    char line[192];
    if (screen_lock() != 0) return;
    ui_clear(0x30, 0x0C, 0x10);
    snprintf(line, sizeof(line), "%s:%s", g_ip, g_port);
    ui_text(60, 120, 2, COL_DIM, line);
    ui_text(60, 240, 3, COL_WARN, error_text(err));
    snprintf(line, sizeof(line), "错误代码 %d", err);
    ui_text(60, 330, 2, COL_DIM, line);
    ui_text(60, 500, 2, COL_TEXT, "按 B 返回");
    screen_unlock();
}

/* ------------------------------------------------------------------ */
/* input helpers                                                      */
/* ------------------------------------------------------------------ */
#define K_A     0x04
#define K_B     0x05
#define K_UP    0x52
#define K_DOWN  0x51
#define K_LEFT  0x50
#define K_RIGHT 0x4F
#define K_ENTER 0x28
#define K_Y     0x1C

static int kbd_held(const u64 k[4], int usage)
{
    return (int)((k[usage / 64] >> (usage % 64)) & 1ULL);
}

static void read_kbd(u64 down[4], u64 prev[4])
{
    HidKeyboardState ks;
    u64 kb[4] = {0,0,0,0};
    if (hidGetKeyboardStates(&ks, 1) == 1)
    {
        kb[0] = ks.keys[0]; kb[1] = ks.keys[1];
        kb[2] = ks.keys[2]; kb[3] = ks.keys[3];
        /* Shift/Ctrl/Alt/... are HID usages 0xE0..0xE7, i.e. word 3 bits
         * 32..39.  Some firmware only fills the modifiers field, so merge it. */
        kb[3] |= (ks.modifiers & 0xFFu) << 32;
    }
    for (int w = 0; w < 4; w++) { down[w] = kb[w] & ~prev[w]; prev[w] = kb[w]; }
}

/* Konami: UP UP DOWN DOWN LEFT RIGHT LEFT RIGHT B A */
#define KONAMI_LEN 10
static const uint8_t k_konami[KONAMI_LEN] = { 0, 0, 1, 1, 2, 3, 2, 3, 4, 5 };
static uint8_t g_hist[KONAMI_LEN];
static int     g_hist_n = 0;

static int konami_push(int code)
{
    int i;
    for (i = 1; i < KONAMI_LEN; i++) g_hist[i - 1] = g_hist[i];
    g_hist[KONAMI_LEN - 1] = (uint8_t)code;
    if (g_hist_n < KONAMI_LEN) g_hist_n++;
    if (g_hist_n < KONAMI_LEN) return 0;
    for (i = 0; i < KONAMI_LEN; i++)
        if (g_hist[i] != k_konami[i]) return 0;
    g_hist_n = 0;
    return 1;
}

int ui_poll_cancel(void)
{
    u64 cur;
    padUpdate(&g_pad);
    cur = padGetButtons(&g_pad);
    return (cur & (HidNpadButton_B | HidNpadButton_Minus)) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* forward declarations                                               */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* session                                                            */
/* ------------------------------------------------------------------ */
static int g_connecting = 0;
static void stage_hook(const char* text)
{
    static int tick = 0;
    tlog("stage: %s", text);
    if (!g_connecting) return;      /* never repaint over a live desktop */
    draw_connect(text, tick++);
}

static const char* error_text(int err)
{
    switch (err)
    {
        case RDP_ERR_ADDRESS:   return "服务器地址无效 请检查";
        case RDP_ERR_NETWORK:   return "连接超时 服务器没有响应";
        case RDP_ERR_REFUSED:   return "连接被拒绝 端口没有监听";
        case RDP_ERR_UNREACH:   return "网络不可达 请检查网络";
        case RDP_ERR_X224:      return "服务器回应异常 端口不对";
        case RDP_ERR_TLS:       return "TLS 握手失败";
        case RDP_ERR_AUTH:      return "账号或密码错误";
        case RDP_ERR_ACL:       return "账号没有远程桌面权限";
        case RDP_ERR_MCS:       return "会话建立失败";
        case RDP_ERR_CANCELLED: return "已取消";
        default:                return "未知错误";
    }
}

/* connect + desktop; always returns 1 (back to the main screen) */
static int session_flow(const char* host, const char* port,
                        const char* user, const char* pass)
{
    rdp_cfg cfg;
    rdp_conn* conn;
    int err;

    memset(&cfg, 0, sizeof(cfg));
    cfg.host   = host;
    cfg.port   = atoi(port);
    cfg.user   = user;
    cfg.pass   = pass;
    cfg.domain = "";
    cfg.width  = SCR_W;
    cfg.height = SCR_H;

    g_connecting = 1;
    draw_connect(NULL, 0);
    tlog("connecting");

    conn = rdp_conn_new(&cfg, NULL, NULL);
    if (!conn) { draw_error(RDP_ERR_OTHER); return 1; }

    err = rdp_conn_connect(conn);
    g_connecting = 0;
    if (err != 0)
    {
        tlog("connect failed: %d", err);
        draw_error(err);
        rdp_conn_free(conn);
        for (;;)
        {
            u64 cur;
            padUpdate(&g_pad);
            cur = padGetButtons(&g_pad);
            if (cur & (HidNpadButton_B | HidNpadButton_A | HidNpadButton_Plus)) break;
            svcSleepThread(50000000ULL);
        }
        return 1;
    }

    /* No Refresh Rect here: sending an uplink PDU before the server finished
     * the activation handshake makes it drop the connection (observed as an
     * immediate RST after "desktop loop start").  The desktop loop nudges with
     * a refresh only after data has actually been flowing. */

    fb_solid(0x00, 0x00, 0x40);
    run_desktop(conn);
    tlog("disconnect: desktop returned");
    rdp_conn_free(conn);
    tlog("disconnect: connection freed");
    return 1;
}

/* ------------------------------------------------------------------ */
/* main screen                                                        */
/* ------------------------------------------------------------------ */
static void main_screen_loop(void)
{
    char status[160];
    int sel = 0, redraw = 1;
    u64 prev_kbd[4] = {0,0,0,0};
    u64 prev_pad = 0;

    if (g_cfg_fields > 0)
        snprintf(status, sizeof(status), "已从配置文件读取 %d 项", g_cfg_fields);
    else
        snprintf(status, sizeof(status), "配置文件已生成 请填写服务器");

    for (;;)
    {
        u64 kdn[4], pad_held, pad_down;
        int quick = 0;

        padUpdate(&g_pad);
        pad_held = padGetButtons(&g_pad);
        pad_down = pad_held & ~prev_pad;
        prev_pad = pad_held;
        read_kbd(kdn, prev_kbd);

        {
            int order[12], n = 0, i;
            if (pad_down & HidNpadButton_Up)    order[n++] = 0;
            if (pad_down & HidNpadButton_Down)  order[n++] = 1;
            if (pad_down & HidNpadButton_Left)  order[n++] = 2;
            if (pad_down & HidNpadButton_Right) order[n++] = 3;
            if (pad_down & HidNpadButton_B)     order[n++] = 4;
            if (pad_down & HidNpadButton_A)     order[n++] = 5;
            if (kbd_held(kdn, K_UP))            order[n++] = 0;
            if (kbd_held(kdn, K_DOWN))          order[n++] = 1;
            if (kbd_held(kdn, K_LEFT))          order[n++] = 2;
            if (kbd_held(kdn, K_RIGHT))         order[n++] = 3;
            if (kbd_held(kdn, K_B))             order[n++] = 4;
            if (kbd_held(kdn, K_A))             order[n++] = 5;
            for (i = 0; i < n; i++) if (konami_push(order[i])) quick = 1;
        }

        if (pad_down || kdn[0] || kdn[1])
            tlog("ui pad=0x%016llX paddown=0x%016llX",
                 (unsigned long long)pad_held, (unsigned long long)pad_down);

        if ((pad_down & HidNpadButton_Down) || kbd_held(kdn, K_DOWN))
        { sel = (sel + 1) % 4; redraw = 1; }
        if ((pad_down & HidNpadButton_Up) || kbd_held(kdn, K_UP))
        { sel = (sel + 3) % 4; redraw = 1; }
        {
            /* "+" alone saves the visible profile, "+" and "-" together store
             * the same fields into the hidden profile instead. */
            int save_alt = 0;
            if ((pad_down & HidNpadButton_Plus) && (pad_held & HidNpadButton_Minus)) save_alt = 1;
            if ((pad_down & HidNpadButton_Minus) && (pad_held & HidNpadButton_Plus)) save_alt = 1;

            if (save_alt)
            {
                profile p;
                profile_get(&p, 0);
                profile_set(&p, 1);
                cfg_write();
                /* While the hidden server was being typed it was sitting in
                 * the visible fields; pull the saved visible profile back so
                 * the screen does not keep showing the secret. */
                cfg_load();
                g_dirty = 0;
                snprintf(status, sizeof(status), "备用配置已保存 已恢复界面配置");
                redraw = 1;
            }
            else if (pad_down & HidNpadButton_Plus)
            {
                cfg_write();
                g_dirty = 0;
                snprintf(status, sizeof(status), "已保存到 SD 卡");
                redraw = 1;
            }
        }

        if (quick)
        {
            quick = 0;
            if (!g_alt_ip[0])
            {
                snprintf(status, sizeof(status), "备用配置为空 请教好服务器后再同时按 + 和 -");
                redraw = 1;
            }
            else
            {
                tlog("konami -> quick connect (hidden profile)");
                session_flow(g_alt_ip, g_alt_port, g_alt_user, g_alt_pass);
                padUpdate(&g_pad);
                prev_pad = padGetButtons(&g_pad);
                for (int w = 0; w < 4; w++) prev_kbd[w] = 0;
                snprintf(status, sizeof(status), "已断开");
                redraw = 1;
            }
        }
        else if ((pad_down & HidNpadButton_A) || kbd_held(kdn, K_ENTER))
        {
            Result rc = edit_field(sel);
            snprintf(status, sizeof(status), R_SUCCEEDED(rc) ? "已修改 按 + 保存"
                                                            : "初始化失败");
            redraw = 1;
        }
        else if ((pad_down & HidNpadButton_Y) || kbd_held(kdn, K_Y))
        {
            if (!g_ip[0])
            {
                snprintf(status, sizeof(status), "没有配置服务器 请先填写");
                redraw = 1;
            }
            else
            {
                session_flow(g_ip, g_port, g_user, g_pass);
                padUpdate(&g_pad);
                prev_pad = padGetButtons(&g_pad);
                for (int w = 0; w < 4; w++) prev_kbd[w] = 0;
                snprintf(status, sizeof(status), "已断开");
                redraw = 1;
            }
        }

        if (redraw) { draw_main(sel, status); redraw = 0; }
        svcSleepThread(50000000ULL);
    }
}

/* ------------------------------------------------------------------ */
/* typing into the remote                                             */
/* ------------------------------------------------------------------ */
static int send_text_to_remote(rdp_conn* conn, const char* s)
{
    const unsigned char* p = (const unsigned char*)s;
    int units = 0;

    while (*p)
    {
        uint32_t cp;
        int extra;

        if (*p < 0x80)                { cp = *p++;        extra = 0; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p++ & 0x1F; extra = 1; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p++ & 0x0F; extra = 2; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p++ & 0x07; extra = 3; }
        else                          { p++;              continue; }

        while (extra-- > 0 && (*p & 0xC0) == 0x80)
            cp = (cp << 6) | (uint32_t)(*p++ & 0x3F);
        if (cp == '\n') cp = '\r';

        if (cp < 0x10000)
        {
            rdp_conn_unicode(conn, (uint16_t)cp, 1);
            rdp_conn_unicode(conn, (uint16_t)cp, 0);
            units++;
        }
        else
        {
            uint32_t v = cp - 0x10000;
            uint16_t hi = (uint16_t)(0xD800 + (v >> 10));
            uint16_t lo = (uint16_t)(0xDC00 + (v & 0x3FF));
            rdp_conn_unicode(conn, hi, 1); rdp_conn_unicode(conn, hi, 0);
            rdp_conn_unicode(conn, lo, 1); rdp_conn_unicode(conn, lo, 0);
            units += 2;
        }
    }
    return units;
}

static int type_text_dialog(rdp_conn* conn, char* status, size_t cap)
{
    SwkbdConfig c;
    char buf[512];
    Result rc = swkbdCreate(&c, 0);
    int n;

    if (R_FAILED(rc))
    {
        if (status) snprintf(status, cap, "初始化失败");
        tlog("swkbdCreate failed 0x%x", rc);
        return -1;
    }
    swkbdConfigMakePresetDefault(&c);
    /* The default preset pins the applet to SwkbdType_QWERTY, which is why the
     * keyboard only ever offered the Latin layout.  SwkbdType_All brings the
     * CJK keyboards back (the globe key switches to 简体中文 pinyin), and the
     * text is then delivered as UTF-16 through SC_UNICODE. */
    swkbdConfigSetType(&c, SwkbdType_All);
    swkbdConfigSetStringLenMax(&c, (u32)(sizeof(buf) - 1));
    swkbdConfigSetHeaderText(&c, "输入要发送到远端的文字");
    swkbdConfigSetInitialText(&c, "");
    buf[0] = '\0';
    rc = swkbdShow(&c, buf, sizeof(buf));
    swkbdClose(&c);
    if (R_FAILED(rc))
    {
        if (status) snprintf(status, cap, "已取消");
        tlog("swkbd cancelled 0x%x", rc);
        return -1;
    }
    n = send_text_to_remote(conn, buf);
    tlog("typed %d unit(s)", n);
    if (status) snprintf(status, cap, "已发送字符到远端 %d", n);
    return n;
}

/* ------------------------------------------------------------------ */
/* settings screen (from the menu)                                    */
/* ------------------------------------------------------------------ */
static void settings_screen(const char* status_in)
{
    char status[160];
    int sel = 0, redraw = 1;
    u64 prev_pad = 0;
    u64 prev_kbd[4] = {0,0,0,0};

    snprintf(status, sizeof(status), "%s", status_in ? status_in : "");
    padUpdate(&g_pad);
    prev_pad = padGetButtons(&g_pad);

    for (;;)
    {
        u64 kdn[4], cur, down;
        padUpdate(&g_pad);
        cur = padGetButtons(&g_pad);
        down = cur & ~prev_pad;
        prev_pad = cur;
        read_kbd(kdn, prev_kbd);

        if ((down & HidNpadButton_Down) || kbd_held(kdn, K_DOWN))
        { sel = (sel + 1) % 4; redraw = 1; }
        if ((down & HidNpadButton_Up) || kbd_held(kdn, K_UP))
        { sel = (sel + 3) % 4; redraw = 1; }
        if (down & HidNpadButton_Plus)
        {
            int save_alt = (cur & HidNpadButton_Minus) ? 1 : 0;
            if (save_alt)
            {
                profile p;
                profile_get(&p, 0);
                profile_set(&p, 1);
                cfg_write();
                cfg_load();          /* put the visible profile back on screen */
                g_dirty = 0;
                snprintf(status, sizeof(status), "备用配置已保存 已恢复界面配置");
            }
            else
            {
                cfg_write();
                g_dirty = 0;
                snprintf(status, sizeof(status), "已保存到 SD 卡");
            }
            redraw = 1;
        }
        if (down & HidNpadButton_A)
        {
            Result rc = edit_field(sel);
            snprintf(status, sizeof(status), R_SUCCEEDED(rc) ? "已修改 按 + 保存"
                                                            : "初始化失败");
            redraw = 1;
        }
        if (down & HidNpadButton_B) break;

        if (redraw) { draw_settings(sel, status); redraw = 0; }
        svcSleepThread(20000000ULL);
    }
}


/* ------------------------------------------------------------------ */
/* on-screen keys (no hardware keyboard needed)                       */
/* ------------------------------------------------------------------ */
#define KEY_PANEL_N 10

typedef struct
{
    const char* label;
    uint16_t mod;       /* first modifier scancode, 0 = none */
    uint16_t mod2;      /* second modifier scancode, 0 = none */
    uint16_t key;       /* main key scancode */
    int ext;            /* main key is an extended scancode */
} key_def;

static const key_def k_keys[KEY_PANEL_N] = {
    { "Ctrl+C", 0x1D, 0,    0x2E, 0 },
    { "Ctrl+V", 0x1D, 0,    0x2F, 0 },
    { "Ctrl+X", 0x1D, 0,    0x2D, 0 },
    { "Ctrl+Z", 0x1D, 0,    0x2C, 0 },
    { "Ctrl+A", 0x1D, 0,    0x1E, 0 },
    { "Enter",  0,    0,    0x1C, 0 },
    { "Backspace", 0, 0,    0x0E, 0 },
    { "Delete", 0,    0,    0x53, 1 },
    { "Esc",    0,    0,    0x01, 0 },
    { "Ctrl+Alt+Del", 0x1D, 0x38, 0x53, 1 },
};

#define KEY_PANEL_COLS 5

static void draw_key_panel(int sel)
{
    if (screen_lock() != 0) return;
    ui_clear(COL_BG);
    ui_rect(40, 40, SCR_W - 80, SCR_H - 80, COL_PANEL, 255);
    ui_frame(40, 40, SCR_W - 80, SCR_H - 80, COL_ACCENT, 255);
    ui_text(80, 90, 3, COL_ACCENT, "发送按键");
    ui_text(80, 150, 2, COL_DIM, "没有硬件键盘也能发这些键");
    for (int i = 0; i < KEY_PANEL_N; i++)
    {
        int cx = 110 + (i % KEY_PANEL_COLS) * 228;
        int cy = 300 + (i / KEY_PANEL_COLS) * 140;
        if (i == sel)
        {
            ui_rect(cx - 26, cy - 18, 210, 66, COL_SEL, 255);
            ui_text(cx, cy, 2, 0xF0, 0xF4, 0xFA, k_keys[i].label);
        }
        else ui_text(cx, cy, 2, 0x8A, 0x94, 0xA6, k_keys[i].label);
    }
    ui_text(100, 640, 2, COL_TEXT, "方向选择   A 发送   B 返回");
    screen_unlock();
}

static void key_send(rdp_conn* conn, const key_def* k)
{
    if (k->mod)  rdp_conn_key(conn, k->mod, 1, 0);
    if (k->mod2) rdp_conn_key(conn, k->mod2, 1, 0);
    rdp_conn_key(conn, k->key, 1, k->ext);
    rdp_conn_key(conn, k->key, 0, k->ext);
    if (k->mod2) rdp_conn_key(conn, k->mod2, 0, 0);
    if (k->mod)  rdp_conn_key(conn, k->mod, 0, 0);
}

static void key_panel(rdp_conn* conn)
{
    int sel = 0, redraw = 1;
    u64 prev = 0;

    padUpdate(&g_pad);
    prev = padGetButtons(&g_pad);
    for (;;)
    {
        u64 cur, down;
        padUpdate(&g_pad);
        cur = padGetButtons(&g_pad);
        down = cur & ~prev;
        prev = cur;

        if (down & HidNpadButton_Right) { sel = (sel + 1) % KEY_PANEL_N; redraw = 1; }
        if (down & HidNpadButton_Left)  { sel = (sel + KEY_PANEL_N - 1) % KEY_PANEL_N; redraw = 1; }
        if (down & HidNpadButton_Down)  { sel = (sel + KEY_PANEL_COLS) % KEY_PANEL_N; redraw = 1; }
        if (down & HidNpadButton_Up)    { sel = (sel + KEY_PANEL_N - KEY_PANEL_COLS) % KEY_PANEL_N; redraw = 1; }
        if (down & HidNpadButton_B) return;
        if (down & HidNpadButton_A)
        {
            key_send(conn, &k_keys[sel]);
            tlog("key panel: sent %s", k_keys[sel].label);
            redraw = 1;
        }
        if (redraw) { draw_key_panel(sel); redraw = 0; }
        for (int i = 0; i < 16; i++) if (rdp_conn_pump(conn, 0) <= 0) break;
        svcSleepThread(20000000ULL);
    }
}

/* ------------------------------------------------------------------ */
/* session menu                                                       */
/* ------------------------------------------------------------------ */
static int session_menu(rdp_conn* conn)
{
    char status[160] = "";
    int sel = 0, action = 0, redraw = 1;
    u64 prev_pad = 0;
    u64 prev_kbd[4] = {0,0,0,0};

    padUpdate(&g_pad);
    prev_pad = padGetButtons(&g_pad);
    tlog("menu opened");

    for (;;)
    {
        u64 kdn[4], cur, down;
        padUpdate(&g_pad);
        cur = padGetButtons(&g_pad);
        down = cur & ~prev_pad;
        prev_pad = cur;
        read_kbd(kdn, prev_kbd);

        if ((down & HidNpadButton_Down) || kbd_held(kdn, K_DOWN))
        { sel = (sel + 1) % 5; redraw = 1; }
        if ((down & HidNpadButton_Up) || kbd_held(kdn, K_UP))
        { sel = (sel + 4) % 5; redraw = 1; }
        if (down & (HidNpadButton_B | HidNpadButton_Plus))
        { tlog("menu: back"); action = 0; break; }

        if (down & HidNpadButton_A)
        {
            if (sel == 0) { tlog("menu: resume"); action = 0; break; }
            if (sel == 1)
            {
                for (int k = 0; k < 16; k++) if (rdp_conn_pump(conn, 0) <= 0) break;
                draw_typing_hint();
                type_text_dialog(conn, status, sizeof(status));
                redraw = 1;
            }
            if (sel == 2) { settings_screen(status); redraw = 1; }
            if (sel == 3) { key_panel(conn); redraw = 1; }
            if (sel == 4) { tlog("menu: disconnect"); action = 1; break; }
        }

        if (redraw) { draw_menu(sel, status); redraw = 0; }
        for (int k = 0; k < 16; k++) if (rdp_conn_pump(conn, 0) <= 0) break;
        svcSleepThread(20000000ULL);
    }
    return action;
}

/* ------------------------------------------------------------------ */
/* desktop                                                            */
/* ------------------------------------------------------------------ */
static const uint8_t k_arrow[19][12] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},{1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},{1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},{1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},{1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},{1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,2,2,2,2,1,0},{1,2,2,2,2,2,2,2,2,2,2,1},
    {1,2,2,2,2,1,1,1,1,1,1,1},{1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,1,2,2,1,0,0,0,0,0,0},{1,1,0,1,2,2,1,0,0,0,0,0},
    {1,0,0,0,1,2,2,1,0,0,0,0},{0,0,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,0,1,2,2,1,0,0,0},
};
#define CURSOR_W 12
#define CURSOR_H 19

static void draw_cursor(uint8_t* dst, uint32_t stride, int x, int y, int w, int h)
{
    for (int r = 0; r < CURSOR_H; r++)
        for (int c = 0; c < CURSOR_W; c++)
        {
            uint8_t v = k_arrow[r][c];
            int dx = x + c, dy = y + r;
            uint8_t* p;
            if (!v || dx < 0 || dy < 0 || dx >= w || dy >= h) continue;
            p = dst + (size_t)dy * stride + (size_t)dx * 4;
            if (v == 1) { p[0] = 0; p[1] = 0; p[2] = 0; }
            else        { p[0] = 255; p[1] = 255; p[2] = 255; }
            p[3] = 0xFF;
        }
}

static void fb_present(rdp_surface* s, int x0, int y0, int x1, int y1,
                       int cx, int cy)
{
    uint32_t stride;
    void* dst;
    size_t row_bytes;

    if (!g_fb_init || !s || !s->pixels) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->width) x1 = s->width;
    if (y1 > s->height) y1 = s->height;
    if (x1 <= x0 || y1 <= y0) return;

    dst = framebufferBegin(&g_fb, &stride);
    if (!dst) return;
    if (stride < (uint32_t)s->width * 4u) { framebufferEnd(&g_fb); return; }
    row_bytes = (size_t)(x1 - x0) * 4;
    for (int y = y0; y < y1; y++)
        memcpy((uint8_t*)dst + (size_t)y * stride + (size_t)x0 * 4,
               s->pixels + ((size_t)y * (size_t)s->width + (size_t)x0) * 4,
               row_bytes);
    draw_cursor((uint8_t*)dst, stride, cx, cy, s->width, s->height);
    framebufferEnd(&g_fb);
}

static int run_desktop(rdp_conn* conn)
{
    rdp_surface* surf = rdp_conn_surface(conn);
    u64 prev_keys[4] = {0,0,0,0};
    u64 prev_buttons = 0;
    int mx = surf->width / 2, my = surf->height / 2;
    int cx = mx, cy = my;
    int last_mx = -1, last_my = -1;
    u64 last_move_ms = 0, lr_since = 0, t_report = svcGetSystemTick();
    u64 t_nudge = svcGetSystemTick();
    u64 last_wheel_ms = 0;
    float accx = 0.0f, accy = 0.0f;      /* sub-pixel stick residue */
    int touch_down = 0, full = 1, lr_latch = 0;
    u64 frames = 0, blits = 0, pdus = 0, blit_ms = 0, pump_ms = 0;
    u64 kbd_events = 0;
    u32 prev_mouse = 0;
    int kbd_states = 0;                  /* hidGetKeyboardStates() return value */
    u64 kbd_mods = 0, kbd_keys = 0;      /* last raw keyboard state, for the log */

    rdp_conn_mouse_move(conn, mx, my);
    /* Activate the keyboard again now that the applet is up and focused: the
     * boot-time call happens before the first padUpdate, and an applet that is
     * not yet running has been seen to swallow it. */
    hidInitializeKeyboard();
    tlog("desktop loop start");

    for (;;)
    {
        u64 now_ms = svcGetSystemTick() / 19200ULL;
        int ax0 = 0, ay0 = 0, ax1 = 0, ay1 = 0, changed = 0, err = 0;

        {
            u64 t0 = svcGetSystemTick();
            for (int d = 0; d < 48; d++)
            {
                int r = rdp_conn_pump(conn, d == 0 ? 2 : 0);
                if (r < 0) { err = 1; break; }
                if (r == 0) break;
                pdus++;
                /* Any traffic resets the idle timer, so the full-screen
                 * refresh below only fires when the server has gone quiet -
                 * a forced 1280x720 repaint every 2 s was pure jank. */
                t_nudge = svcGetSystemTick();
                changed = 1;
                if (surf->dirty_x1 > surf->dirty_x0 && surf->dirty_y1 > surf->dirty_y0)
                {
                    if (ax1 <= ax0) { ax0 = surf->dirty_x0; ay0 = surf->dirty_y0;
                                      ax1 = surf->dirty_x1; ay1 = surf->dirty_y1; }
                    else
                    {
                        if (surf->dirty_x0 < ax0) ax0 = surf->dirty_x0;
                        if (surf->dirty_y0 < ay0) ay0 = surf->dirty_y0;
                        if (surf->dirty_x1 > ax1) ax1 = surf->dirty_x1;
                        if (surf->dirty_y1 > ay1) ay1 = surf->dirty_y1;
                    }
                }
            }
            pump_ms += (svcGetSystemTick() - t0) / 19200ULL;
        }

        if (err) { tlog("pump error, session ended"); fb_solid(0x40, 0, 0); return 1; }
        frames++;

        {
            /* USB mouse, if the firmware routes it: relative deltas move the
             * pointer, the three buttons map to left/right/middle and the
             * wheel goes through the same nudge as the right stick. */
            HidMouseState mst;
            if (hidGetMouseStates(&mst, 1) == 1)
            {
                if (mst.delta_x || mst.delta_y)
                {
                    mx += mst.delta_x;
                    my += mst.delta_y;
                    if (mx < 0) mx = 0;
                    if (my < 0) my = 0;
                    if (mx >= surf->width) mx = surf->width - 1;
                    if (my >= surf->height) my = surf->height - 1;
                    changed = 1;
                }
                if (mst.buttons != prev_mouse)
                {
                    u32 diff = mst.buttons ^ prev_mouse, b;
                    prev_mouse = mst.buttons;
                    while (diff)
                    {
                        int bit = __builtin_ctz(diff);
                        diff &= diff - 1;
                        b = 1u << bit;
                        if (b == HidMouseButton_Left)   rdp_conn_mouse_button(conn, 1, (int)((mst.buttons & b) != 0));
                        if (b == HidMouseButton_Right)  rdp_conn_mouse_button(conn, 2, (int)((mst.buttons & b) != 0));
                        if (b == HidMouseButton_Middle) rdp_conn_mouse_button(conn, 3, (int)((mst.buttons & b) != 0));
                    }
                }
                if (mst.wheel_delta_y)
                    rdp_conn_mouse_wheel(conn, mst.wheel_delta_y > 0 ? 1 : -1);
            }

            HidKeyboardState ks;
            int kn = (int)hidGetKeyboardStates(&ks, 1);
            kbd_states = kn;
            if (kn == 1)
            {
                u64 keys[4] = { ks.keys[0], ks.keys[1], ks.keys[2], ks.keys[3] };
                /* Usages 0xE0..0xE7 (Shift/Ctrl/Alt/Gui) live in word 3, bits
                 * 32..39; merge the modifiers bitfield in case the firmware
                 * only fills that one. */
                keys[3] |= (u64)(ks.modifiers & 0xFFu) << 32;
                kbd_mods = ks.modifiers;
                kbd_keys = keys[0] | keys[1] | keys[2] | keys[3];
                for (int w = 0; w < 4; w++)
                {
                    u64 diff = keys[w] ^ prev_keys[w];
                    while (diff)
                    {
                        int bit = __builtin_ctzll(diff);
                        int usage = w * 64 + bit;
                        int down, ext = 0;
                        uint16_t sc;
                        diff &= diff - 1;
                        down = (int)((keys[w] >> bit) & 1);
                        sc = rdp_scancode_from_hid((uint8_t)usage, &ext);
                        /* The first few events are logged: a USB keyboard that
                         * stays silent is then a known-unsupported device
                         * instead of a guess. */
                        if (kbd_events < 24)
                        {
                            kbd_events++;
                            tlog("kbd usage=%d sc=%04X ext=%d down=%d",
                                 usage, (unsigned)sc, ext, down);
                        }
                        if (sc) rdp_conn_key(conn, sc, down, ext);
                    }
                }
                prev_keys[0] = keys[0]; prev_keys[1] = keys[1];
                prev_keys[2] = keys[2]; prev_keys[3] = keys[3];
            }
        }

        {
            HidTouchScreenState ts;
            if (hidGetTouchScreenStates(&ts, 1) == 1 && ts.count > 0)
            {
                int tx = (int)(int32_t)ts.touches[0].x;
                int ty = (int)(int32_t)ts.touches[0].y;
                if (tx < 0) tx = 0;
                if (ty < 0) ty = 0;
                if (tx >= surf->width) tx = surf->width - 1;
                if (ty >= surf->height) ty = surf->height - 1;
                if (tx != mx || ty != my)
                {
                    mx = tx; my = ty;
                    rdp_conn_mouse_move(conn, mx, my);
                    last_mx = mx; last_my = my; last_move_ms = now_ms;
                    changed = 1;
                }
                if (!touch_down) { rdp_conn_mouse_button(conn, 1, 1); touch_down = 1; }
            }
            else if (touch_down) { rdp_conn_mouse_button(conn, 1, 0); touch_down = 0; }
        }

        padUpdate(&g_pad);
        u64 cur = padGetButtons(&g_pad);
        u64 pad_down = cur & ~prev_buttons;

        {
            /* Left stick = pointer.  The step is time based (px/s) instead of
             * per frame, so the feel does not change with the frame rate:
             * ~30 px/s just past the dead zone (single pixels per tick, good
             * for icons) up to ~1400 px/s pushed all the way. */
            HidAnalogStickState ls = padGetStickPos(&g_pad, 0);
            float nx = (float)ls.x / 32767.0f, ny = (float)ls.y / 32767.0f;
            float ax = nx < 0 ? -nx : nx, ay = ny < 0 ? -ny : ny;

            if ((ax > 0.12f || ay > 0.12f) && (now_ms - last_move_ms) >= 16)
            {
                float dt = (float)(now_ms - last_move_ms) / 1000.0f;
                if (dt > 0.05f) dt = 0.05f;     /* one slow frame must not teleport */
                if (ax > 0.12f)
                {
                    float v = (ax - 0.12f) / 0.88f;
                    float sp = 30.0f + 1400.0f * v * v * (0.5f + 0.5f * v);
                    accx += (nx > 0 ? sp : -sp) * dt;
                }
                if (ay > 0.12f)
                {
                    float v = (ay - 0.12f) / 0.88f;
                    float sp = 30.0f + 1400.0f * v * v * (0.5f + 0.5f * v);
                    accy -= (ny > 0 ? sp : -sp) * dt;   /* +y is up on the pad */
                }
                {
                    int dx = (int)accx, dy = (int)accy;
                    accx -= (float)dx;
                    accy -= (float)dy;
                    mx += dx;
                    my += dy;
                }
                if (mx < 0) mx = 0;
                if (my < 0) my = 0;
                if (mx >= surf->width) mx = surf->width - 1;
                if (my >= surf->height) my = surf->height - 1;
            }
        }
        if ((mx != last_mx || my != last_my) && (now_ms - last_move_ms) >= 16)
        {
            rdp_conn_mouse_move(conn, mx, my);
            last_mx = mx; last_my = my; last_move_ms = now_ms;
            changed = 1;
        }

        {
            /* Right stick, vertical = wheel.  One notch per repeat interval,
             * the interval shortens with the deflection (140 ms .. 45 ms). */
            HidAnalogStickState rs = padGetStickPos(&g_pad, 1);
            int a = rs.y < 0 ? -rs.y : rs.y;
            if (!lr_latch && a > 9000)
            {
                u64 interval = 140u - (u64)((a - 9000) * 95 / 23767);
                if (!last_wheel_ms || now_ms - last_wheel_ms >= interval)
                {
                    if (rdp_conn_mouse_wheel(conn, rs.y > 0 ? 1 : -1) == 0)
                        last_wheel_ms = now_ms ? now_ms : 1;
                }
            }
            else
            {
                last_wheel_ms = 0;
            }
        }

        /* L+R held 300 ms -> session menu */
        if ((cur & (HidNpadButton_L | HidNpadButton_R)) ==
            (HidNpadButton_L | HidNpadButton_R))
        {
            if (!lr_latch)
            {
                if (!lr_since) lr_since = now_ms;
                else if (now_ms - lr_since >= 300)
                {
                    int act;
                    lr_latch = 1;
                    lr_since = 0;
                    act = session_menu(conn);
                    padUpdate(&g_pad);
                    cur = padGetButtons(&g_pad);
                    prev_buttons = cur;
                    pad_down = 0;
                    for (int w = 0; w < 4; w++) prev_keys[w] = 0;
                    touch_down = 0;
                    last_move_ms = 0;
                    if (act == 1) return 1;
                    full = 1;
                    changed = 1;
                }
            }
        }
        else
        {
            lr_since = 0;
            if (!(cur & (HidNpadButton_L | HidNpadButton_R))) lr_latch = 0;
        }

        /* Y -> system keyboard */
        if (pad_down & HidNpadButton_Y)
        {
            tlog("pad Y: type into the remote");
            for (int k = 0; k < 16; k++) if (rdp_conn_pump(conn, 0) <= 0) break;
            draw_typing_hint();
            type_text_dialog(conn, NULL, 0);
            padUpdate(&g_pad);
            cur = padGetButtons(&g_pad);
            prev_buttons = cur;
            pad_down = 0;
            for (int w = 0; w < 4; w++) prev_keys[w] = 0;
            full = 1;
            changed = 1;
        }

        {
            u64 diff = cur ^ prev_buttons;
            while (diff)
            {
                int bit = __builtin_ctzll(diff);
                int down;
                uint16_t sc = 0;
                int ext = 0;
                diff &= diff - 1;
                down = (int)((cur >> bit) & 1);
                switch (1ULL << bit)
                {
                    case HidNpadButton_Up:    sc = 0x48; ext = 1; break;
                    case HidNpadButton_Down:  sc = 0x50; ext = 1; break;
                    case HidNpadButton_Left:  sc = 0x4B; ext = 1; break;
                    case HidNpadButton_Right: sc = 0x4D; ext = 1; break;
                    case HidNpadButton_A:     sc = 0x1C; break;
                    case HidNpadButton_B:     sc = 0x01; break;
                    case HidNpadButton_ZL:    rdp_conn_mouse_button(conn, 1, down); break;
                    case HidNpadButton_ZR:    rdp_conn_mouse_button(conn, 2, down); break;
                    default: break;
                }
                if (sc) rdp_conn_key(conn, sc, down, ext);
            }
            prev_buttons = cur;
        }

        if (changed)
        {
            int px = cx, py = cy, x0 = ax0, y0 = ay0, x1 = ax1, y1 = ay1;
            u64 t0 = svcGetSystemTick();
            cx = mx; cy = my;
            if (full) { x0 = 0; y0 = 0; x1 = surf->width; y1 = surf->height; full = 0; }
            else
            {
                int bx0 = cx < px ? cx : px, by0 = cy < py ? cy : py;
                int bx1 = (cx > px ? cx : px) + CURSOR_W;
                int by1 = (cy > py ? cy : py) + CURSOR_H;
                if (x1 <= x0 || y1 <= y0) { x0 = bx0; y0 = by0; x1 = bx1; y1 = by1; }
                else
                {
                    if (bx0 < x0) x0 = bx0;
                    if (by0 < y0) y0 = by0;
                    if (bx1 > x1) x1 = bx1;
                    if (by1 > y1) y1 = by1;
                }
            }
            fb_present(surf, x0, y0, x1, y1, cx, cy);
            blit_ms += (svcGetSystemTick() - t0) / 19200ULL;
            blits++;
        }

        if ((svcGetSystemTick() - t_report) > 19200000ULL * 5ULL)
        {
            u64 dt = (svcGetSystemTick() - t_report) / 19200000ULL;
            char codes[128];
            int o = 0;
            t_report = svcGetSystemTick();
            tlog("perf %llus frames=%llu pdus=%llu blits=%llu avg_blit=%llums avg_pump=%llums",
                 (unsigned long long)dt, (unsigned long long)frames,
                 (unsigned long long)pdus, (unsigned long long)blits,
                 (unsigned long long)(blits ? blit_ms / blits : 0),
                 (unsigned long long)(frames ? pump_ms / frames : 0));
            /* kbd_states stays 0 when the HID server has no keyboard device at
             * all, which is a completely different problem from a bad scancode
             * mapping; kbd_keys/mod show whether the lifo ever carries input. */
            tlog("kbd states=%d ev=%llu mods=%016llX keys=%016llX",
                 kbd_states, (unsigned long long)kbd_events,
                 (unsigned long long)kbd_mods, (unsigned long long)kbd_keys);
            {
                /* Dump the *newest* entry of the keyboard and mouse rings
                 * (slot tail-1, not slot 0 - slot 0 may be stale).  The
                 * keyboard entry never carries any key bits and its
                 * "modifiers" word is a constant 0x400, so the field layout
                 * and the input routing both need checking against bytes. */
                HidSharedMemory* sm = (HidSharedMemory*)hidGetSharedmemAddr();
                if (sm)
                {
                    char hex[200];
                    int h;
                    unsigned kslot = 0, mslot = 0;

                    if (sm->keyboard.lifo.header.buffer_count)
                        kslot = (unsigned)((sm->keyboard.lifo.header.tail +
                                            sm->keyboard.lifo.header.buffer_count - 1) %
                                           sm->keyboard.lifo.header.buffer_count);
                    if (sm->mouse.lifo.header.buffer_count)
                        mslot = (unsigned)((sm->mouse.lifo.header.tail +
                                            sm->mouse.lifo.header.buffer_count - 1) %
                                           sm->mouse.lifo.header.buffer_count);

                    h = 0;
                    {
                        const uint8_t* raw = (const uint8_t*)&sm->keyboard.lifo.storage[kslot];
                        for (int k = 0; k < 56 && h < 100; k++)
                            h += snprintf(hex + h, sizeof(hex) - h, "%02x", raw[k]);
                        tlog("kbd raw slot=%u tail=%llu cnt=%llu %s", kslot,
                             (unsigned long long)sm->keyboard.lifo.header.tail,
                             (unsigned long long)sm->keyboard.lifo.header.count, hex);
                    }
                    h = 0;
                    {
                        const uint8_t* raw = (const uint8_t*)&sm->mouse.lifo.storage[mslot];
                        for (int k = 0; k < 56 && h < 100; k++)
                            h += snprintf(hex + h, sizeof(hex) - h, "%02x", raw[k]);
                        tlog("ms raw slot=%u tail=%llu cnt=%llu %s", mslot,
                             (unsigned long long)sm->mouse.lifo.header.tail,
                             (unsigned long long)sm->mouse.lifo.header.count, hex);
                    }
                }
            }
            codes[0] = 0;
            for (int i = 0; i < 16; i++)
                if (rdp_update_other_codes[i])
                    o += snprintf(codes + o, sizeof(codes) - o, "%d:%d ", i,
                                  rdp_update_other_codes[i]);
            tlog("codes %s", o ? codes : "none");
        }

        /* Idle nudge: only after 5 s without a single PDU (t_nudge is reset by
         * real traffic), so a live desktop is never re-sent wholesale. */
        if (pdus > 0 && (svcGetSystemTick() - t_nudge) > 19200000ULL * 5ULL)
        {
            t_nudge = svcGetSystemTick();
            rdp_conn_refresh(conn, 0, 0, SCR_W - 1, SCR_H - 1);
            tlog("nudge: idle refresh (pdus=%llu)", (unsigned long long)pdus);
        }

        if (!pdus) svcSleepThread(500000ULL);
    }
}

/* ------------------------------------------------------------------ */
int main(void)
{
    rdp_console_live = 0;               /* no libnx console in this build */

    tlog_open();
    {
        Result mrc = fsdevMountSdmc();
        tlog("RDP %s boot (sdmc 0x%x)", SWRDP_BUILD, mrc);
    }
    {
        Result nrc = nifmInitialize(NifmServiceType_User);
        Result src = socketInitializeDefault();
        tlog("nifm 0x%x socket 0x%x", nrc, src);
    }
    hidInitializeKeyboard();
    hidInitializeMouse();
    hidInitializeTouchScreen();
    {
        static const HidNpadIdType ids[] = { HidNpadIdType_No1, HidNpadIdType_Handheld };
        hidInitializeNpad();
        hidSetSupportedNpadIdType(ids, 2);
        hidSetSupportedNpadStyleSet(HidNpadStyleSet_NpadStandard);
    }
    padInitializeDefault(&g_pad);
    {
        /* Application vs library applet decides which HID inputs the server
         * routes to us; a USB keyboard that never shows up is far easier to
         * explain with this line in the log. */
        HidKeyboardState st[8];
        size_t nb = hidGetKeyboardStates(st, 8);
        tlog("applet type=%d focus=%d aruid=%016llx kbd_count=%d",
             (int)appletGetAppletType(), (int)appletGetFocusState(),
             (unsigned long long)appletGetAppletResourceUserId(), (int)nb);
    }

    rdp_ui_stage_hook = stage_hook;
    rdp_cancel_hook = ui_poll_cancel;
    rdp_update_log_hook = update_log;

    cfg_ensure();
    g_cfg_fields = cfg_load();

    if (fb_setup() != 0)
        for (;;) svcSleepThread(1000000000ULL);

    main_screen_loop();

    for (;;) svcSleepThread(1000000000ULL);   /* no exit path on purpose */
    return 0;
}
