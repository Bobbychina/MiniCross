// MiniCross-Core v2 —— 旧版 C# 功能全量迁移
// 纯 Win32 + UpdateLayeredWindow + WIC，零运行时依赖
//
// 图层: cross / diagonal / dot / circle / bracket / image
// 方案: [profile:name] 覆盖 section.key，F10 循环
// 热键: F8 显隐 | F9 重载 | F10 下一方案 | Ctrl+Alt+E 配置台 | Ctrl+Alt+Q 退出
//        Ctrl+Alt+方向键 微调（不写回文件）
//
// 编译:
//   gcc -Os -s -mwindows "-Wl,--stack=65536" crosshair_v2.c -o crosshair.exe \
//       -lgdi32 -luser32 -lole32 -lwindowscodecs -luuid -lshell32

#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <wincodec.h>
#include <shellapi.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <commctrl.h>
#include <commdlg.h>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#ifndef M_PI
#define M_PI 3.14159265358979
#endif

#define WM_APP_TRIM   (WM_APP + 1)
#define WM_APP_TOP    (WM_APP + 2)
#define WM_APP_WATCH  (WM_APP + 3)

// ---------------- 颜色 ----------------
static int parse_color(const char *v, int def) {
    if (!v || !*v) return def;
    while (*v == ' ' || *v == '\t') v++;
    static const struct { const char *n; int c; } names[] = {
        {"red",0xFFFF0000},{"lime",0xFF00FF00},{"green",0xFF008000},{"blue",0xFF0000FF},
        {"cyan",0xFF00FFFF},{"magenta",0xFFFF00FF},{"yellow",0xFFFFFF00},{"white",0xFFFFFFFF},
        {"black",0xFF000000},{"orange",0xFFFFA500},{"pink",0xFFFFC0CB},{"purple",0xFF800080},
        {"gray",0xFF808080},{"grey",0xFF808080},{"transparent",0x00000000}
    };
    for (int i = 0; i < (int)(sizeof(names)/sizeof(names[0])); i++)
        if (!_stricmp(v, names[i].n)) return names[i].c;
    if (*v == '#') v++;
    int n = (int)strlen(v);
    char buf[16]; int i;
    for (i = 0; i < n && i < 15; i++) buf[i] = v[i];
    buf[i] = 0;
    if (n == 3) {
        char h[3] = {buf[0], buf[0], 0};
        int r = (int)strtol(h, NULL, 16);
        h[0] = buf[1]; int g = (int)strtol(h, NULL, 16);
        h[0] = buf[2]; int b = (int)strtol(h, NULL, 16);
        return (int)(0xFF000000u | (r << 16) | (g << 8) | b);
    }
    if (n == 6) return (int)(0xFF000000u | (unsigned)strtoul(buf, NULL, 16));
    if (n == 8) return (int)strtoul(buf, NULL, 16);
    return def;
}

static int parse_arms(const char *v, int def) {
    if (!v || !*v) return def;
    while (*v == ' ') v++;
    if (v[0] == '0' || v[0] == '1') {
        int m = 0;
        for (int i = 0; i < 4 && v[i]; i++) if (v[i] == '1') m |= (1 << i);
        return m;
    }
    int m = 0;
    if (strstr(v, "up"))    m |= 1;
    if (strstr(v, "down"))  m |= 2;
    if (strstr(v, "left"))  m |= 4;
    if (strstr(v, "right")) m |= 8;
    return m;
}

static int parse_bool(const char *v, int def) {
    if (!v) return def;
    while (*v == ' ') v++;
    if (!_stricmp(v, "1") || !_stricmp(v, "true") || !_stricmp(v, "yes") || !_stricmp(v, "on")) return 1;
    if (!_stricmp(v, "0") || !_stricmp(v, "false") || !_stricmp(v, "no") || !_stricmp(v, "off")) return 0;
    return atoi(v) ? 1 : 0;
}

// ---------------- 配置 ----------------
typedef struct { int enabled, color, alpha, outline, outline_color, outline_alpha; } LBASE;
typedef struct { LBASE b; int length, thickness, gap, arms; } CROSS;
typedef struct { LBASE b; int length, thickness, gap, arms, angle; } DIAG;
typedef struct { LBASE b; int size; } DOT;
typedef struct { LBASE b; int radius, thickness; } CIRCLE;
typedef struct { LBASE b; int size, length, thickness; } BRACKET;
typedef struct { LBASE b; char path[MAX_PATH]; int size, angle; } IMAGE;

typedef struct {
    char profile[64];
    int  monitor;
    double x_percent, y_percent;
    int  offset_x, offset_y;
    double scale, rotation;
    int  nudge_step;
    int  hide_from_capture, antialias, auto_reload;
    int  keep_on_top, topmost_check_ms;
    int  trim_memory, trim_delay_ms, trim_active_ms, trim_window_ms, trim_idle_ms;
    char toggle_key[32], open_ui_key[32], reload_key[32], next_profile_key[32], quit_key[32];
    char nudge_up[32], nudge_down[32], nudge_left[32], nudge_right[32];
    CROSS cross; DIAG diag; DOT dot; CIRCLE circle; BRACKET bracket; IMAGE image;
} CFG;

static CFG   g_cfg;
static HWND  g_wnd;
static HDC   g_memdc;
static HBITMAP g_bmp;
static void *g_bits;
static int   g_w, g_h, g_cx, g_cy;
static int   g_visible = 1;
static DWORD g_last_activity = 0;
static int   g_nudge_x = 0, g_nudge_y = 0;
static char  g_ini_path[MAX_PATH];
static char  g_profile_override[64] = "";

static unsigned char *g_img_px = NULL;
static int g_img_w = 0, g_img_h = 0;
static char g_img_loaded[MAX_PATH] = "";

static int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void base_default(LBASE *b, int color) {
    b->enabled = 0; b->color = color; b->alpha = 255;
    b->outline = 0; b->outline_color = 0xFF000000; b->outline_alpha = 200;
}

static void cfg_default(CFG *c) {
    memset(c, 0, sizeof(*c));
    lstrcpyA(c->profile, "default");
    c->monitor = 0; c->x_percent = 50.0; c->y_percent = 50.0;
    c->offset_x = 0; c->offset_y = 0; c->scale = 1.0; c->rotation = 0.0;
    c->nudge_step = 1; c->hide_from_capture = 0; c->antialias = 1; c->auto_reload = 1;
    c->keep_on_top = 1; c->topmost_check_ms = 1500;
    c->trim_memory = 1; c->trim_delay_ms = 20000;
    c->trim_active_ms = 2000; c->trim_window_ms = 30000; c->trim_idle_ms = 60000;
    lstrcpyA(c->toggle_key, "F8"); lstrcpyA(c->open_ui_key, "Ctrl+Alt+E");
    lstrcpyA(c->reload_key, "F9"); lstrcpyA(c->next_profile_key, "F10");
    lstrcpyA(c->quit_key, "Ctrl+Alt+Q");
    lstrcpyA(c->nudge_up, "Ctrl+Alt+Up"); lstrcpyA(c->nudge_down, "Ctrl+Alt+Down");
    lstrcpyA(c->nudge_left, "Ctrl+Alt+Left"); lstrcpyA(c->nudge_right, "Ctrl+Alt+Right");

    base_default(&c->cross.b, 0xFF00FF00);
    c->cross.b.enabled = 1; c->cross.length = 10; c->cross.thickness = 2;
    c->cross.gap = 4; c->cross.arms = 0xF; c->cross.b.outline = 1; c->cross.b.outline_alpha = 200;

    base_default(&c->diag.b, 0xFF00FF00);
    c->diag.length = 8; c->diag.thickness = 1; c->diag.gap = 4;
    c->diag.arms = 0xF; c->diag.angle = 45; c->diag.b.alpha = 160;

    base_default(&c->dot.b, 0xFF00FF00);
    c->dot.b.enabled = 1; c->dot.size = 3;

    base_default(&c->circle.b, 0xFF00FF00);
    c->circle.radius = 18; c->circle.thickness = 1; c->circle.b.alpha = 150;

    base_default(&c->bracket.b, 0xFF00FF00);
    c->bracket.size = 16; c->bracket.length = 6; c->bracket.thickness = 2;
    c->bracket.b.alpha = 200; c->bracket.b.outline = 1;

    base_default(&c->image.b, 0xFFFFFFFF);
    lstrcpyA(c->image.path, "crosshair.png");
    c->image.size = 64; c->image.angle = 0;
}

// ---------------- ini ----------------
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static void strip_comment(char *s) {
    for (char *p = s; *p; p++) {
        if ((*p == ';' || *p == '#') && (p == s || p[-1] == ' ' || p[-1] == '\t')) { *p = 0; return; }
    }
}

static int key_is(const char *k, const char *name) { return !_stricmp(k, name); }

static int apply_kv(CFG *c, const char *key, const char *val) {
    char sec[32] = "", k[64] = "";
    const char *dot = strchr(key, '.');
    if (!dot) return 0;
    int sl = (int)(dot - key);
    if (sl >= (int)sizeof(sec)) sl = sizeof(sec) - 1;
    memcpy(sec, key, sl); sec[sl] = 0;
    lstrcpynA(k, dot + 1, sizeof(k));

    if (!_stricmp(sec, "general")) {
        if (key_is(k, "profile"))          { lstrcpynA(c->profile, val, sizeof(c->profile)); return 1; }
        if (key_is(k, "monitor"))          { c->monitor = atoi(val); return 1; }
        if (key_is(k, "x_percent"))        { c->x_percent = atof(val); return 1; }
        if (key_is(k, "y_percent"))        { c->y_percent = atof(val); return 1; }
        if (key_is(k, "offset_x"))         { c->offset_x = atoi(val); return 1; }
        if (key_is(k, "offset_y"))         { c->offset_y = atoi(val); return 1; }
        if (key_is(k, "scale"))            { c->scale = atof(val); return 1; }
        if (key_is(k, "rotation"))         { c->rotation = atof(val); return 1; }
        if (key_is(k, "nudge_step"))       { c->nudge_step = atoi(val); return 1; }
        if (key_is(k, "hide_from_capture")){ c->hide_from_capture = parse_bool(val, 0); return 1; }
        if (key_is(k, "antialias"))        { c->antialias = parse_bool(val, 1); return 1; }
        if (key_is(k, "auto_reload"))      { c->auto_reload = parse_bool(val, 1); return 1; }
        if (key_is(k, "keep_on_top"))      { c->keep_on_top = parse_bool(val, 1); return 1; }
        if (key_is(k, "topmost_check_ms")) { c->topmost_check_ms = iclamp(atoi(val), 200, 60000); return 1; }
        if (key_is(k, "trim_memory"))      { c->trim_memory = parse_bool(val, 1); return 1; }
        if (key_is(k, "trim_delay_ms"))    { c->trim_delay_ms = iclamp(atoi(val), 0, 600000); return 1; }
        if (key_is(k, "trim_active_ms"))   { c->trim_active_ms = iclamp(atoi(val), 100, 60000); return 1; }
        if (key_is(k, "trim_window_ms"))   { c->trim_window_ms = iclamp(atoi(val), 0, 600000); return 1; }
        if (key_is(k, "trim_idle_ms"))     { c->trim_idle_ms = iclamp(atoi(val), 0, 3600000); return 1; }
        return 1;
    }
    if (!_stricmp(sec, "hotkeys")) {
        if (key_is(k, "toggle"))       { lstrcpynA(c->toggle_key, val, 32); return 1; }
        if (key_is(k, "open_ui"))      { lstrcpynA(c->open_ui_key, val, 32); return 1; }
        if (key_is(k, "reload"))       { lstrcpynA(c->reload_key, val, 32); return 1; }
        if (key_is(k, "next_profile")) { lstrcpynA(c->next_profile_key, val, 32); return 1; }
        if (key_is(k, "quit"))         { lstrcpynA(c->quit_key, val, 32); return 1; }
        if (key_is(k, "nudge_up"))     { lstrcpynA(c->nudge_up, val, 32); return 1; }
        if (key_is(k, "nudge_down"))   { lstrcpynA(c->nudge_down, val, 32); return 1; }
        if (key_is(k, "nudge_left"))   { lstrcpynA(c->nudge_left, val, 32); return 1; }
        if (key_is(k, "nudge_right"))  { lstrcpynA(c->nudge_right, val, 32); return 1; }
        return 1;
    }
    if (!_stricmp(sec, "cross")) {
        if (key_is(k, "enabled"))       { c->cross.b.enabled = parse_bool(val, 1); return 1; }
        if (key_is(k, "length"))        { c->cross.length = iclamp(atoi(val), 0, 256); return 1; }
        if (key_is(k, "thickness"))     { c->cross.thickness = iclamp(atoi(val), 1, 16); return 1; }
        if (key_is(k, "gap"))           { c->cross.gap = iclamp(atoi(val), 0, 128); return 1; }
        if (key_is(k, "arms"))          { c->cross.arms = parse_arms(val, 0xF); return 1; }
        if (key_is(k, "color"))         { c->cross.b.color = parse_color(val, 0xFF00FF00); return 1; }
        if (key_is(k, "alpha"))         { c->cross.b.alpha = iclamp(atoi(val), 0, 255); return 1; }
        if (key_is(k, "outline"))       { c->cross.b.outline = iclamp(atoi(val), 0, 8); return 1; }
        if (key_is(k, "outline_color")) { c->cross.b.outline_color = parse_color(val, 0xFF000000); return 1; }
        if (key_is(k, "outline_alpha")) { c->cross.b.outline_alpha = iclamp(atoi(val), 0, 255); return 1; }
        return 1;
    }
    if (!_stricmp(sec, "diagonal")) {
        if (key_is(k, "enabled"))       { c->diag.b.enabled = parse_bool(val, 0); return 1; }
        if (key_is(k, "length"))        { c->diag.length = iclamp(atoi(val), 0, 256); return 1; }
        if (key_is(k, "thickness"))     { c->diag.thickness = iclamp(atoi(val), 1, 16); return 1; }
        if (key_is(k, "gap"))           { c->diag.gap = iclamp(atoi(val), 0, 128); return 1; }
        if (key_is(k, "arms"))          { c->diag.arms = parse_arms(val, 0xF); return 1; }
        if (key_is(k, "angle"))         { c->diag.angle = atoi(val); return 1; }
        if (key_is(k, "color"))         { c->diag.b.color = parse_color(val, 0xFF00FF00); return 1; }
        if (key_is(k, "alpha"))         { c->diag.b.alpha = iclamp(atoi(val), 0, 255); return 1; }
        if (key_is(k, "outline"))       { c->diag.b.outline = iclamp(atoi(val), 0, 8); return 1; }
        if (key_is(k, "outline_color")) { c->diag.b.outline_color = parse_color(val, 0xFF000000); return 1; }
        if (key_is(k, "outline_alpha")) { c->diag.b.outline_alpha = iclamp(atoi(val), 0, 255); return 1; }
        return 1;
    }
    if (!_stricmp(sec, "dot")) {
        if (key_is(k, "enabled"))       { c->dot.b.enabled = parse_bool(val, 1); return 1; }
        if (key_is(k, "size"))          { c->dot.size = iclamp(atoi(val), 0, 64); return 1; }
        if (key_is(k, "color"))         { c->dot.b.color = parse_color(val, 0xFF00FF00); return 1; }
        if (key_is(k, "alpha"))         { c->dot.b.alpha = iclamp(atoi(val), 0, 255); return 1; }
        if (key_is(k, "outline"))       { c->dot.b.outline = iclamp(atoi(val), 0, 8); return 1; }
        if (key_is(k, "outline_color")) { c->dot.b.outline_color = parse_color(val, 0xFF000000); return 1; }
        if (key_is(k, "outline_alpha")) { c->dot.b.outline_alpha = iclamp(atoi(val), 0, 255); return 1; }
        return 1;
    }
    if (!_stricmp(sec, "circle")) {
        if (key_is(k, "enabled"))       { c->circle.b.enabled = parse_bool(val, 0); return 1; }
        if (key_is(k, "radius"))        { c->circle.radius = iclamp(atoi(val), 0, 256); return 1; }
        if (key_is(k, "thickness"))     { c->circle.thickness = iclamp(atoi(val), 1, 16); return 1; }
        if (key_is(k, "color"))         { c->circle.b.color = parse_color(val, 0xFF00FF00); return 1; }
        if (key_is(k, "alpha"))         { c->circle.b.alpha = iclamp(atoi(val), 0, 255); return 1; }
        if (key_is(k, "outline"))       { c->circle.b.outline = iclamp(atoi(val), 0, 8); return 1; }
        if (key_is(k, "outline_color")) { c->circle.b.outline_color = parse_color(val, 0xFF000000); return 1; }
        if (key_is(k, "outline_alpha")) { c->circle.b.outline_alpha = iclamp(atoi(val), 0, 255); return 1; }
        return 1;
    }
    if (!_stricmp(sec, "bracket")) {
        if (key_is(k, "enabled"))       { c->bracket.b.enabled = parse_bool(val, 0); return 1; }
        if (key_is(k, "size"))          { c->bracket.size = iclamp(atoi(val), 0, 256); return 1; }
        if (key_is(k, "length"))        { c->bracket.length = iclamp(atoi(val), 0, 128); return 1; }
        if (key_is(k, "thickness"))     { c->bracket.thickness = iclamp(atoi(val), 1, 16); return 1; }
        if (key_is(k, "color"))         { c->bracket.b.color = parse_color(val, 0xFF00FF00); return 1; }
        if (key_is(k, "alpha"))         { c->bracket.b.alpha = iclamp(atoi(val), 0, 255); return 1; }
        if (key_is(k, "outline"))       { c->bracket.b.outline = iclamp(atoi(val), 0, 8); return 1; }
        if (key_is(k, "outline_color")) { c->bracket.b.outline_color = parse_color(val, 0xFF000000); return 1; }
        if (key_is(k, "outline_alpha")) { c->bracket.b.outline_alpha = iclamp(atoi(val), 0, 255); return 1; }
        return 1;
    }
    if (!_stricmp(sec, "image")) {
        if (key_is(k, "enabled")) { c->image.b.enabled = parse_bool(val, 0); return 1; }
        if (key_is(k, "path"))    { lstrcpynA(c->image.path, val, MAX_PATH); return 1; }
        if (key_is(k, "size"))    { c->image.size = iclamp(atoi(val), 1, 512); return 1; }
        if (key_is(k, "angle"))   { c->image.angle = atoi(val); return 1; }
        if (key_is(k, "alpha"))   { c->image.b.alpha = iclamp(atoi(val), 0, 255); return 1; }
        return 1;
    }
    return 0;
}

static char *read_file(const char *path, DWORD *out_len) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { *out_len = 0; return NULL; }
    DWORD sz = GetFileSize(f, NULL), rd = 0;
    char *buf = (char *)malloc(sz + 1);
    if (buf) { ReadFile(f, buf, sz, &rd, NULL); buf[rd] = 0; }
    CloseHandle(f);
    *out_len = rd;
    return buf;
}

// 三遍解析：1 找当前 profile 名；2 应用基础段；3 应用 profile 覆盖
static void cfg_load(const char *path) {
    DWORD len = 0;
    char *src = read_file(path, &len);
    if (!src) return;
    cfg_default(&g_cfg);

    char section[64] = "";
    char cur[64] = "default";
    char *p = src;

    // pass 1
    while (p && *p) {
        char *nl = strchr(p, '\n');
        char line[512];
        int ll = nl ? (int)(nl - p) : (int)strlen(p);
        if (ll > 511) ll = 511;
        memcpy(line, p, ll); line[ll] = 0;
        p = nl ? nl + 1 : NULL;
        char *s = trim(line); strip_comment(s); s = trim(s);
        if (!*s) continue;
        if (*s == '[') { char *e = strchr(s, ']'); if (e) { *e = 0; lstrcpynA(section, s + 1, sizeof(section)); } continue; }
        char *eq = strchr(s, '='); if (!eq) continue;
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        if (!_stricmp(section, "general") && key_is(k, "profile")) lstrcpynA(cur, v, sizeof(cur));
    }
    if (g_profile_override[0]) lstrcpynA(cur, g_profile_override, sizeof(cur));

    // pass 2：基础段
    p = src; section[0] = 0;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        char line[512];
        int ll = nl ? (int)(nl - p) : (int)strlen(p);
        if (ll > 511) ll = 511;
        memcpy(line, p, ll); line[ll] = 0;
        p = nl ? nl + 1 : NULL;
        char *s = trim(line); strip_comment(s); s = trim(s);
        if (!*s) continue;
        if (*s == '[') { char *e = strchr(s, ']'); if (e) { *e = 0; lstrcpynA(section, s + 1, sizeof(section)); } continue; }
        char *eq = strchr(s, '='); if (!eq) continue;
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        if (!section[0] || !_strnicmp(section, "profile:", 8)) continue;
        char full[128];
        snprintf(full, sizeof(full), "%s.%s", section, k);
        apply_kv(&g_cfg, full, v);
    }

    // pass 3：当前 profile 覆盖
    char want[96];
    snprintf(want, sizeof(want), "profile:%s", cur);
    p = src; section[0] = 0;
    int in_target = 0;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        char line[512];
        int ll = nl ? (int)(nl - p) : (int)strlen(p);
        if (ll > 511) ll = 511;
        memcpy(line, p, ll); line[ll] = 0;
        p = nl ? nl + 1 : NULL;
        char *s = trim(line); strip_comment(s); s = trim(s);
        if (!*s) continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (e) { *e = 0; lstrcpynA(section, s + 1, sizeof(section)); }
            in_target = !_stricmp(section, want);
            continue;
        }
        if (!in_target) continue;
        char *eq = strchr(s, '='); if (!eq) continue;
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        if (strchr(k, '.')) apply_kv(&g_cfg, k, v);
        else {
            char full[128];
            snprintf(full, sizeof(full), "general.%s", k);
            apply_kv(&g_cfg, full, v);
        }
    }
    if (g_profile_override[0]) lstrcpynA(g_cfg.profile, cur, sizeof(g_cfg.profile));
    free(src);
}

// ---------------- 位图 ----------------
static void ensure_bitmap(int w, int h) {
    if (g_bits && g_w == w && g_h == h) return;
    if (g_bmp)   { DeleteObject(g_bmp);  g_bmp = NULL; }
    if (g_memdc) { DeleteDC(g_memdc);    g_memdc = NULL; }
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &g_bits, NULL, 0);
    g_memdc = CreateCompatibleDC(NULL);
    SelectObject(g_memdc, g_bmp);
    g_w = w; g_h = h; g_cx = w / 2; g_cy = h / 2;
}

// DIB 是预乘 ARGB
static void put_px(int x, int y, int argb) {
    if (x < 0 || y < 0 || x >= g_w || y >= g_h) return;
    int a = (argb >> 24) & 255;
    if (!a) return;
    int r = (argb >> 16) & 255, g = (argb >> 8) & 255, b = argb & 255;
    unsigned char *p = (unsigned char *)g_bits + ((size_t)y * g_w + x) * 4;
    p[0] = (unsigned char)(b * a / 255);
    p[1] = (unsigned char)(g * a / 255);
    p[2] = (unsigned char)(r * a / 255);
    p[3] = (unsigned char)a;
}

static int with_alpha(int argb, int alpha) {
    int a = (argb >> 24) & 255;
    if (a == 255) a = alpha; else a = a * alpha / 255;
    return (a << 24) | (argb & 0xFFFFFF);
}

static void fill_rect(int x0, int y0, int w, int h, int argb) {
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) put_px(x, y, argb);
}

static void fill_disc(int cx, int cy, int rad, int argb) {
    if (rad <= 0) { put_px(cx, cy, argb); return; }
    for (int y = -rad; y <= rad; y++)
        for (int x = -rad; x <= rad; x++)
            if (x * x + y * y <= rad * rad) put_px(cx + x, cy + y, argb);
}

static void fill_ring(int cx, int cy, int rad, int th, int argb) {
    int ro = rad * rad, ri = (rad - th) * (rad - th);
    if (ri < 0) ri = 0;
    for (int y = -rad; y <= rad; y++)
        for (int x = -rad; x <= rad; x++) {
            int d = x * x + y * y;
            if (d <= ro && d >= ri) put_px(cx + x, cy + y, argb);
        }
}

static void draw_line(int x0, int y0, int x1, int y1, int w, int argb) {
    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy, half = w / 2;
    for (;;) {
        if (w <= 1) put_px(x0, y0, argb);
        else fill_rect(x0 - half, y0 - half, w, w, argb);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

static void draw_cross_arms(int cx, int cy, int len, int th, int gap, int arms,
                            int color, int alpha, int outline, int ocolor, int oalpha) {
    int T = th, o = outline;
    if (arms & 1) {
        if (o) draw_line(cx, cy - gap, cx, cy - gap - len, T + o * 2, with_alpha(ocolor, oalpha));
        draw_line(cx, cy - gap, cx, cy - gap - len, T, with_alpha(color, alpha));
    }
    if (arms & 2) {
        if (o) draw_line(cx, cy + gap, cx, cy + gap + len, T + o * 2, with_alpha(ocolor, oalpha));
        draw_line(cx, cy + gap, cx, cy + gap + len, T, with_alpha(color, alpha));
    }
    if (arms & 4) {
        if (o) draw_line(cx - gap, cy, cx - gap - len, cy, T + o * 2, with_alpha(ocolor, oalpha));
        draw_line(cx - gap, cy, cx - gap - len, cy, T, with_alpha(color, alpha));
    }
    if (arms & 8) {
        if (o) draw_line(cx + gap, cy, cx + gap + len, cy, T + o * 2, with_alpha(ocolor, oalpha));
        draw_line(cx + gap, cy, cx + gap + len, cy, T, with_alpha(color, alpha));
    }
}

static void draw_bracket(int cx, int cy, int size, int len, int th,
                         int color, int alpha, int outline, int ocolor, int oalpha) {
    int o = outline;
    for (int q = 0; q < 4; q++) {
        int dx = (q & 1) ? 1 : -1;
        int dy = (q & 2) ? 1 : -1;
        int px = cx + dx * size, py = cy + dy * size;
        if (o) {
            draw_line(px, py, px - dx * len, py, th + o * 2, with_alpha(ocolor, oalpha));
            draw_line(px, py, px, py - dy * len, th + o * 2, with_alpha(ocolor, oalpha));
        }
        draw_line(px, py, px - dx * len, py, th, with_alpha(color, alpha));
        draw_line(px, py, px, py - dy * len, th, with_alpha(color, alpha));
    }
}

// ---------------- WIC 图片 ----------------
static void load_png(const char *path) {
    if (!path || !*path) return;
    if (!_stricmp(g_img_loaded, path)) return;
    if (g_img_px) { free(g_img_px); g_img_px = NULL; }
    g_img_w = g_img_h = 0;
    lstrcpynA(g_img_loaded, path, MAX_PATH);

    WCHAR wpath[MAX_PATH];
    if (!MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH)) return;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IWICImagingFactory *fac = NULL;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IWICImagingFactory, (void **)&fac)) || !fac) return;

    IWICBitmapDecoder *dec = NULL;
    if (FAILED(fac->lpVtbl->CreateDecoderFromFilename(fac, wpath, NULL, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnLoad, &dec))) {
        fac->lpVtbl->Release(fac); return;
    }
    IWICBitmapFrameDecode *frame = NULL;
    if (FAILED(dec->lpVtbl->GetFrame(dec, 0, &frame))) {
        dec->lpVtbl->Release(dec); fac->lpVtbl->Release(fac); return;
    }
    IWICFormatConverter *conv = NULL;
    if (SUCCEEDED(fac->lpVtbl->CreateFormatConverter(fac, &conv))) {
        if (SUCCEEDED(conv->lpVtbl->Initialize(conv, (IWICBitmapSource *)frame,
                    &GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0,
                    WICBitmapPaletteTypeCustom))) {
            UINT w = 0, h = 0;
            conv->lpVtbl->GetSize(conv, &w, &h);
            if (w && h && w < 4096 && h < 4096) {
                UINT stride = w * 4, size = stride * h;
                unsigned char *buf = (unsigned char *)malloc(size);
                if (buf && SUCCEEDED(conv->lpVtbl->CopyPixels(conv, NULL, stride, size, buf))) {
                    g_img_px = buf; g_img_w = (int)w; g_img_h = (int)h;
                } else free(buf);
            }
            conv->lpVtbl->Release(conv);
        }
    }
    frame->lpVtbl->Release(frame);
    dec->lpVtbl->Release(dec);
    fac->lpVtbl->Release(fac);
}

static void draw_image(int cx, int cy, int size, int angle, int alpha) {
    if (!g_img_px || !g_img_w || !g_img_h) return;
    double sc = (double)size / (double)(g_img_w > g_img_h ? g_img_w : g_img_h);
    double rad = angle * M_PI / 180.0;
    double ca = cos(rad), sa = sin(rad);
    int half = size / 2 + 2;
    for (int y = -half; y <= half; y++)
        for (int x = -half; x <= half; x++) {
            double rx =  x * ca + y * sa;
            double ry = -x * sa + y * ca;
            int sx = (int)(g_img_w / 2.0 + rx / sc);
            int sy = (int)(g_img_h / 2.0 + ry / sc);
            if (sx < 0 || sy < 0 || sx >= g_img_w || sy >= g_img_h) continue;
            unsigned char *p = g_img_px + ((size_t)sy * g_img_w + sx) * 4;
            if (!p[3]) continue;
            put_px(cx + x, cy + y, (iclamp(p[3] * alpha / 255, 0, 255) << 24) |
                   (p[2] << 16) | (p[1] << 8) | p[0]);
        }
}

// ---------------- 渲染 ----------------
static int layer_extent(void) {
    int ext = 0;
    double s = g_cfg.scale;
    if (g_cfg.cross.b.enabled) {
        int e = (int)((g_cfg.cross.gap + g_cfg.cross.length + g_cfg.cross.thickness) * s) + 4;
        if (e > ext) ext = e;
    }
    if (g_cfg.diag.b.enabled) {
        int e = (int)((g_cfg.diag.gap + g_cfg.diag.length + g_cfg.diag.thickness) * s) + 4;
        if (e > ext) ext = e;
    }
    if (g_cfg.circle.b.enabled) {
        int e = (int)((g_cfg.circle.radius + g_cfg.circle.thickness + g_cfg.circle.b.outline) * s) + 4;
        if (e > ext) ext = e;
    }
    if (g_cfg.bracket.b.enabled) {
        int e = (int)((g_cfg.bracket.size + g_cfg.bracket.length + g_cfg.bracket.thickness) * s) + 4;
        if (e > ext) ext = e;
    }
    if (g_cfg.image.b.enabled && g_img_w > 0) {
        int e = (int)(g_cfg.image.size * s / 2) + 4;
        if (e > ext) ext = e;
    }
    if (g_cfg.dot.b.enabled) {
        int e = (int)((g_cfg.dot.size / 2 + g_cfg.dot.b.outline + 1) * s) + 4;
        if (e > ext) ext = e;
    }
    if (ext < 6) ext = 6;
    if (ext > 400) ext = 400;
    return ext;
}

static void render(void) {
    if (g_cfg.image.b.enabled) load_png(g_cfg.image.path);

    int ext = layer_extent();
    int w = ext * 2, h = ext * 2;
    ensure_bitmap(w, h);
    ZeroMemory(g_bits, (size_t)w * h * 4);

    double s = g_cfg.scale;
    int cx = g_cx, cy = g_cy;

    if (g_cfg.cross.b.enabled) {
        LBASE *b = &g_cfg.cross.b;
        draw_cross_arms(cx, cy, (int)(g_cfg.cross.length * s),
                        (int)(g_cfg.cross.thickness * s + 0.5),
                        (int)(g_cfg.cross.gap * s), g_cfg.cross.arms,
                        b->color, b->alpha, b->outline, b->outline_color, b->outline_alpha);
    }
    if (g_cfg.diag.b.enabled) {
        LBASE *b = &g_cfg.diag.b;
        int L = (int)(g_cfg.diag.length * s), T = (int)(g_cfg.diag.thickness * s + 0.5);
        int G = (int)(g_cfg.diag.gap * s);
        double rad = g_cfg.diag.angle * M_PI / 180.0;
        double ca = cos(rad), sa = sin(rad);
        for (int q = 0; q < 4; q++) {
            if (!(g_cfg.diag.arms & (1 << q))) continue;
            double dx, dy;
            if (q == 0) { dx = 0; dy = -1; }
            else if (q == 1) { dx = 0; dy = 1; }
            else if (q == 2) { dx = -1; dy = 0; }
            else { dx = 1; dy = 0; }
            int x0 = cx + (int)(dx * G), y0 = cy + (int)(dy * G);
            int x1 = cx + (int)((dx * (G + L)) * ca - (dy * (G + L)) * sa);
            int y1 = cy + (int)((dx * (G + L)) * sa + (dy * (G + L)) * ca);
            if (b->outline) draw_line(x0, y0, x1, y1, T + b->outline * 2,
                                      with_alpha(b->outline_color, b->outline_alpha));
            draw_line(x0, y0, x1, y1, T, with_alpha(b->color, b->alpha));
        }
    }
    if (g_cfg.circle.b.enabled) {
        LBASE *b = &g_cfg.circle.b;
        int R = (int)(g_cfg.circle.radius * s), T = (int)(g_cfg.circle.thickness * s + 0.5);
        if (b->outline) fill_ring(cx, cy, R + b->outline, T + b->outline * 2,
                                  with_alpha(b->outline_color, b->outline_alpha));
        fill_ring(cx, cy, R, T, with_alpha(b->color, b->alpha));
    }
    if (g_cfg.bracket.b.enabled) {
        LBASE *b = &g_cfg.bracket.b;
        draw_bracket(cx, cy, (int)(g_cfg.bracket.size * s), (int)(g_cfg.bracket.length * s),
                     (int)(g_cfg.bracket.thickness * s + 0.5),
                     b->color, b->alpha, b->outline, b->outline_color, b->outline_alpha);
    }
    if (g_cfg.image.b.enabled)
        draw_image(cx, cy, (int)(g_cfg.image.size * s), g_cfg.image.angle, g_cfg.image.b.alpha);
    if (g_cfg.dot.b.enabled) {
        LBASE *b = &g_cfg.dot.b;
        int R = (int)(g_cfg.dot.size * s / 2 + 0.5);
        if (R < 1) R = 1;
        if (b->outline) fill_disc(cx, cy, R + b->outline, with_alpha(b->outline_color, b->outline_alpha));
        fill_disc(cx, cy, R, with_alpha(b->color, b->alpha));
    }

    // 整体旋转
    int rot = (int)g_cfg.rotation;
    if ((rot % 360) != 0) {
        double rad = rot * M_PI / 180.0;
        double ca = cos(rad), sa = sin(rad);
        unsigned char *cpy = (unsigned char *)malloc((size_t)w * h * 4);
        if (cpy) {
            memcpy(cpy, g_bits, (size_t)w * h * 4);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    double rx =  (x - cx) * ca + (y - cy) * sa;
                    double ry = -(x - cx) * sa + (y - cy) * ca;
                    int sx = (int)(cx + rx + 0.5), sy = (int)(cy + ry + 0.5);
                    unsigned char *d = (unsigned char *)g_bits + ((size_t)y * w + x) * 4;
                    if (sx < 0 || sy < 0 || sx >= w || sy >= h) { d[0]=d[1]=d[2]=d[3]=0; continue; }
                    unsigned char *sp = cpy + ((size_t)sy * w + sx) * 4;
                    d[0]=sp[0]; d[1]=sp[1]; d[2]=sp[2]; d[3]=sp[3];
                }
            free(cpy);
        }
    }

    // 定位
    RECT mr = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (g_cfg.monitor > 0) {
        DISPLAY_DEVICEA dd; ZeroMemory(&dd, sizeof(dd)); dd.cb = sizeof(dd);
        if (EnumDisplayDevicesA(NULL, g_cfg.monitor, &dd, 0)) {
            DEVMODEA dm; ZeroMemory(&dm, sizeof(dm)); dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsA(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
                mr.left = dm.dmPosition.x; mr.top = dm.dmPosition.y;
                mr.right = mr.left + (LONG)dm.dmPelsWidth;
                mr.bottom = mr.top + (LONG)dm.dmPelsHeight;
            }
        }
    }
    LONG mw = mr.right - mr.left, mh = mr.bottom - mr.top;
    int px = mr.left + (int)(mw * g_cfg.x_percent / 100.0) - w / 2 + g_cfg.offset_x + g_nudge_x;
    int py = mr.top + (int)(mh * g_cfg.y_percent / 100.0) - h / 2 + g_cfg.offset_y + g_nudge_y;

    POINT src = {0, 0};
    SIZE sz = {w, h};
    POINT dst = {px, py};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_wnd, NULL, &dst, &sz, g_memdc, &src, 0, &bf, ULW_ALPHA);
}

// ---------------- 热键 ----------------
static int key_name_to_vk(const char *name) {
    if (!name || !*name) return 0;
    if (name[0] == 'F' || name[0] == 'f') {
        int n = atoi(name + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (!_stricmp(name, "up")) return VK_UP;
    if (!_stricmp(name, "down")) return VK_DOWN;
    if (!_stricmp(name, "left")) return VK_LEFT;
    if (!_stricmp(name, "right")) return VK_RIGHT;
    if (!_stricmp(name, "esc")) return VK_ESCAPE;
    if (!_stricmp(name, "space")) return VK_SPACE;
    if (strlen(name) == 1) return toupper((unsigned char)name[0]);
    return 0;
}

static int parse_hotkey(const char *spec, UINT *mod, UINT *vk) {
    *mod = 0; *vk = 0;
    char buf[64];
    lstrcpynA(buf, spec, sizeof(buf));
    char *p = buf;
    while (p && *p) {
        char *plus = strchr(p, '+');
        char part[32];
        if (plus) { int n = (int)(plus - p); if (n > 31) n = 31; memcpy(part, p, n); part[n] = 0; p = plus + 1; }
        else { lstrcpynA(part, p, sizeof(part)); p = NULL; }
        char *t = trim(part);
        if (!_stricmp(t, "ctrl")) *mod |= MOD_CONTROL;
        else if (!_stricmp(t, "alt")) *mod |= MOD_ALT;
        else if (!_stricmp(t, "shift")) *mod |= MOD_SHIFT;
        else if (!_stricmp(t, "win")) *mod |= MOD_WIN;
        else *vk = (UINT)key_name_to_vk(t);
    }
    return *vk ? 1 : 0;
}

static void register_hotkeys(void) {
    for (int i = 1; i <= 9; i++) UnregisterHotKey(g_wnd, i);
    struct { const char *spec; int id; } list[] = {
        {g_cfg.toggle_key, 1}, {g_cfg.reload_key, 2}, {g_cfg.next_profile_key, 3},
        {g_cfg.quit_key, 4}, {g_cfg.nudge_up, 5}, {g_cfg.nudge_down, 6},
        {g_cfg.nudge_left, 7}, {g_cfg.nudge_right, 8}, {g_cfg.open_ui_key, 9}
    };
    for (int i = 0; i < (int)(sizeof(list)/sizeof(list[0])); i++) {
        UINT m, v;
        if (!parse_hotkey(list[i].spec, &m, &v)) continue;
        RegisterHotKey(g_wnd, list[i].id, m | MOD_NOREPEAT, v);
    }
}

// ---------------- 定时器 ----------------
static void mark_activity(void) { g_last_activity = GetTickCount(); }

static void schedule_trim(HWND h) {
    if (!g_cfg.trim_memory) { KillTimer(h, WM_APP_TRIM); return; }
    DWORD since = GetTickCount() - g_last_activity;
    UINT next = (since < (DWORD)g_cfg.trim_window_ms) ? (UINT)g_cfg.trim_active_ms : (UINT)g_cfg.trim_idle_ms;
    KillTimer(h, WM_APP_TRIM);
    if (next > 0) SetTimer(h, WM_APP_TRIM, next, NULL);
}

static void apply(void) {
    mark_activity();
    if (g_visible) {
        render();
        ShowWindow(g_wnd, SW_SHOWNA);
        if (g_cfg.keep_on_top)
            SetWindowPos(g_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        ShowWindow(g_wnd, SW_HIDE);
    }
    schedule_trim(g_wnd);
}

static void reload(void) {
    cfg_load(g_ini_path);
    register_hotkeys();
    SetWindowDisplayAffinity(g_wnd, g_cfg.hide_from_capture ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
    if (g_cfg.keep_on_top) SetTimer(g_wnd, WM_APP_TOP, (UINT)g_cfg.topmost_check_ms, NULL);
    else KillTimer(g_wnd, WM_APP_TOP);
    if (g_cfg.auto_reload) SetTimer(g_wnd, WM_APP_WATCH, 2000, NULL);
    else KillTimer(g_wnd, WM_APP_WATCH);
    apply();
}

static void exe_dir(char *out, DWORD cap) {
    GetModuleFileNameA(NULL, out, cap);
    char *p = strrchr(out, '\\');
    if (p) *p = 0;
}

// 读 profiles 列表并切换
static void next_profile(void) {
    DWORD len = 0;
    char *src = read_file(g_ini_path, &len);
    if (!src) return;
    char section[64] = "", list[512] = "";
    char *p = src;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        char line[512];
        int ll = nl ? (int)(nl - p) : (int)strlen(p);
        if (ll > 511) ll = 511;
        memcpy(line, p, ll); line[ll] = 0;
        p = nl ? nl + 1 : NULL;
        char *s = trim(line); strip_comment(s); s = trim(s);
        if (!*s) continue;
        if (*s == '[') { char *e = strchr(s, ']'); if (e) { *e = 0; lstrcpynA(section, s + 1, sizeof(section)); } continue; }
        char *eq = strchr(s, '='); if (!eq) continue;
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        if (!_stricmp(section, "general") && key_is(k, "profiles")) lstrcpynA(list, v, sizeof(list));
    }
    free(src);
    if (!list[0]) return;

    char items[16][64];
    int n = 0;
    char *tok = list, *nxt;
    while (tok && *tok && n < 16) {
        nxt = strchr(tok, ',');
        if (nxt) *nxt = 0;
        char *t = trim(tok);
        if (*t) { lstrcpynA(items[n], t, 64); n++; }
        tok = nxt ? nxt + 1 : NULL;
    }
    if (n == 0) return;
    int idx = -1;
    for (int i = 0; i < n; i++) if (!_stricmp(items[i], g_cfg.profile)) { idx = i; break; }
    int next = (idx + 1) % n;
    lstrcpynA(g_profile_override, items[next], sizeof(g_profile_override));
    cfg_load(g_ini_path);
    register_hotkeys();
    apply();
}

// ================= 配置面板（原生 Win32，零依赖） =================
#define PANEL_CLASS "MiniCrossPanel"
#define ID_GROUP 2001
#define ID_SAVE  2002
#define ID_RESET 2003
#define ID_CLOSE 2004
#define ID_CTL   2100

typedef struct { const char *label; int *pval; int lo, hi; char kind; } PARAM;
typedef struct { const char *name; PARAM items[10]; int n; } PGROUP;

static PGROUP g_groups[8];
static int    g_ngroups = 0;
static HWND   g_panel, g_group_cb;
static HWND   g_ctl[10], g_lbl[10], g_val[10];
static int    g_cur_group = 0;
static HINSTANCE g_hi;

#define ADDP(g, lbl, fld, mn, mx, kd) do { \
    (g)->items[(g)->n].label = (lbl); (g)->items[(g)->n].pval = &(fld); \
    (g)->items[(g)->n].lo = (mn); (g)->items[(g)->n].hi = (mx); \
    (g)->items[(g)->n].kind = (kd); (g)->n++; } while (0)

static void init_groups(void) {
    if (g_ngroups) return;
    int i = 0;
    g_groups[i].name = "Cross"; g_groups[i].n = 0;
    ADDP(&g_groups[i], "Enabled",  g_cfg.cross.b.enabled, 0, 1, 'b');
    ADDP(&g_groups[i], "Length",   g_cfg.cross.length, 0, 60, 'i');
    ADDP(&g_groups[i], "Thickness",g_cfg.cross.thickness, 1, 10, 'i');
    ADDP(&g_groups[i], "Gap",      g_cfg.cross.gap, 0, 40, 'i');
    ADDP(&g_groups[i], "Color",    g_cfg.cross.b.color, 0, 0, 'c');
    ADDP(&g_groups[i], "Alpha",    g_cfg.cross.b.alpha, 0, 255, 'i');
    ADDP(&g_groups[i], "Outline",  g_cfg.cross.b.outline, 0, 4, 'i');
    ADDP(&g_groups[i], "OL Color", g_cfg.cross.b.outline_color, 0, 0, 'c');
    i++;
    g_groups[i].name = "Diagonal"; g_groups[i].n = 0;
    ADDP(&g_groups[i], "Enabled",  g_cfg.diag.b.enabled, 0, 1, 'b');
    ADDP(&g_groups[i], "Length",   g_cfg.diag.length, 0, 60, 'i');
    ADDP(&g_groups[i], "Thickness",g_cfg.diag.thickness, 1, 10, 'i');
    ADDP(&g_groups[i], "Gap",      g_cfg.diag.gap, 0, 40, 'i');
    ADDP(&g_groups[i], "Angle",    g_cfg.diag.angle, 0, 90, 'i');
    ADDP(&g_groups[i], "Color",    g_cfg.diag.b.color, 0, 0, 'c');
    ADDP(&g_groups[i], "Alpha",    g_cfg.diag.b.alpha, 0, 255, 'i');
    i++;
    g_groups[i].name = "Dot"; g_groups[i].n = 0;
    ADDP(&g_groups[i], "Enabled",  g_cfg.dot.b.enabled, 0, 1, 'b');
    ADDP(&g_groups[i], "Diameter", g_cfg.dot.size, 0, 20, 'i');
    ADDP(&g_groups[i], "Color",    g_cfg.dot.b.color, 0, 0, 'c');
    ADDP(&g_groups[i], "Alpha",    g_cfg.dot.b.alpha, 0, 255, 'i');
    ADDP(&g_groups[i], "Outline",  g_cfg.dot.b.outline, 0, 4, 'i');
    i++;
    g_groups[i].name = "Circle"; g_groups[i].n = 0;
    ADDP(&g_groups[i], "Enabled",  g_cfg.circle.b.enabled, 0, 1, 'b');
    ADDP(&g_groups[i], "Radius",   g_cfg.circle.radius, 0, 120, 'i');
    ADDP(&g_groups[i], "Thickness",g_cfg.circle.thickness, 1, 10, 'i');
    ADDP(&g_groups[i], "Color",    g_cfg.circle.b.color, 0, 0, 'c');
    ADDP(&g_groups[i], "Alpha",    g_cfg.circle.b.alpha, 0, 255, 'i');
    i++;
    g_groups[i].name = "Bracket"; g_groups[i].n = 0;
    ADDP(&g_groups[i], "Enabled",  g_cfg.bracket.b.enabled, 0, 1, 'b');
    ADDP(&g_groups[i], "Distance", g_cfg.bracket.size, 0, 120, 'i');
    ADDP(&g_groups[i], "Arm",      g_cfg.bracket.length, 0, 60, 'i');
    ADDP(&g_groups[i], "Thickness",g_cfg.bracket.thickness, 1, 10, 'i');
    ADDP(&g_groups[i], "Color",    g_cfg.bracket.b.color, 0, 0, 'c');
    ADDP(&g_groups[i], "Alpha",    g_cfg.bracket.b.alpha, 0, 255, 'i');
    i++;
    g_groups[i].name = "Image"; g_groups[i].n = 0;
    ADDP(&g_groups[i], "Enabled",  g_cfg.image.b.enabled, 0, 1, 'b');
    ADDP(&g_groups[i], "Size",     g_cfg.image.size, 1, 256, 'i');
    ADDP(&g_groups[i], "Angle",    g_cfg.image.angle, -180, 180, 'i');
    ADDP(&g_groups[i], "Alpha",    g_cfg.image.b.alpha, 0, 255, 'i');
    i++;
    g_groups[i].name = "General"; g_groups[i].n = 0;
    ADDP(&g_groups[i], "Offset X", g_cfg.offset_x, -400, 400, 'i');
    ADDP(&g_groups[i], "Offset Y", g_cfg.offset_y, -400, 400, 'i');
    ADDP(&g_groups[i], "Hide in cap", g_cfg.hide_from_capture, 0, 1, 'b');
    ADDP(&g_groups[i], "Keep on top", g_cfg.keep_on_top, 0, 1, 'b');
    i++;
    g_ngroups = i;
}

static void pick_color(PARAM *p) {
    CHOOSECOLORA cc;
    ZeroMemory(&cc, sizeof(cc));
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = g_panel;
    cc.rgbResult = RGB((*p->pval >> 16) & 255, (*p->pval >> 8) & 255, *p->pval & 255);
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorA(&cc)) {
        int a = (*p->pval >> 24) & 255;
        if (a == 0) a = 255;
        *p->pval = (a << 24) | (GetRValue(cc.rgbResult) << 16) |
                   (GetGValue(cc.rgbResult) << 8) | GetBValue(cc.rgbResult);
    }
}

static void save_ini(void) {
    char bak[MAX_PATH];
    lstrcpynA(bak, g_ini_path, MAX_PATH);
    lstrcatA(bak, ".bak");
    CopyFileA(g_ini_path, bak, FALSE);

    DWORD len = 0;
    char *src = read_file(g_ini_path, &len);
    char *prof = NULL;
    if (src) {
        char *p = src;
        while (p && *p) {
            if ((p == src || p[-1] == '\n') && !_strnicmp(p, "[profile:", 9)) { prof = p; break; }
            p = strchr(p, '\n');
            if (p) p++;
        }
    }

    FILE *f = fopen(g_ini_path, "wb");
    if (!f) { if (src) free(src); return; }

    fprintf(f, "; saved by MiniCross panel, backup at config.ini.bak\r\n\r\n");
    fprintf(f, "[general]\r\n");
    fprintf(f, "profile = %s\r\n", g_cfg.profile);
    fprintf(f, "monitor = %d\r\n", g_cfg.monitor);
    fprintf(f, "x_percent = %.2f\r\n", g_cfg.x_percent);
    fprintf(f, "y_percent = %.2f\r\n", g_cfg.y_percent);
    fprintf(f, "offset_x = %d\r\n", g_cfg.offset_x);
    fprintf(f, "offset_y = %d\r\n", g_cfg.offset_y);
    fprintf(f, "scale = %.2f\r\n", g_cfg.scale);
    fprintf(f, "rotation = %.1f\r\n", g_cfg.rotation);
    fprintf(f, "nudge_step = %d\r\n", g_cfg.nudge_step);
    fprintf(f, "hide_from_capture = %d\r\n", g_cfg.hide_from_capture);
    fprintf(f, "antialias = %d\r\n", g_cfg.antialias);
    fprintf(f, "auto_reload = %d\r\n", g_cfg.auto_reload);
    fprintf(f, "keep_on_top = %d\r\n", g_cfg.keep_on_top);
    fprintf(f, "topmost_check_ms = %d\r\n", g_cfg.topmost_check_ms);
    fprintf(f, "trim_memory = %d\r\n", g_cfg.trim_memory);
    fprintf(f, "trim_active_ms = %d\r\n", g_cfg.trim_active_ms);
    fprintf(f, "trim_window_ms = %d\r\n", g_cfg.trim_window_ms);
    fprintf(f, "trim_idle_ms = %d\r\n", g_cfg.trim_idle_ms);
    fprintf(f, "\r\n[hotkeys]\r\n");
    fprintf(f, "toggle = %s\r\n", g_cfg.toggle_key);
    fprintf(f, "open_ui = %s\r\n", g_cfg.open_ui_key);
    fprintf(f, "reload = %s\r\n", g_cfg.reload_key);
    fprintf(f, "next_profile = %s\r\n", g_cfg.next_profile_key);
    fprintf(f, "quit = %s\r\n", g_cfg.quit_key);

#define SAVE_LB(sec, st) \
    fprintf(f, "\r\n[%s]\r\n", sec); \
    fprintf(f, "enabled = %d\r\ncolor = #%06X\r\nalpha = %d\r\noutline = %d\r\n" \
               "outline_color = #%06X\r\noutline_alpha = %d\r\n", \
            st.b.enabled, st.b.color & 0xFFFFFF, st.b.alpha, st.b.outline, \
            st.b.outline_color & 0xFFFFFF, st.b.outline_alpha)

    SAVE_LB("cross", g_cfg.cross);
    fprintf(f, "length = %d\r\nthickness = %d\r\ngap = %d\r\narms = %d%d%d%d\r\n",
            g_cfg.cross.length, g_cfg.cross.thickness, g_cfg.cross.gap,
            !!(g_cfg.cross.arms & 1), !!(g_cfg.cross.arms & 2),
            !!(g_cfg.cross.arms & 4), !!(g_cfg.cross.arms & 8));

    SAVE_LB("diagonal", g_cfg.diag);
    fprintf(f, "length = %d\r\nthickness = %d\r\ngap = %d\r\nangle = %d\r\n",
            g_cfg.diag.length, g_cfg.diag.thickness, g_cfg.diag.gap, g_cfg.diag.angle);

    SAVE_LB("dot", g_cfg.dot);
    fprintf(f, "size = %d\r\n", g_cfg.dot.size);

    SAVE_LB("circle", g_cfg.circle);
    fprintf(f, "radius = %d\r\nthickness = %d\r\n", g_cfg.circle.radius, g_cfg.circle.thickness);

    SAVE_LB("bracket", g_cfg.bracket);
    fprintf(f, "size = %d\r\nlength = %d\r\nthickness = %d\r\n",
            g_cfg.bracket.size, g_cfg.bracket.length, g_cfg.bracket.thickness);

    fprintf(f, "\r\n[image]\r\nenabled = %d\r\npath = %s\r\nsize = %d\r\nangle = %d\r\nalpha = %d\r\n",
            g_cfg.image.b.enabled, g_cfg.image.path, g_cfg.image.size,
            g_cfg.image.angle, g_cfg.image.b.alpha);

    if (prof) { fprintf(f, "\r\n%s", prof); }
    fclose(f);
    if (src) free(src);
}

static void panel_build_controls(void);
static LRESULT CALLBACK panel_proc(HWND h, UINT m, WPARAM w, LPARAM l);

static void panel_build_controls(void) {
    for (int i = 0; i < 10; i++) {
        if (g_ctl[i]) { DestroyWindow(g_ctl[i]); g_ctl[i] = NULL; }
        if (g_lbl[i]) { DestroyWindow(g_lbl[i]); g_lbl[i] = NULL; }
        if (g_val[i]) { DestroyWindow(g_val[i]); g_val[i] = NULL; }
    }
    PGROUP *g = &g_groups[g_cur_group];
    for (int i = 0; i < g->n; i++) {
        PARAM *p = &g->items[i];
        int y = 52 + i * 40;
        g_lbl[i] = CreateWindowExA(0, "STATIC", p->label, WS_CHILD | WS_VISIBLE,
                                   16, y + 6, 110, 20, g_panel, NULL, g_hi, NULL);
        if (p->kind == 'b') {
            g_ctl[i] = CreateWindowExA(0, "BUTTON", "", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                       134, y, 60, 28, g_panel, (HMENU)(INT_PTR)(ID_CTL + i), g_hi, NULL);
            SendMessageA(g_ctl[i], BM_SETCHECK, *p->pval ? BST_CHECKED : BST_UNCHECKED, 0);
        } else if (p->kind == 'c') {
            g_ctl[i] = CreateWindowExA(0, "BUTTON", "", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       134, y, 120, 28, g_panel, (HMENU)(INT_PTR)(ID_CTL + i), g_hi, NULL);
            char buf[16]; wsprintfA(buf, "#%06X", (*p->pval) & 0xFFFFFF);
            SetWindowTextA(g_ctl[i], buf);
        } else {
            g_ctl[i] = CreateWindowExA(0, TRACKBAR_CLASS, "", WS_CHILD | WS_VISIBLE | TBS_HORZ,
                                       134, y + 4, 160, 24, g_panel, (HMENU)(INT_PTR)(ID_CTL + i), g_hi, NULL);
            SendMessageA(g_ctl[i], TBM_SETRANGE, TRUE, MAKELPARAM((short)p->lo, (short)p->hi));
            SendMessageA(g_ctl[i], TBM_SETPOS, TRUE, *p->pval);
            char buf[32]; wsprintfA(buf, "%d", *p->pval);
            g_val[i] = CreateWindowExA(0, "STATIC", buf, WS_CHILD | WS_VISIBLE,
                                       300, y + 6, 50, 20, g_panel, NULL, g_hi, NULL);
        }
    }
    HFONT fnt = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    for (int i = 0; i < g->n; i++) {
        if (g_lbl[i]) SendMessageA(g_lbl[i], WM_SETFONT, (WPARAM)fnt, TRUE);
        if (g_ctl[i]) SendMessageA(g_ctl[i], WM_SETFONT, (WPARAM)fnt, TRUE);
        if (g_val[i]) SendMessageA(g_val[i], WM_SETFONT, (WPARAM)fnt, TRUE);
    }
}

static LRESULT CALLBACK panel_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == ID_GROUP && HIWORD(w) == CBN_SELCHANGE) {
            g_cur_group = (int)SendMessageA(g_group_cb, CB_GETCURSEL, 0, 0);
            panel_build_controls();
            InvalidateRect(h, NULL, FALSE);
        } else if (id == ID_SAVE) {
            save_ini();
            MessageBoxA(h, "Saved. The overlay reloads within 2 seconds.", "MiniCross",
                        MB_OK | MB_ICONINFORMATION);
        } else if (id == ID_RESET) {
            cfg_load(g_ini_path);
            panel_build_controls();
            render();
            InvalidateRect(h, NULL, FALSE);
        } else if (id == ID_CLOSE) {
            ShowWindow(h, SW_HIDE);
        } else if (id >= ID_CTL && id < ID_CTL + 10) {
            int idx = id - ID_CTL;
            if (idx < g_groups[g_cur_group].n) {
                PARAM *p = &g_groups[g_cur_group].items[idx];
                if (p->kind == 'b') {
                    *p->pval = SendMessageA(g_ctl[idx], BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
                    render(); InvalidateRect(h, NULL, FALSE);
                } else if (p->kind == 'c') {
                    pick_color(p);
                    char buf[16]; wsprintfA(buf, "#%06X", (*p->pval) & 0xFFFFFF);
                    SetWindowTextA(g_ctl[idx], buf);
                    render(); InvalidateRect(h, NULL, FALSE);
                }
            }
        }
        return 0;
    }
    case WM_HSCROLL: {
        HWND c = (HWND)l;
        int id = GetDlgCtrlID(c) - ID_CTL;
        if (id >= 0 && id < g_groups[g_cur_group].n) {
            PARAM *p = &g_groups[g_cur_group].items[id];
            int v = (int)SendMessageA(c, TBM_GETPOS, 0, 0);
            *p->pval = v;
            if (g_val[id]) { char buf[32]; wsprintfA(buf, "%d", v); SetWindowTextA(g_val[id], buf); }
            render();
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT cr; GetClientRect(h, &cr);
        HBRUSH bg = CreateSolidBrush(RGB(28, 30, 34));
        FillRect(dc, &cr, bg);
        DeleteObject(bg);
        RECT r = {400, 52, cr.right - 12, cr.bottom - 130};
        if (r.right < 500) r.right = 500;
        HBRUSH br = CreateSolidBrush(RGB(18, 20, 24));
        FillRect(dc, &r, br);
        DeleteObject(br);
        if (g_bits && g_w > 0 && g_memdc) {
            int maxw = 330, maxh = 330;
            double sc = (double)maxw / (g_w > g_h ? g_w : g_h);
            int dw = (int)(g_w * sc), dh = (int)(g_h * sc);
            if (dw < 4) dw = 4;
            if (dh < 4) dh = 4;
            int dx = r.left + (r.right - r.left - dw) / 2;
            int dy = r.top + (r.bottom - r.top - dh) / 2;
            BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            SetStretchBltMode(dc, HALFTONE);
            AlphaBlend(dc, dx, dy, dw, dh, g_memdc, 0, 0, g_w, g_h, bf);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcA(h, m, w, l);
}

static void panel_create(void) {
    init_groups();
    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = panel_proc;
    wc.hInstance = g_hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(28, 30, 34));
    wc.lpszClassName = PANEL_CLASS;
    RegisterClassA(&wc);

    g_panel = CreateWindowExA(WS_EX_TOOLWINDOW, PANEL_CLASS, "MiniCross Settings",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT, 780, 620, NULL, NULL, g_hi, NULL);

    g_group_cb = CreateWindowExA(0, "COMBOBOX", "",
                                 WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                 16, 12, 260, 240, g_panel, (HMENU)ID_GROUP, g_hi, NULL);
    for (int i = 0; i < g_ngroups; i++)
        SendMessageA(g_group_cb, CB_ADDSTRING, 0, (LPARAM)g_groups[i].name);
    SendMessageA(g_group_cb, CB_SETCURSEL, 0, 0);

    CreateWindowExA(0, "BUTTON", "Save",   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    16, 520, 90, 32, g_panel, (HMENU)ID_SAVE, g_hi, NULL);
    CreateWindowExA(0, "BUTTON", "Reset",  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    116, 520, 90, 32, g_panel, (HMENU)ID_RESET, g_hi, NULL);
    CreateWindowExA(0, "BUTTON", "Close",  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    216, 520, 90, 32, g_panel, (HMENU)ID_CLOSE, g_hi, NULL);

    g_cur_group = 0;
}

static void toggle_panel(void) {
    if (!g_panel) panel_create();
    if (IsWindowVisible(g_panel)) { ShowWindow(g_panel, SW_HIDE); return; }
    render();
    panel_build_controls();
    ShowWindow(g_panel, SW_SHOW);
    SetForegroundWindow(g_panel);
    InvalidateRect(g_panel, NULL, FALSE);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_HOTKEY:
        switch (w) {
        case 1: g_visible = !g_visible; apply(); break;
        case 2: reload(); break;
        case 3: next_profile(); break;
        case 4: PostQuitMessage(0); break;
        case 5: g_nudge_y -= g_cfg.nudge_step; apply(); break;
        case 6: g_nudge_y += g_cfg.nudge_step; apply(); break;
        case 7: g_nudge_x -= g_cfg.nudge_step; apply(); break;
        case 8: g_nudge_x += g_cfg.nudge_step; apply(); break;
        case 9: toggle_panel(); break;
        }
        return 0;
    case WM_TIMER:
        if (w == WM_APP_TRIM) {
            KillTimer(h, WM_APP_TRIM);
            SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
            schedule_trim(h);
        } else if (w == WM_APP_TOP) {
            if (g_cfg.keep_on_top && g_visible)
                SetWindowPos(g_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        } else if (w == WM_APP_WATCH) {
            static FILETIME last = {0, 0};
            WIN32_FILE_ATTRIBUTE_DATA fa;
            if (GetFileAttributesExA(g_ini_path, GetFileExInfoStandard, &fa)) {
                if (last.dwLowDateTime || last.dwHighDateTime) {
                    if (CompareFileTime(&fa.ftLastWriteTime, &last) != 0) {
                        last = fa.ftLastWriteTime;
                        reload();
                    }
                } else last = fa.ftLastWriteTime;
            }
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show) {
    g_hi = hi;
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    exe_dir(g_ini_path, MAX_PATH);
    lstrcatA(g_ini_path, "\\config.ini");
    cfg_load(g_ini_path);

    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hi;
    wc.lpszClassName = "MiniCrossCore";
    RegisterClassA(&wc);

    g_wnd = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        "MiniCrossCore", "", WS_POPUP, 0, 0, 8, 8, NULL, NULL, hi, NULL);
    if (!g_wnd) return 1;

    register_hotkeys();
    SetWindowDisplayAffinity(g_wnd, g_cfg.hide_from_capture ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
    SetTimer(g_wnd, WM_APP_TOP, (UINT)g_cfg.topmost_check_ms, NULL);
    if (g_cfg.auto_reload) SetTimer(g_wnd, WM_APP_WATCH, 2000, NULL);
    apply();

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}
