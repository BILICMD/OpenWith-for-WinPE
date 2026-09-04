//允许学习、修改、编译、整合和二次发布。二改、整合或转载时，必须保留原作者并注明“基于本项目修改”。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

#ifndef WM_CAPTURECHANGED
#define WM_CAPTURECHANGED 0x0215
#endif
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef WM_TIMER
#define WM_TIMER 0x0113
#endif
#ifndef SWP_NOZORDER
#define SWP_NOZORDER 0x0004
#endif
#ifndef SWP_NOACTIVATE
#define SWP_NOACTIVATE 0x0010
#endif
#ifndef WS_EX_TOOLWINDOW
#define WS_EX_TOOLWINDOW 0x00000080L
#endif

#define APP_MAX 128
#define PATH_CAP 1024
#define BASE_WIN_W 396
#define BASE_WIN_H 406
#define BASE_SHADOW_PAD 32
#define BASE_SHADOW_Y 4
#define BASE_BODY_RADIUS 16
#define BASE_BORDER_WIDTH 1
#define AA_SS 4
#define BASE_ROW_H 52
#define BASE_LIST_TOP 74
#define BASE_LIST_BOTTOM 282
#define IDI_PLACEHOLDER 201
#define IDI_SCRIPT 202

/* 所有布局数值都以 96 DPI 为基准，运行时统一换算成当前 DPI。 */
static UINT g_dpi = 96;
static int g_shadowEnabled = 1;
static int dpi_px(int value) {
    /* 布局值和 DPI 都很小，32 位整数足够。避免 x86 每次换算都调用软件 64 位除法。 */
    return (value * (int)g_dpi + 48) / 96;
}
#define PX(v) dpi_px(v)
#define WIN_W PX(BASE_WIN_W)
#define WIN_H PX(BASE_WIN_H)
#define SHADOW_PAD (g_shadowEnabled ? PX(BASE_SHADOW_PAD) : 0)
#define SHADOW_Y (g_shadowEnabled ? PX(BASE_SHADOW_Y) : 0)
#define BODY_RADIUS PX(BASE_BODY_RADIUS)
#define BORDER_WIDTH PX(BASE_BORDER_WIDTH)
#define LAYER_W (WIN_W + SHADOW_PAD * 2)
#define LAYER_H (WIN_H + SHADOW_PAD * 2)
#define ROW_H PX(BASE_ROW_H)
#define LIST_TOP PX(BASE_LIST_TOP)
#define LIST_BOTTOM PX(BASE_LIST_BOTTOM)

typedef struct {
    wchar_t path[PATH_CAP];
    wchar_t name[256];
    HICON icon;
} APP_ITEM;

typedef struct {
    COLORREF background, surface, hover, selected, text, secondary;
    COLORREF border, link, accent, button, buttonHover, buttonPressed, disabled;
} PALETTE;

enum HIT_TARGET {
    HIT_NONE = -1, HIT_BROWSE = 1002,
    HIT_ALWAYS = 1003, HIT_ONCE = 1004
};

static HWND g_hwnd;
static APP_ITEM g_apps[APP_MAX];
static int g_count, g_selected = -1, g_hover = HIT_NONE, g_pressed = HIT_NONE;
static int g_scroll, g_scrollTarget, g_wheelRemainder, g_dark;
static int g_scrollDragging, g_scrollDragOffset;
static DWORD g_osBuild;
static int g_useRounded;
#define SCROLL_TIMER_ID 0x4F57
static wchar_t g_file[PATH_CAP], g_extension[64];
static int g_allowDeactivate;
static HFONT g_titleFont, g_textFont, g_linkFont;
static PALETTE g_pal;

static void recreate_fonts(void);
static void resize_for_dpi(HWND hwnd, const RECT *suggested);
void *memset(void *dst, int value, size_t count);

/*
 * WinPE 里不要用 GetVersionEx 判断系统代际，它可能受兼容性清单影响。
 * RtlGetVersion 返回内核真实 Build：22000 起按 Win11 风格使用圆角，
 * 低于 22000（包括 Win10 各版本）统一使用直角。
 */
typedef struct _OW_RTL_OSVERSIONINFOW {
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    wchar_t szCSDVersion[128];
} OW_RTL_OSVERSIONINFOW;

static DWORD query_os_build(void) {
    typedef LONG (WINAPI *PFN_RTLGETVERSION)(OW_RTL_OSVERSIONINFOW *);
    DWORD build = 0;
    HMODULE ntdll = LoadLibraryW(L"ntdll.dll");
    if (ntdll) {
        PFN_RTLGETVERSION fn = (PFN_RTLGETVERSION)GetProcAddress(ntdll, "RtlGetVersion");
        if (fn) {
            OW_RTL_OSVERSIONINFOW vi;
            ZeroMemory(&vi, sizeof(vi));
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (fn(&vi) == 0) build = vi.dwBuildNumber;
        }
        FreeLibrary(ntdll);
    }
    return build;
}


/*
 * WinPE 里的 msvcrt.dll 不一定完整，部分精简环境连 wmemcpy 都没有。
 * 这里补一小套实际用到的运行库函数，避免 EXE 依赖 MSVCRT/UCRT。
 */
void *memset(void *dst, int value, size_t count) {
    unsigned char *p = (unsigned char *)dst;
    while (count--) *p++ = (unsigned char)value;
    return dst;
}

void *memmove(void *dst, const void *src, size_t count) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || count == 0) return dst;
    if (d < s) {
        while (count--) *d++ = *s++;
    } else {
        d += count; s += count;
        while (count--) *--d = *--s;
    }
    return dst;
}

wchar_t *wmemcpy(wchar_t *dst, const wchar_t *src, size_t count) {
    for (size_t i = 0; i < count; ++i) dst[i] = src[i];
    return dst;
}

size_t wcslen(const wchar_t *s) {
    const wchar_t *p = s;
    while (*p) ++p;
    return (size_t)(p - s);
}

wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t count) {
    size_t i = 0;
    for (; i < count && src[i]; ++i) dst[i] = src[i];
    for (; i < count; ++i) dst[i] = 0;
    return dst;
}

wchar_t *wcschr(const wchar_t *s, wchar_t ch) {
    for (;; ++s) {
        if (*s == ch) return (wchar_t *)s;
        if (!*s) return NULL;
    }
}

wchar_t *wcsrchr(const wchar_t *s, wchar_t ch) {
    const wchar_t *last = NULL;
    do {
        if (*s == ch) last = s;
    } while (*s++);
    return (wchar_t *)last;
}

static wchar_t ow_lower(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return c + (L'a' - L'A');
    return c;
}

int _wcsicmp(const wchar_t *a, const wchar_t *b) {
    while (*a && *b) {
        wchar_t ca = ow_lower(*a++), cb = ow_lower(*b++);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (*a == *b) return 0;
    return *a ? 1 : -1;
}

int _wcsnicmp(const wchar_t *a, const wchar_t *b, size_t count) {
    while (count--) {
        wchar_t ca = ow_lower(*a++), cb = ow_lower(*b++);
        if (ca != cb) return ca < cb ? -1 : 1;
        if (!ca) return 0;
    }
    return 0;
}

static int ow_putc(wchar_t *dst, size_t cap, size_t *pos, wchar_t c) {
    if (*pos + 1 < cap) dst[*pos] = c;
    ++*pos;
    return 1;
}

static void ow_puts(wchar_t *dst, size_t cap, size_t *pos, const wchar_t *s) {
    if (!s) s = L"";
    while (*s) ow_putc(dst, cap, pos, *s++);
}

static void ow_hex4(wchar_t *dst, size_t cap, size_t *pos, unsigned int v) {
    static const wchar_t hex[] = L"0123456789abcdef";
    for (int shift = 12; shift >= 0; shift -= 4)
        ow_putc(dst, cap, pos, hex[(v >> shift) & 0xF]);
}

int _snwprintf(wchar_t *dst, size_t cap, const wchar_t *fmt, ...) {
    __builtin_va_list ap;
    size_t pos = 0;
    if (!dst || cap == 0) return -1;
    __builtin_va_start(ap, fmt);
    while (*fmt) {
        if (*fmt != L'%') {
            ow_putc(dst, cap, &pos, *fmt++);
            continue;
        }
        ++fmt;
        if (*fmt == L'%') {
            ow_putc(dst, cap, &pos, L'%'); ++fmt;
        } else if (*fmt == L's') {
            const wchar_t *s = __builtin_va_arg(ap, const wchar_t *);
            ow_puts(dst, cap, &pos, s); ++fmt;
        } else if (fmt[0] == L'0' && fmt[1] == L'4' && fmt[2] == L'x') {
            unsigned int v = __builtin_va_arg(ap, unsigned int);
            ow_hex4(dst, cap, &pos, v); fmt += 3;
        } else {
            ow_putc(dst, cap, &pos, L'%');
            if (*fmt) ow_putc(dst, cap, &pos, *fmt++);
        }
    }
    __builtin_va_end(ap);
    if (cap) dst[(pos < cap) ? pos : (cap - 1)] = 0;
    return (int)pos;
}

static void safe_copy(wchar_t *dst, size_t cap, const wchar_t *src) {
    if (!src) src = L"";
    wcsncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

/* ============================== 主题与基础绘制 ============================== */

static void set_palette(void) {
    if (g_dark) {
        g_pal.background = RGB(45,45,45);
        g_pal.surface = RGB(45,45,45);
        g_pal.hover = RGB(57,57,57);
        g_pal.selected = RGB(62,62,62);
        g_pal.text = RGB(255,255,255);
        g_pal.secondary = RGB(205,205,205);
        g_pal.border = RGB(73,73,73);
        g_pal.link = RGB(96,205,255);
        g_pal.accent = RGB(96,205,255);
        g_pal.button = RGB(56,56,56);
        g_pal.buttonHover = RGB(66,66,66);
        g_pal.buttonPressed = RGB(74,74,74);
        g_pal.disabled = RGB(135,135,135);
    } else {
        g_pal.background = RGB(249,249,249);
        g_pal.surface = RGB(249,249,249);
        g_pal.hover = RGB(243,243,243);
        g_pal.selected = RGB(238,238,238);
        g_pal.text = RGB(26,26,26);
        g_pal.secondary = RGB(92,92,92);
        g_pal.border = RGB(229,229,229);
        g_pal.link = RGB(0,95,184);
        g_pal.accent = RGB(0,103,192);
        g_pal.button = RGB(251,251,251);
        g_pal.buttonHover = RGB(244,244,244);
        g_pal.buttonPressed = RGB(235,235,235);
        g_pal.disabled = RGB(150,150,150);
    }
}

/* 对应“高级系统设置 → 性能 → 在窗口下显示阴影”。查询失败时保持旧版行为。 */
static int query_window_shadow_enabled(void) {
    BOOL enabled = TRUE;
    if (SystemParametersInfoW(SPI_GETDROPSHADOW, 0, &enabled, 0))
        return enabled ? 1 : 0;
    return 1;
}

static void refresh_window_shadow(HWND hwnd) {
    int enabled = query_window_shadow_enabled();
    if (enabled == g_shadowEnabled) return;

    RECT wr;
    if (!GetWindowRect(hwnd, &wr)) {
        g_shadowEnabled = enabled;
        return;
    }
    int oldPad = g_shadowEnabled ? PX(BASE_SHADOW_PAD) : 0;
    int bodyLeft = wr.left + oldPad;
    int bodyTop = wr.top + oldPad;
    g_shadowEnabled = enabled;
    int newPad = g_shadowEnabled ? PX(BASE_SHADOW_PAD) : 0;
    SetWindowPos(hwnd, NULL, bodyLeft - newPad, bodyTop - newPad,
                 LAYER_W, LAYER_H, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void apply_theme(HWND hwnd) {
    DWORD light = 1, bytes = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &bytes);
    g_dark = light == 0;
    set_palette();

    /* 圆角和阴影由分层窗口自己画。WinPE 里 DWM 可能不存在，这里不依赖它做裁剪。 */
    BOOL dark = g_dark ? TRUE : FALSE;
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm) {
        typedef HRESULT (WINAPI *DWMSET)(HWND, DWORD, LPCVOID, DWORD);
        DWMSET set = (DWMSET)GetProcAddress(dwm, "DwmSetWindowAttribute");
        if (set) set(hwnd, 20, &dark, sizeof(dark));
        FreeLibrary(dwm);
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

static void fill_rect(HDC dc, const RECT *r, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, r, brush);
    DeleteObject(brush);
}

static void round_fill(HDC dc, const RECT *r, int radius, COLORREF fill, COLORREF border) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, r->left, r->top, r->right, r->bottom, radius * 2, radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

static void draw_text(HDC dc, HFONT font, COLORREF color, const wchar_t *text,
                      RECT r, UINT format) {
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &r, format | DT_NOPREFIX);
    SelectObject(dc, old);
}

/* ============================== 应用与文件信息 ============================== */

static void basename_no_ext(const wchar_t *path, wchar_t *out, size_t cap) {
    const wchar_t *base = path;
    const wchar_t *p;
    for (p = path; *p; ++p) if (*p == L'\\' || *p == L'/') base = p + 1;
    safe_copy(out, cap, base);
    p = wcsrchr(out, L'.');
    if (p) out[p - out] = 0;
}

static void basename_file(const wchar_t *path, wchar_t *out, size_t cap) {
    const wchar_t *base = path;
    for (const wchar_t *p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') base = p + 1;
    safe_copy(out, cap, base);
}

static void get_file_description(const wchar_t *path, wchar_t *out, size_t cap) {
    DWORD ignored = 0;
    DWORD size = GetFileVersionInfoSizeW(path, &ignored);
    if (!size) { basename_no_ext(path, out, cap); return; }
    BYTE *data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, size);
    if (!data || !GetFileVersionInfoW(path, 0, size, data)) {
        if (data) HeapFree(GetProcessHeap(), 0, data);
        basename_no_ext(path, out, cap);
        return;
    }
    struct LANGANDCODEPAGE { WORD language, codepage; } *trans = NULL;
    UINT transBytes = 0;
    wchar_t query[96];
    wchar_t *value = NULL;
    UINT valueChars = 0;
    if (VerQueryValueW(data, L"\\VarFileInfo\\Translation", (LPVOID *)&trans, &transBytes)
        && transBytes >= sizeof(*trans)) {
        _snwprintf(query, 95, L"\\StringFileInfo\\%04x%04x\\FileDescription",
                   trans[0].language, trans[0].codepage);
        query[95] = 0;
        VerQueryValueW(data, query, (LPVOID *)&value, &valueChars);
    }
    if (value && *value) safe_copy(out, cap, value);
    else basename_no_ext(path, out, cap);
    HeapFree(GetProcessHeap(), 0, data);
}

static int path_exists(const wchar_t *path) {
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static void parse_command_path(const wchar_t *command, wchar_t *out, size_t cap) {
    wchar_t expanded[PATH_CAP * 2];
    ExpandEnvironmentStringsW(command, expanded, (DWORD)(sizeof(expanded) / sizeof(expanded[0])));
    const wchar_t *start = expanded;
    while (*start == L' ' || *start == L'\t') ++start;
    if (*start == L'\"') {
        ++start;
        const wchar_t *end = wcschr(start, L'\"');
        size_t n = end ? (size_t)(end - start) : wcslen(start);
        if (n >= cap) n = cap - 1;
        for (size_t i = 0; i < n; ++i) out[i] = start[i]; out[n] = 0;
        return;
    }
    const wchar_t *end = start;
    while (*end) {
        if ((end[0] == L'.') && (end[1] == L'e' || end[1] == L'E') &&
            (end[2] == L'x' || end[2] == L'X') && (end[3] == L'e' || end[3] == L'E')) {
            end += 4; break;
        }
        ++end;
    }
    if (end == start) return;
    size_t n = (size_t)(end - start);
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; ++i) out[i] = start[i]; out[n] = 0;
}

static int find_app_index(const wchar_t *path) {
    if (!path || !*path) return -1;
    for (int i = 0; i < g_count; ++i)
        if (_wcsicmp(g_apps[i].path, path) == 0) return i;
    return -1;
}

static int is_batch_or_command_script(const wchar_t *path) {
    const wchar_t *extension = wcsrchr(path, L'.');
    return extension && (!_wcsicmp(extension, L".bat") || !_wcsicmp(extension, L".cmd"));
}

static HICON load_embedded_icon(int resourceId) {
    return (HICON)LoadImageW(
        GetModuleHandleW(NULL), MAKEINTRESOURCEW(resourceId), IMAGE_ICON,
        PX(32), PX(32), LR_DEFAULTCOLOR
    );
}

static int add_app(const wchar_t *path, int first) {
    if (!path || !*path || !path_exists(path)) return 0;
    if (find_app_index(path) >= 0) return 0;
    if (g_count >= APP_MAX) return 0;
    int at = first ? 0 : g_count;
    if (first && g_count) memmove(&g_apps[1], &g_apps[0], sizeof(APP_ITEM) * g_count);
    safe_copy(g_apps[at].path, PATH_CAP, path);
    get_file_description(path, g_apps[at].name, 256);
    HICON large = NULL, small = NULL;
    ExtractIconExW(path, 0, &large, &small, 1);
    if (is_batch_or_command_script(path)) {
        if (large) DestroyIcon(large);
        if (small) DestroyIcon(small);
        g_apps[at].icon = load_embedded_icon(IDI_SCRIPT);
    } else {
        g_apps[at].icon = large ? large : small;
        if (large && small) DestroyIcon(small);
        if (!g_apps[at].icon)
            g_apps[at].icon = load_embedded_icon(IDI_PLACEHOLDER);
    }
    ++g_count;
    return 1;
}

/* 从 HKCR\Applications 枚举当前 PE 已注册的可用程序。 */
static void enum_apps(void) {
    HKEY root;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, L"Applications", 0, KEY_READ, &root) != ERROR_SUCCESS) return;
    for (DWORD index = 0;; ++index) {
        wchar_t subName[260]; DWORD chars = 260;
        if (RegEnumKeyExW(root, index, subName, &chars, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        HKEY app;
        if (RegOpenKeyExW(root, subName, 0, KEY_READ, &app) != ERROR_SUCCESS) continue;
        if (RegQueryValueExW(app, L"NoOpenWith", NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            RegCloseKey(app); continue;
        }
        wchar_t command[PATH_CAP * 2]; DWORD type = 0, bytes = sizeof(command);
        if (RegGetValueW(app, L"shell\\open\\command", NULL, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                         &type, command, &bytes) == ERROR_SUCCESS) {
            wchar_t path[PATH_CAP] = L"";
            parse_command_path(command, path, PATH_CAP);
            add_app(path, 0);
        }
        RegCloseKey(app);
    }
    RegCloseKey(root);
}

/* ============================== 界面布局与列表 ============================== */

static void get_extension(void) {
    g_extension[0] = 0;
    const wchar_t *base = g_file;
    for (const wchar_t *p = g_file; *p; ++p)
        if (*p == L'\\' || *p == L'/') base = p + 1;

    const wchar_t *dot = wcsrchr(base, L'.');

    /* 无扩展名文件只允许“仅一次”。.gitignore、file. 也按无扩展名处理。 */
    if (!dot || dot == base || dot[1] == 0) return;
    safe_copy(g_extension, 64, dot);
}

static int has_extension(void) { return g_extension[0] != 0; }

static RECT browse_rect(void) { RECT r = {PX(24), PX(303), PX(300), PX(329)}; return r; }
static RECT always_rect(void) { RECT r = {PX(24), PX(361), PX(190), PX(395)}; return r; }
static RECT once_rect(void) { RECT r = {PX(206), PX(361), PX(372), PX(395)}; return r; }
static RECT once_only_rect(void) { RECT r = {PX(24), PX(361), PX(372), PX(395)}; return r; }

static int point_in(RECT r, int x, int y) {
    POINT p = {x, y}; return PtInRect(&r, p);
}

static int visible_rows(void) { return (LIST_BOTTOM - LIST_TOP) / ROW_H; }

static int scroll_view_height(void) { return LIST_BOTTOM - LIST_TOP; }
static int scroll_content_height(void) { return g_count * ROW_H; }
static int max_scroll_pos(void) {
    int maxPos = scroll_content_height() - scroll_view_height();
    return maxPos > 0 ? maxPos : 0;
}
static int clamp_scroll_pos(int pos) {
    int maxPos = max_scroll_pos();
    if (pos < 0) return 0;
    if (pos > maxPos) return maxPos;
    return pos;
}

/* 平滑滚轮动画只动态取 SetTimer/KillTimer，避免给精简 PE 增加新的静态导入。 */
static void start_scroll_timer(HWND hwnd) {
    typedef UINT_PTR (WINAPI *PFN_SETTIMER)(HWND,UINT_PTR,UINT,void*);
    HMODULE user = LoadLibraryW(L"user32.dll");
    if (!user) return;
    PFN_SETTIMER fn = (PFN_SETTIMER)GetProcAddress(user, "SetTimer");
    if (fn) fn(hwnd, SCROLL_TIMER_ID, 15, NULL);
    FreeLibrary(user);
}
static void stop_scroll_timer(HWND hwnd) {
    typedef BOOL (WINAPI *PFN_KILLTIMER)(HWND,UINT_PTR);
    HMODULE user = LoadLibraryW(L"user32.dll");
    if (!user) return;
    PFN_KILLTIMER fn = (PFN_KILLTIMER)GetProcAddress(user, "KillTimer");
    if (fn) fn(hwnd, SCROLL_TIMER_ID);
    FreeLibrary(user);
}

/* 让指定项目进入可视区域。已有项目从“在电脑上选择应用”重新选中时会用到。 */
static void reveal_app_index(HWND hwnd, int index) {
    if (index < 0 || index >= g_count) return;
    int viewH = scroll_view_height();
    int itemTop = index * ROW_H;
    int itemBottom = itemTop + ROW_H;
    int target = g_scrollTarget;

    if (itemTop < target) target = itemTop;
    else if (itemBottom > target + viewH) target = itemBottom - viewH;

    target = clamp_scroll_pos(target);
    g_selected = index;
    g_wheelRemainder = 0;
    g_scrollTarget = target;

    if (g_scroll != g_scrollTarget) start_scroll_timer(hwnd);
    InvalidateRect(hwnd, NULL, FALSE);
}

/* 列表按像素滚动；这里用 GDI clip 保证半行不会画到标题/链接区域。 */
static int push_list_clip(HDC dc) {
    typedef int (WINAPI *PFN_SAVEDC)(HDC);
    typedef int (WINAPI *PFN_INTERSECTCLIPRECT)(HDC,int,int,int,int);
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    if (!gdi) return 0;
    PFN_SAVEDC save = (PFN_SAVEDC)GetProcAddress(gdi, "SaveDC");
    PFN_INTERSECTCLIPRECT clip = (PFN_INTERSECTCLIPRECT)GetProcAddress(gdi, "IntersectClipRect");
    int id = save ? save(dc) : 0;
    if (id && clip) clip(dc, 0, LIST_TOP, WIN_W, LIST_BOTTOM);
    FreeLibrary(gdi);
    return id;
}
static void pop_list_clip(HDC dc, int id) {
    typedef BOOL (WINAPI *PFN_RESTOREDC)(HDC,int);
    if (!id) return;
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    if (!gdi) return;
    PFN_RESTOREDC restore = (PFN_RESTOREDC)GetProcAddress(gdi, "RestoreDC");
    if (restore) restore(dc, id);
    FreeLibrary(gdi);
}

static int hit_test(int x, int y) {
    if (point_in(browse_rect(), x, y)) return HIT_BROWSE;
    if (has_extension()) {
        if (point_in(always_rect(), x, y)) return HIT_ALWAYS;
        if (point_in(once_rect(), x, y)) return HIT_ONCE;
    } else {
        if (point_in(once_only_rect(), x, y)) return HIT_ONCE;
    }
    if (x >= PX(14) && x < WIN_W - PX(14) && y >= LIST_TOP && y < LIST_BOTTOM) {
        int row = (y - LIST_TOP + g_scroll) / ROW_H;
        if (row >= 0 && row < g_count) return row;
    }
    return HIT_NONE;
}

static void paint_app_row(HDC dc, int index, int top) {
    /* 行底板稍后直接画进 ARGB 位图，以便按系统版本切换 G2 圆角/直角。 */
    if (g_apps[index].icon) DrawIconEx(dc, PX(26), top + PX(9), g_apps[index].icon, PX(32), PX(32), 0, NULL, DI_NORMAL);
    RECT text = {PX(69), top, WIN_W - PX(24), top + ROW_H - PX(2)};
    draw_text(dc, g_textFont, g_pal.text, g_apps[index].name, text,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

static void paint_button(HDC dc, RECT r, int hit, const wchar_t *label) {
    int enabled = g_selected >= 0;
    (void)hit;
    /* 按钮底板稍后直接画进 ARGB 位图，这里只画文字。 */
    draw_text(dc, g_textFont, enabled ? g_pal.text : g_pal.disabled, label, r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}


/* ============================== 抗锯齿控件 ============================== */

static BYTE g2_rect_alpha_at(int x, int y, const RECT *r, int radius) {
    if (x < r->left || y < r->top || x >= r->right || y >= r->bottom) return 0;
    int w = r->right - r->left;
    int h = r->bottom - r->top;
    int rr = radius;
    if (rr < 0) rr = 0;
    if (rr * 2 > w) rr = w / 2;
    if (rr * 2 > h) rr = h / 2;
    if (rr <= 0) return 255;

    if ((x >= r->left + rr && x < r->right - rr) ||
        (y >= r->top + rr && y < r->bottom - rr)) return 255;

    const int ss = AA_SS;
    const int scale = 2 * ss;
    const long long R = (long long)rr * scale;
    const unsigned long long R2 = (unsigned long long)R * (unsigned long long)R;
    const unsigned long long R4 = R2 * R2;
    const long long left = (long long)r->left * scale;
    const long long top = (long long)r->top * scale;
    const long long right = (long long)r->right * scale;
    const long long bottom = (long long)r->bottom * scale;
    const long long cxLeft = left + R;
    const long long cxRight = right - R;
    const long long cyTop = top + R;
    const long long cyBottom = bottom - R;
    int inside = 0;

    for (int sy = 0; sy < ss; ++sy) {
        for (int sx = 0; sx < ss; ++sx) {
            long long px = (long long)x * scale + (2 * sx + 1);
            long long py = (long long)y * scale + (2 * sy + 1);
            if ((px >= cxLeft && px < cxRight) || (py >= cyTop && py < cyBottom)) {
                ++inside;
                continue;
            }
            long long cx = px < cxLeft ? cxLeft : cxRight;
            long long cy = py < cyTop ? cyTop : cyBottom;
            long long dx = px - cx; if (dx < 0) dx = -dx;
            long long dy = py - cy; if (dy < 0) dy = -dy;
            unsigned long long dx2 = (unsigned long long)dx * (unsigned long long)dx;
            unsigned long long dy2 = (unsigned long long)dy * (unsigned long long)dy;
            unsigned long long v = dx2 * dx2 + dy2 * dy2;
            if (v <= R4) ++inside;
        }
    }
    return (BYTE)((inside * 255 + (ss * ss) / 2) / (ss * ss));
}

static void blend_pixel_color(DWORD *bits, int w, int h, int x, int y, COLORREF color, BYTE alpha) {
    if (!bits || alpha == 0 || x < 0 || y < 0 || x >= w || y >= h) return;
    SIZE_T i = (SIZE_T)y * (SIZE_T)w + (SIZE_T)x;
    DWORD dst = bits[i];
    unsigned int db = dst & 0xFFu;
    unsigned int dg = (dst >> 8) & 0xFFu;
    unsigned int dr = (dst >> 16) & 0xFFu;
    unsigned int sr = (unsigned int)(color & 0xFFu);
    unsigned int sg = (unsigned int)((color >> 8) & 0xFFu);
    unsigned int sb = (unsigned int)((color >> 16) & 0xFFu);
    unsigned int a = (unsigned int)alpha;
    unsigned int ia = 255u - a;
    unsigned int orr = (sr * a + dr * ia + 127u) / 255u;
    unsigned int og = (sg * a + dg * ia + 127u) / 255u;
    unsigned int ob = (sb * a + db * ia + 127u) / 255u;
    bits[i] = (DWORD)((orr << 16) | (og << 8) | ob);
}

static void fill_g2_rect_aa(DWORD *bits, int w, int h, const RECT *r, int radius,
                            COLORREF color, BYTE alpha) {
    if (!bits || !r || alpha == 0) return;
    int left = r->left, top = r->top, right = r->right, bottom = r->bottom;
    if (left < 0) left = 0; if (top < 0) top = 0;
    if (right > w) right = w; if (bottom > h) bottom = h;
    RECT clipped = {left, top, right, bottom};
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            BYTE cov = g2_rect_alpha_at(x, y, &clipped, radius);
            if (!cov) continue;
            unsigned int a = (unsigned int)cov * (unsigned int)alpha;
            blend_pixel_color(bits, w, h, x, y, color, (BYTE)((a + 127u) / 255u));
        }
    }
}

static BYTE capsule_alpha_at(int x, int y, const RECT *r) {
    if (x < r->left || y < r->top || x >= r->right || y >= r->bottom) return 0;
    int rw = r->right - r->left;
    int rh = r->bottom - r->top;
    if (rw <= 0 || rh <= 0) return 0;

    /* 滚动条只有几像素宽，用超椭圆反而显方，这里直接画抗锯齿胶囊。 */
    const int ss = 8;
    const int scale = 2 * ss;
    long long left = (long long)r->left * scale;
    long long right = (long long)r->right * scale;
    long long top = (long long)r->top * scale;
    long long bottom = (long long)r->bottom * scale;
    long long halfW = ((long long)rw * scale) / 2;
    long long cx = (left + right) / 2;
    long long topCy = top + halfW;
    long long bottomCy = bottom - halfW;
    unsigned long long rr = (unsigned long long)halfW * (unsigned long long)halfW;
    int inside = 0;

    for (int sy = 0; sy < ss; ++sy) {
        for (int sx = 0; sx < ss; ++sx) {
            long long px = (long long)x * scale + (2 * sx + 1);
            long long py = (long long)y * scale + (2 * sy + 1);
            int hit = 0;
            if (py >= topCy && py <= bottomCy) {
                hit = (px >= left && px <= right);
            } else {
                long long cy = py < topCy ? topCy : bottomCy;
                long long dx = px - cx;
                long long dy = py - cy;
                unsigned long long d2 = (unsigned long long)(dx * dx) +
                                        (unsigned long long)(dy * dy);
                hit = d2 <= rr;
            }
            if (hit) ++inside;
        }
    }
    return (BYTE)((inside * 255 + (ss * ss) / 2) / (ss * ss));
}

static void fill_capsule_aa(DWORD *bits, int w, int h, const RECT *r,
                            COLORREF color, BYTE alpha) {
    if (!bits || !r || alpha == 0) return;
    int left = r->left, top = r->top, right = r->right, bottom = r->bottom;
    if (left < 0) left = 0; if (top < 0) top = 0;
    if (right > w) right = w; if (bottom > h) bottom = h;
    RECT clipped = {left, top, right, bottom};
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            BYTE cov = capsule_alpha_at(x, y, &clipped);
            if (!cov) continue;
            unsigned int a = (unsigned int)cov * (unsigned int)alpha;
            blend_pixel_color(bits, w, h, x, y, color,
                              (BYTE)((a + 127u) / 255u));
        }
    }
}


static void fill_g2_outline_aa(DWORD *bits, int w, int h, const RECT *outer, int outerRadius,
                               int borderWidth, COLORREF borderColor,
                               COLORREF fillColor, BYTE borderAlpha, BYTE fillAlpha) {
    RECT inner = *outer;
    int bw = borderWidth;
    if (bw < 1) bw = 1;
    inner.left += bw; inner.top += bw; inner.right -= bw; inner.bottom -= bw;
    if (inner.right <= inner.left || inner.bottom <= inner.top) {
        fill_g2_rect_aa(bits, w, h, outer, outerRadius, borderColor, borderAlpha);
        return;
    }
    int innerRadius = outerRadius > 0 ? outerRadius - bw : 0;
    if (outerRadius > 0 && innerRadius < 1) innerRadius = 1;
    fill_g2_rect_aa(bits, w, h, outer, outerRadius, borderColor, borderAlpha);
    fill_g2_rect_aa(bits, w, h, &inner, innerRadius, fillColor, fillAlpha);
}

static void fill_g2_rect_aa_clipped(DWORD *bits, int w, int h, const RECT *r,
                                    const RECT *clip, int radius,
                                    COLORREF color, BYTE alpha) {
    if (!bits || !r || !clip || alpha == 0) return;
    int left = r->left > clip->left ? r->left : clip->left;
    int top = r->top > clip->top ? r->top : clip->top;
    int right = r->right < clip->right ? r->right : clip->right;
    int bottom = r->bottom < clip->bottom ? r->bottom : clip->bottom;
    if (left >= right || top >= bottom) return;
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            BYTE cov = g2_rect_alpha_at(x, y, r, radius);
            if (!cov) continue;
            unsigned int a = (unsigned int)cov * (unsigned int)alpha;
            blend_pixel_color(bits, w, h, x, y, color,
                              (BYTE)((a + 127u) / 255u));
        }
    }
}

static void paint_row_backgrounds_bits(DWORD *bits, int w, int h) {
    if (!bits || ROW_H <= 0) return;
    int first = g_scroll / ROW_H;
    int offset = g_scroll % ROW_H;
    int top = LIST_TOP - offset;
    RECT clip = {0, LIST_TOP, WIN_W, LIST_BOTTOM};
    for (int index = first; index < g_count && top < LIST_BOTTOM; ++index, top += ROW_H) {
        if (top + ROW_H <= LIST_TOP) continue;
        if (index != g_selected && index != g_hover) continue;
        /* 列表高亮左右各留 14px；滚动条作为悬浮层，不占列表宽度。 */
        RECT rc = {PX(14), top, WIN_W - PX(14), top + ROW_H - PX(2)};
        COLORREF fill = (index == g_selected) ? g_pal.selected : g_pal.hover;
        int radius = g_useRounded ? PX(7) : 0;
        fill_g2_rect_aa_clipped(bits, w, h, &rc, &clip, radius, fill, 255);
    }
}

static void paint_buttons_bits(DWORD *bits, int w, int h) {
    if (!bits) return;
    int enabled = g_selected >= 0;
    RECT r1, r2;
    int count;
    if (has_extension()) {
        r1 = always_rect();
        r2 = once_rect();
        count = 2;
    } else {
        r1 = once_only_rect();
        r2 = r1;
        count = 1;
    }
    RECT rects[2] = {r1, r2};
    int hits[2] = { has_extension() ? HIT_ALWAYS : HIT_ONCE, HIT_ONCE };
    int radius = g_useRounded ? PX(8) : 0;
    for (int i = 0; i < count; ++i) {
        COLORREF fill = g_pal.button;
        if (enabled && g_pressed == hits[i]) fill = g_pal.buttonPressed;
        else if (enabled && g_hover == hits[i]) fill = g_pal.buttonHover;
        fill_g2_outline_aa(bits, w, h, &rects[i], radius, PX(1),
                           g_pal.border, fill, 255, 255);
    }
}

static void fill_capsule_aa_clipped(DWORD *bits, int w, int h, const RECT *r,
                                    const RECT *clip, COLORREF color, BYTE alpha) {
    if (!bits || !r || !clip || alpha == 0) return;
    int left = r->left > clip->left ? r->left : clip->left;
    int top = r->top > clip->top ? r->top : clip->top;
    int right = r->right < clip->right ? r->right : clip->right;
    int bottom = r->bottom < clip->bottom ? r->bottom : clip->bottom;
    if (left >= right || top >= bottom) return;
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            BYTE cov = capsule_alpha_at(x, y, r);
            if (!cov) continue;
            unsigned int a = (unsigned int)cov * (unsigned int)alpha;
            blend_pixel_color(bits, w, h, x, y, color,
                              (BYTE)((a + 127u) / 255u));
        }
    }
}

static void paint_selection_accent_bits(DWORD *bits, int w, int h) {
    if (!bits || g_selected < 0) return;
    int top = LIST_TOP + g_selected * ROW_H - g_scroll;
    if (top >= LIST_BOTTOM || top + ROW_H <= LIST_TOP) return;
    RECT accent = {PX(14), top + PX(14), PX(18), top + PX(36)};
    RECT clip = {0, LIST_TOP, WIN_W, LIST_BOTTOM};
    if (g_useRounded)
        fill_capsule_aa_clipped(bits, w, h, &accent, &clip, g_pal.accent, 255);
    else
        fill_g2_rect_aa_clipped(bits, w, h, &accent, &clip, 0, g_pal.accent, 255);
}

static int get_scrollbar_geometry(RECT *track, RECT *thumb) {
    int viewH = scroll_view_height();
    int contentH = scroll_content_height();
    int maxScroll = max_scroll_pos();
    if (maxScroll <= 0 || contentH <= 0) return 0;

    /* 无可见轨道。视觉宽度固定为 2 个逻辑像素，不随系统版本改变。 */
    int sbRight = WIN_W - PX(4);
    int sbWidth = PX(2); if (sbWidth < 1) sbWidth = 1;
    RECT tr = {sbRight - sbWidth, LIST_TOP + PX(5), sbRight, LIST_BOTTOM - PX(5)};
    int trackH = tr.bottom - tr.top;
    int thumbH = (trackH * viewH) / contentH;
    if (thumbH < PX(24)) thumbH = PX(24);
    if (thumbH > trackH) thumbH = trackH;
    int travel = trackH - thumbH;
    int thumbTop = tr.top;
    if (travel > 0)
        thumbTop += (travel * g_scroll + maxScroll / 2) / maxScroll;
    RECT th = {tr.left, thumbTop, tr.right, thumbTop + thumbH};
    if (track) *track = tr;
    if (thumb) *thumb = th;
    return 1;
}

static int point_in_scrollbar_hitbox(int x, int y, int thumbOnly) {
    RECT track, thumb;
    if (!get_scrollbar_geometry(&track, &thumb)) return 0;
    RECT r = thumbOnly ? thumb : track;
    /* 视觉只有 2px，命中区域仍然保持宽一些，避免难以拖动。 */
    r.left -= PX(7); r.right += PX(4);
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

static void set_scroll_from_thumb_top(int thumbTop) {
    RECT track, thumb;
    if (!get_scrollbar_geometry(&track, &thumb)) return;
    int maxScroll = max_scroll_pos();
    int travel = (track.bottom - track.top) - (thumb.bottom - thumb.top);
    if (maxScroll <= 0 || travel <= 0) { g_scroll = g_scrollTarget = 0; return; }
    int pos = thumbTop - track.top;
    if (pos < 0) pos = 0;
    if (pos > travel) pos = travel;
    g_scroll = (pos * maxScroll + travel / 2) / travel;
    g_scroll = clamp_scroll_pos(g_scroll);
    g_scrollTarget = g_scroll;
}

static void paint_scrollbar_bits(DWORD *bits, int w, int h) {
    if (!bits) return;
    RECT track, thumb;
    if (!get_scrollbar_geometry(&track, &thumb)) return;
    (void)track;
    /* 不画滚动条背景。亮色取样自 Win11 原版，主体约 RGB(140,140,140)。 */
    COLORREF scrollColor = g_dark ? RGB(174,174,174) : RGB(140,140,140);
    if (g_useRounded)
        fill_capsule_aa(bits, w, h, &thumb, scrollColor, 255);
    else
        fill_g2_rect_aa(bits, w, h, &thumb, 0, scrollColor, 255);
}


static BYTE body_alpha_at(int x, int y);

/*
 * 窗口外沿的 1 个逻辑像素细边框。
 * 分层窗口没有系统非客户区，所以边框直接画进 ARGB 表面里。
 * 外轮廓和内轮廓都沿用同一套 G2 曲线，避免圆角处出现突兀的直角边。
 */
static void paint_window_border_bits(DWORD *bits, int w, int h) {
    if (!bits || w <= 0 || h <= 0) return;
    int bw = BORDER_WIDTH;
    if (bw < 1) bw = 1;
    if (bw * 2 >= w || bw * 2 >= h) return;

    RECT inner = {bw, bw, w - bw, h - bw};
    int innerRadius = BODY_RADIUS - bw;
    if (innerRadius < 1) innerRadius = 1;

    /*
     * 亮色主题用很淡的灰边，暗色主题稍微提亮一点。
     * 这里不直接用按钮/分隔线的 border 色，否则外框会显得太重。
     */
    /* 这里取样自你给的 Win11 截图，亮色边框约为 #F1F1F1。 */
    COLORREF edge = g_dark ? RGB(82,82,82) : RGB(173,173,173);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            BYTE outer = body_alpha_at(x, y);
            if (!outer) continue;
            BYTE innerA;
            if (g_useRounded) innerA = g2_rect_alpha_at(x, y, &inner, innerRadius);
            else innerA = (x >= inner.left && x < inner.right && y >= inner.top && y < inner.bottom) ? 255 : 0;
            unsigned int cov = ((unsigned int)outer * (255u - (unsigned int)innerA) + 127u) / 255u;
            if (!cov) continue;
            /* Win11 的外框非常轻，避免做成明显描边。 */
            /* 亮色主题直接按提取到的颜色实打实画出来；否则会淡到看不见。 */
            unsigned int alpha = (cov * (g_dark ? 190u : 255u) + 127u) / 255u;
            blend_pixel_color(bits, w, h, x, y, edge, (BYTE)alpha);
        }
    }
}

static void paint_content_base(HDC dc) {
    RECT client = {0, 0, WIN_W, WIN_H};
    fill_rect(dc, &client, g_pal.background);
}

static void paint_content_foreground(HDC dc) {
    const wchar_t *title = L"选择一个应用以打开此文件";
    RECT titleRect = {PX(24), PX(21), PX(374), PX(62)};
    draw_text(dc, g_titleFont, g_pal.text, title, titleRect,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    int first = ROW_H > 0 ? g_scroll / ROW_H : 0;
    int offset = ROW_H > 0 ? g_scroll % ROW_H : 0;
    int top = LIST_TOP - offset;
    int clipId = push_list_clip(dc);
    for (int index = first; index < g_count && top < LIST_BOTTOM; ++index, top += ROW_H) {
        if (top + ROW_H > LIST_TOP) paint_app_row(dc, index, top);
    }
    pop_list_clip(dc, clipId);
    /* 滚动条最后直接画到 32 位位图上，保证小尺寸下也有平滑边缘。 */

    RECT browse = browse_rect();
    draw_text(dc, g_linkFont, g_hover == HIT_BROWSE ? g_pal.accent : g_pal.link,
              L"在电脑上选择应用", browse, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT separator = {0, PX(350), WIN_W, PX(351)}; fill_rect(dc, &separator, g_pal.border);
    if (has_extension()) {
        paint_button(dc, always_rect(), HIT_ALWAYS, L"始终");
        paint_button(dc, once_rect(), HIT_ONCE, L"仅一次");
    } else {
        paint_button(dc, once_only_rect(), HIT_ONCE, L"仅一次");
    }
}

/* 窗口外轮廓用四次超椭圆近似连续曲率圆角，再用 4x 超采样生成 Alpha 边缘。 */
/* ============================== 分层窗口与阴影 ============================== */

static BYTE body_alpha_at(int x, int y) {
    if (x < 0 || y < 0 || x >= WIN_W || y >= WIN_H) return 0;
    if (!g_useRounded) return 255;
    const int r = BODY_RADIUS;
    if ((x >= r && x < WIN_W - r) || (y >= r && y < WIN_H - r)) return 255;

    int cornerX = x < r ? r : (WIN_W - r);
    int cornerY = y < r ? r : (WIN_H - r);
    int inside = 0;
    const int ss = AA_SS;
    const int scale = 2 * ss;
    const long long R = (long long)r * scale;
    const unsigned long long R2 = (unsigned long long)R * (unsigned long long)R;
    const unsigned long long R4 = R2 * R2;

    for (int sy = 0; sy < ss; ++sy) {
        for (int sx = 0; sx < ss; ++sx) {
            long long px = (long long)x * scale + (2 * sx + 1);
            long long py = (long long)y * scale + (2 * sy + 1);
            long long cx = (long long)cornerX * scale;
            long long cy = (long long)cornerY * scale;
            long long dx = px - cx; if (dx < 0) dx = -dx;
            long long dy = py - cy; if (dy < 0) dy = -dy;
            unsigned long long dx2 = (unsigned long long)dx * (unsigned long long)dx;
            unsigned long long dy2 = (unsigned long long)dy * (unsigned long long)dy;
            unsigned long long v = dx2 * dx2 + dy2 * dy2;
            if (v <= R4) ++inside;
        }
    }
    return (BYTE)((inside * 255 + (ss * ss) / 2) / (ss * ss));
}

static void box_blur_h(const BYTE *src, BYTE *dst, int w, int h, int radius) {
    int span = radius * 2 + 1;
    for (int y = 0; y < h; ++y) {
        unsigned int sum = 0;
        for (int k = -radius; k <= radius; ++k) {
            int x = k; if (x < 0) x = 0; if (x >= w) x = w - 1;
            sum += src[y * w + x];
        }
        for (int x = 0; x < w; ++x) {
            dst[y * w + x] = (BYTE)(sum / (unsigned int)span);
            int oldx = x - radius; if (oldx < 0) oldx = 0;
            int newx = x + radius + 1; if (newx >= w) newx = w - 1;
            sum += src[y * w + newx];
            sum -= src[y * w + oldx];
        }
    }
}

static void box_blur_v(const BYTE *src, BYTE *dst, int w, int h, int radius) {
    int span = radius * 2 + 1;
    for (int x = 0; x < w; ++x) {
        unsigned int sum = 0;
        for (int k = -radius; k <= radius; ++k) {
            int y = k; if (y < 0) y = 0; if (y >= h) y = h - 1;
            sum += src[y * w + x];
        }
        for (int y = 0; y < h; ++y) {
            dst[y * w + x] = (BYTE)(sum / (unsigned int)span);
            int oldy = y - radius; if (oldy < 0) oldy = 0;
            int newy = y + radius + 1; if (newy >= h) newy = h - 1;
            sum += src[newy * w + x];
            sum -= src[oldy * w + x];
        }
    }
}

static void make_blur(const BYTE *src, BYTE *out, BYTE *tmp, int w, int h, int radius) {
    box_blur_h(src, tmp, w, h, radius);
    box_blur_v(tmp, out, w, h, radius);
    /* 再模糊一遍，削掉 Box Blur 容易出现的方肩。 */
    box_blur_h(out, tmp, w, h, radius);
    box_blur_v(tmp, out, w, h, radius);
}

/* 用 32 位 DIB + UpdateLayeredWindow 一次完成圆角、阴影和控件 AA。 */
static int render_layered_window(HWND hwnd) {
    typedef HBITMAP (WINAPI *PFN_CREATEDIBSECTION)(HDC,const BITMAPINFO*,UINT,void**,HANDLE,DWORD);
    typedef BOOL (WINAPI *PFN_UPDATELAYEREDWINDOW)(HWND,HDC,const POINT*,const SIZE*,HDC,const POINT*,COLORREF,const BLENDFUNCTION*,DWORD);

    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    HMODULE user = LoadLibraryW(L"user32.dll");
    if (!gdi || !user) { if (gdi) FreeLibrary(gdi); if (user) FreeLibrary(user); return 0; }
    PFN_CREATEDIBSECTION pCreateDIBSection = (PFN_CREATEDIBSECTION)GetProcAddress(gdi, "CreateDIBSection");
    PFN_UPDATELAYEREDWINDOW pUpdateLayeredWindow = (PFN_UPDATELAYEREDWINDOW)GetProcAddress(user, "UpdateLayeredWindow");
    if (!pCreateDIBSection || !pUpdateLayeredWindow) { FreeLibrary(gdi); FreeLibrary(user); return 0; }

    HDC contentDC = CreateCompatibleDC(NULL);
    HDC layerDC = CreateCompatibleDC(NULL);
    if (!contentDC || !layerDC) {
        if (contentDC) DeleteDC(contentDC); if (layerDC) DeleteDC(layerDC);
        FreeLibrary(gdi); FreeLibrary(user); return 0;
    }

    BITMAPINFO cbi; ZeroMemory(&cbi, sizeof(cbi));
    cbi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    cbi.bmiHeader.biWidth = WIN_W; cbi.bmiHeader.biHeight = -WIN_H;
    cbi.bmiHeader.biPlanes = 1; cbi.bmiHeader.biBitCount = 32; cbi.bmiHeader.biCompression = BI_RGB;
    DWORD *contentBits = NULL;
    HBITMAP contentBmp = pCreateDIBSection(contentDC, &cbi, DIB_RGB_COLORS, (void **)&contentBits, NULL, 0);

    BITMAPINFO lbi; ZeroMemory(&lbi, sizeof(lbi));
    lbi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    lbi.bmiHeader.biWidth = LAYER_W; lbi.bmiHeader.biHeight = -LAYER_H;
    lbi.bmiHeader.biPlanes = 1; lbi.bmiHeader.biBitCount = 32; lbi.bmiHeader.biCompression = BI_RGB;
    DWORD *layerBits = NULL;
    HBITMAP layerBmp = pCreateDIBSection(layerDC, &lbi, DIB_RGB_COLORS, (void **)&layerBits, NULL, 0);
    if (!contentBmp || !layerBmp || !contentBits || !layerBits) {
        if (contentBmp) DeleteObject(contentBmp); if (layerBmp) DeleteObject(layerBmp);
        DeleteDC(contentDC); DeleteDC(layerDC); FreeLibrary(gdi); FreeLibrary(user); return 0;
    }

    HGDIOBJ oldContent = SelectObject(contentDC, contentBmp);
    HGDIOBJ oldLayer = SelectObject(layerDC, layerBmp);
    paint_content_base(contentDC);
    paint_row_backgrounds_bits(contentBits, WIN_W, WIN_H);
    paint_buttons_bits(contentBits, WIN_W, WIN_H);
    paint_content_foreground(contentDC);
    paint_selection_accent_bits(contentBits, WIN_W, WIN_H);
    paint_scrollbar_bits(contentBits, WIN_W, WIN_H);
    paint_window_border_bits(contentBits, WIN_W, WIN_H);

    SIZE_T pixels = (SIZE_T)LAYER_W * (SIZE_T)LAYER_H;
    HANDLE heap = GetProcessHeap();
    BYTE *ambientSrc = (BYTE *)HeapAlloc(heap, HEAP_ZERO_MEMORY, pixels);
    BYTE *dropSrc = (BYTE *)HeapAlloc(heap, HEAP_ZERO_MEMORY, pixels);
    BYTE *tmp = (BYTE *)HeapAlloc(heap, HEAP_ZERO_MEMORY, pixels);
    BYTE *ambientBlur = (BYTE *)HeapAlloc(heap, HEAP_ZERO_MEMORY, pixels);
    BYTE *dropBlur = (BYTE *)HeapAlloc(heap, HEAP_ZERO_MEMORY, pixels);
    if (!ambientSrc || !dropSrc || !tmp || !ambientBlur || !dropBlur) {
        if (ambientSrc) HeapFree(heap,0,ambientSrc); if (dropSrc) HeapFree(heap,0,dropSrc);
        if (tmp) HeapFree(heap,0,tmp); if (ambientBlur) HeapFree(heap,0,ambientBlur);
        if (dropBlur) HeapFree(heap,0,dropBlur);
        SelectObject(contentDC, oldContent); SelectObject(layerDC, oldLayer);
        DeleteObject(contentBmp); DeleteObject(layerBmp); DeleteDC(contentDC); DeleteDC(layerDC);
        FreeLibrary(gdi); FreeLibrary(user); return 0;
    }

    if (g_shadowEnabled) {
        /* 两层阴影：一层大范围环境阴影，一层轻微下移的投影。 */
        for (int y = 0; y < WIN_H; ++y) {
            for (int x = 0; x < WIN_W; ++x) {
                BYTE a = body_alpha_at(x, y);
                int ax = SHADOW_PAD + x;
                int ay = SHADOW_PAD + y;
                int dx = SHADOW_PAD + x;
                int dy = SHADOW_PAD + SHADOW_Y + y;
                if (ax >= 0 && ax < LAYER_W && ay >= 0 && ay < LAYER_H)
                    ambientSrc[ay * LAYER_W + ax] = a;
                if (dx >= 0 && dx < LAYER_W && dy >= 0 && dy < LAYER_H)
                    dropSrc[dy * LAYER_W + dx] = a;
            }
        }
        make_blur(ambientSrc, ambientBlur, tmp, LAYER_W, LAYER_H, PX(18));
        make_blur(dropSrc, dropBlur, tmp, LAYER_W, LAYER_H, PX(11));

        for (SIZE_T i = 0; i < pixels; ++i) {
            unsigned int aa = (unsigned int)ambientBlur[i];
            unsigned int da = (unsigned int)dropBlur[i];
            /* 范围更大，但近边缘不刻意压黑，整体比上一版更柔。 */
            unsigned int a = (aa * 26u + da * 44u + 127u) / 255u;
            if (a > 74u) a = 74u;
            layerBits[i] = (DWORD)(a << 24); /* 预乘 Alpha 的黑色阴影 */
        }
    } else {
        ZeroMemory(layerBits, pixels * sizeof(DWORD));
    }

    /* 把窗口本体合成到阴影层上。 */
    for (int y = 0; y < WIN_H; ++y) {
        for (int x = 0; x < WIN_W; ++x) {
            BYTE ba = body_alpha_at(x, y);
            if (!ba) continue;
            int lx = SHADOW_PAD + x, ly = SHADOW_PAD + y;
            SIZE_T li = (SIZE_T)ly * LAYER_W + lx;
            DWORD src = contentBits[(SIZE_T)y * WIN_W + x];
            unsigned int sb = src & 0xFFu;
            unsigned int sg = (src >> 8) & 0xFFu;
            unsigned int sr = (src >> 16) & 0xFFu;
            unsigned int sa = (layerBits[li] >> 24) & 0xFFu;
            unsigned int oa = (unsigned int)ba + (sa * (255u - (unsigned int)ba) + 127u) / 255u;
            unsigned int ob = (sb * (unsigned int)ba + 127u) / 255u;
            unsigned int og = (sg * (unsigned int)ba + 127u) / 255u;
            unsigned int orr = (sr * (unsigned int)ba + 127u) / 255u;
            layerBits[li] = (DWORD)((oa << 24) | (orr << 16) | (og << 8) | ob);
        }
    }

    RECT wr; GetClientRect(hwnd, &wr);
    (void)wr;
    POINT dst = {0,0};
    /* 取当前窗口左上角的屏幕坐标。 */
    ScreenToClient(hwnd, &dst); /* 下面重新计算 */
    POINT origin = {0,0};
    /* 不额外引入 ClientToScreen，反算一次即可得到屏幕坐标。 */
    POINT screenZero = {0,0};
    ScreenToClient(hwnd, &screenZero);
    dst.x = -screenZero.x; dst.y = -screenZero.y;

    SIZE size = {LAYER_W, LAYER_H}; POINT srcPt = {0,0};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    BOOL ok = pUpdateLayeredWindow(hwnd, NULL, &dst, &size, layerDC, &srcPt, 0, &blend, ULW_ALPHA);

    HeapFree(heap,0,ambientSrc); HeapFree(heap,0,dropSrc); HeapFree(heap,0,tmp);
    HeapFree(heap,0,ambientBlur); HeapFree(heap,0,dropBlur);
    SelectObject(contentDC, oldContent); SelectObject(layerDC, oldLayer);
    DeleteObject(contentBmp); DeleteObject(layerBmp); DeleteDC(contentDC); DeleteDC(layerDC);
    FreeLibrary(gdi); FreeLibrary(user);
    return ok ? 1 : 0;
}

static void browse_for_app(HWND hwnd) {
    wchar_t path[PATH_CAP] = L"";
    OPENFILENAMEW ofn; ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hwnd; ofn.lpstrFile = path;
    ofn.nMaxFile = PATH_CAP;
    ofn.lpstrFilter = L"应用程序 (*.exe;*.bat;*.cmd)\0*.exe;*.bat;*.cmd\0所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrTitle = L"在电脑上选择应用";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    g_allowDeactivate = 1;
    BOOL picked = GetOpenFileNameW(&ofn);
    g_allowDeactivate = 0;
    if (picked) {
        int existing = find_app_index(path);
        if (existing >= 0) {
            /* 已经在列表里：不重复添加，直接选中并滚动定位。 */
            reveal_app_index(hwnd, existing);
        } else if (add_app(path, 1)) {
            /* 新程序仍插到列表首位，并立即选中。 */
            g_selected = 0;
            g_scroll = g_scrollTarget = 0;
            g_wheelRemainder = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
    }
}

/* ============================== 文件关联 ============================== */

static void write_string(HKEY root, const wchar_t *subkey, const wchar_t *name, const wchar_t *value) {
    HKEY key;
    if (RegCreateKeyExW(root, subkey, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value,
                       (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

static void write_empty_value(HKEY root, const wchar_t *subkey, const wchar_t *name) {
    HKEY key;
    if (RegCreateKeyExW(root, subkey, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_NONE, NULL, 0);
        RegCloseKey(key);
    }
}

static void delete_value(HKEY root, const wchar_t *subkey, const wchar_t *name) {
    HKEY key;
    if (RegOpenKeyExW(root, subkey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, name);
        RegCloseKey(key);
    }
}

static int read_string(HKEY root, const wchar_t *subkey, const wchar_t *name,
                       wchar_t *out, size_t cap) {
    HKEY key;
    DWORD type = 0, bytes = (DWORD)(cap * sizeof(wchar_t));
    LONG rc;
    if (!out || cap == 0) return 0;
    out[0] = 0;
    rc = RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE, &key);
    if (rc != ERROR_SUCCESS) return 0;
    rc = RegQueryValueExW(key, name, NULL, &type, (LPBYTE)out, &bytes);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        out[0] = 0;
        return 0;
    }
    out[cap - 1] = 0;
    return *out != 0;
}

/*
 * 读取系统原来的文件类型名称。
 * 这里优先查 HKLM，避免被本程序写入 HKCU\Software\Classes 的覆盖项干扰。
 */
static void get_original_type_name(wchar_t *out, size_t cap) {
    wchar_t extKey[128], progid[128], classKey[192], typeName[256];
    safe_copy(out, cap, L"");
    _snwprintf(extKey, 127, L"Software\\Classes\\%s", g_extension);
    extKey[127] = 0;

    if (!read_string(HKEY_LOCAL_MACHINE, extKey, NULL, progid, 128)) {
        progid[0] = 0;
    }
    if (!*progid || !_wcsnicmp(progid, L"GPE.", 4)) {
        /* HKLM 没有默认 ProgID 时，再从 OpenWithProgids 里找一个可用的。 */
        wchar_t openWithKey[160];
        HKEY key;
        _snwprintf(openWithKey, 159, L"%s\\OpenWithProgids", extKey);
        openWithKey[159] = 0;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, openWithKey, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
            DWORD count = 0, maxName = 0;
            if (RegQueryInfoKeyW(key, NULL, NULL, NULL, NULL, NULL, NULL, &count,
                                 &maxName, NULL, NULL, NULL) == ERROR_SUCCESS && count) {
                wchar_t *name = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                     (maxName + 2) * sizeof(wchar_t));
                if (name) {
                    DWORD nameChars = maxName + 1;
                    if (RegEnumValueW(key, 0, name, &nameChars, NULL, NULL, NULL, NULL) == ERROR_SUCCESS)
                        safe_copy(progid, 128, name);
                    HeapFree(GetProcessHeap(), 0, name);
                }
            }
            RegCloseKey(key);
        }
    }

    if (*progid && _wcsnicmp(progid, L"GPE.", 4)) {
        _snwprintf(classKey, 191, L"Software\\Classes\\%s", progid);
        classKey[191] = 0;
        if (read_string(HKEY_LOCAL_MACHINE, classKey, NULL, typeName, 256)) {
            safe_copy(out, cap, typeName);
            return;
        }
        if (read_string(HKEY_CLASSES_ROOT, progid, NULL, typeName, 256)) {
            safe_copy(out, cap, typeName);
            return;
        }
    }
}

static void delete_tree(HKEY root, const wchar_t *subkey) {
    RegDeleteTreeW(root, subkey);
}

static int progid_uses_exe(const wchar_t *progid, const wchar_t *exe) {
    wchar_t key[320], command[PATH_CAP * 2], commandExe[PATH_CAP];
    _snwprintf(key, 319, L"%s\\shell\\open\\command", progid);
    key[319] = 0;
    if (!read_string(HKEY_CLASSES_ROOT, key, NULL, command,
                     sizeof(command) / sizeof(command[0]))) return 0;
    commandExe[0] = 0;
    parse_command_path(command, commandExe, PATH_CAP);
    return *commandExe && _wcsicmp(commandExe, exe) == 0;
}

/* 优先复用应用自己注册的 ProgID，这样图标和文件类型名称都能跟着应用走。 */
static int find_registered_progid(const wchar_t *exe, wchar_t *out, size_t cap) {
    wchar_t current[256], keyPath[256];
    HKEY key = NULL;
    safe_copy(out, cap, L"");

    if (read_string(HKEY_CLASSES_ROOT, g_extension, NULL, current, 256) &&
        progid_uses_exe(current, exe)) {
        safe_copy(out, cap, current);
        return 1;
    }

    _snwprintf(keyPath, 255, L"%s\\OpenWithProgids", g_extension);
    keyPath[255] = 0;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, keyPath, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        for (DWORD index = 0;; ++index) {
            wchar_t name[256];
            DWORD chars = (DWORD)(sizeof(name) / sizeof(name[0]));
            LONG rc = RegEnumValueW(key, index, name, &chars, NULL, NULL, NULL, NULL);
            if (rc == ERROR_NO_MORE_ITEMS) break;
            if (rc != ERROR_SUCCESS || !*name) continue;
            if (progid_uses_exe(name, exe)) {
                safe_copy(out, cap, name);
                RegCloseKey(key);
                return 1;
            }
        }
        RegCloseKey(key);
    }
    return 0;
}

static void register_application(const wchar_t *exe, const wchar_t *appName,
                                 const wchar_t *appFriendlyName,
                                 const wchar_t *command) {
    wchar_t appKey[320], appCommandKey[360], supportedKey[360], iconKey[360];
    wchar_t userAppKey[360], userAppCommandKey[400], userSupportedKey[400], userIconKey[400];
    wchar_t iconValue[PATH_CAP * 2];

    _snwprintf(iconValue, PATH_CAP * 2 - 1, L"\"%s\",0", exe);
    iconValue[PATH_CAP * 2 - 1] = 0;

    /* 绿色软件没有自己的 ProgID 时，使用 Shell 能识别的 Applications\\xxx.exe。 */
    _snwprintf(appKey, 319, L"Applications\\%s", appName); appKey[319] = 0;
    _snwprintf(appCommandKey, 359, L"%s\\shell\\open\\command", appKey); appCommandKey[359] = 0;
    _snwprintf(supportedKey, 359, L"%s\\SupportedTypes", appKey); supportedKey[359] = 0;
    _snwprintf(iconKey, 359, L"%s\\DefaultIcon", appKey); iconKey[359] = 0;
    write_string(HKEY_CLASSES_ROOT, appKey, NULL, appFriendlyName);
    write_string(HKEY_CLASSES_ROOT, appKey, L"FriendlyAppName", appFriendlyName);
    write_string(HKEY_CLASSES_ROOT, appKey, L"FriendlyTypeName", appFriendlyName);
    write_string(HKEY_CLASSES_ROOT, appCommandKey, NULL, command);
    write_string(HKEY_CLASSES_ROOT, iconKey, NULL, iconValue);
    write_empty_value(HKEY_CLASSES_ROOT, supportedKey, g_extension);

    _snwprintf(userAppKey, 359, L"Software\\Classes\\Applications\\%s", appName); userAppKey[359] = 0;
    _snwprintf(userAppCommandKey, 399, L"%s\\shell\\open\\command", userAppKey); userAppCommandKey[399] = 0;
    _snwprintf(userSupportedKey, 399, L"%s\\SupportedTypes", userAppKey); userSupportedKey[399] = 0;
    _snwprintf(userIconKey, 399, L"%s\\DefaultIcon", userAppKey); userIconKey[399] = 0;
    write_string(HKEY_CURRENT_USER, userAppKey, NULL, appFriendlyName);
    write_string(HKEY_CURRENT_USER, userAppKey, L"FriendlyAppName", appFriendlyName);
    write_string(HKEY_CURRENT_USER, userAppKey, L"FriendlyTypeName", appFriendlyName);
    write_string(HKEY_CURRENT_USER, userAppCommandKey, NULL, command);
    write_string(HKEY_CURRENT_USER, userIconKey, NULL, iconValue);
    write_empty_value(HKEY_CURRENT_USER, userSupportedKey, g_extension);
}

static void set_explorer_openwith(const wchar_t *appName, const wchar_t *progid) {
    wchar_t listKey[320], progidsKey[320], userChoiceKey[320];
    HKEY key;

    _snwprintf(listKey, 319,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\%s\\OpenWithList",
        g_extension);
    listKey[319] = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, listKey, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
        /* PE 里保留一个明确的最近使用项即可。 */
        RegSetValueExW(key, L"a", 0, REG_SZ, (const BYTE *)appName,
                       (DWORD)((wcslen(appName) + 1) * sizeof(wchar_t)));
        RegSetValueExW(key, L"MRUList", 0, REG_SZ, (const BYTE *)L"a",
                       (DWORD)(2 * sizeof(wchar_t)));
        RegCloseKey(key);
    }

    _snwprintf(progidsKey, 319,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\%s\\OpenWithProgids",
        g_extension);
    progidsKey[319] = 0;
    write_empty_value(HKEY_CURRENT_USER, progidsKey, progid);

    /* WinPE 没有桌面系统那套“默认应用”流程，直接写入所选 ProgID。 */
    _snwprintf(userChoiceKey, 319,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\%s\\UserChoice",
        g_extension);
    userChoiceKey[319] = 0;
    delete_tree(HKEY_CURRENT_USER, userChoiceKey);
    write_string(HKEY_CURRENT_USER, userChoiceKey, L"ProgId", progid);
}

/* “始终”按钮最终走这里：注册应用、写扩展名关联并通知 Explorer 刷新。 */
static void set_association(const wchar_t *exe) {
    if (!has_extension()) return;

    wchar_t progid[320], command[PATH_CAP * 2];
    wchar_t userExtKey[160], classOpenWith[384];
    wchar_t appName[260], appFriendlyName[256];
    wchar_t oldGpe[128], oldGpeUser[320];

    basename_file(exe, appName, 260);
    get_file_description(exe, appFriendlyName, 256);
    if (!*appFriendlyName) safe_copy(appFriendlyName, 256, appName);

    _snwprintf(command, PATH_CAP * 2 - 1, L"\"%s\" \"%%1\"", exe);
    command[PATH_CAP * 2 - 1] = 0;

    register_application(exe, appName, appFriendlyName, command);

    /* 有应用自己的 ProgID 就直接用；绿色软件则退回 Applications\\xxx.exe。 */
    if (!find_registered_progid(exe, progid, 320)) {
        _snwprintf(progid, 319, L"Applications\\%s", appName);
        progid[319] = 0;
    }

    /* WinPE Explorer 使用的经典扩展名关联。 */
    write_string(HKEY_CLASSES_ROOT, g_extension, NULL, progid);
    _snwprintf(userExtKey, 159, L"Software\\Classes\\%s", g_extension);
    userExtKey[159] = 0;
    write_string(HKEY_CURRENT_USER, userExtKey, NULL, progid);

    _snwprintf(classOpenWith, 383, L"%s\\OpenWithProgids", g_extension);
    classOpenWith[383] = 0;
    write_empty_value(HKEY_CLASSES_ROOT, classOpenWith, progid);

    set_explorer_openwith(appName, progid);

    /* 顺手清理旧版本遗留的 GPE.*.AutoFile。 */
    if (g_extension[0] == L'.') {
        _snwprintf(oldGpe, 127, L"GPE.%s.AutoFile", g_extension + 1);
        oldGpe[127] = 0;
        if (_wcsicmp(oldGpe, progid) != 0) {
            delete_tree(HKEY_CLASSES_ROOT, oldGpe);
            _snwprintf(oldGpeUser, 319, L"Software\\Classes\\%s", oldGpe);
            oldGpeUser[319] = 0;
            delete_tree(HKEY_CURRENT_USER, oldGpeUser);
        }
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, NULL, NULL);
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSH, g_file, NULL);
}

static void execute_selected(int always) {
    if (g_selected < 0 || g_selected >= g_count) return;
    wchar_t params[PATH_CAP + 4];
    _snwprintf(params, PATH_CAP + 3, L"\"%s\"", g_file); params[PATH_CAP + 3] = 0;

    /* 再兜底一次：没有扩展名时绝不写永久关联。 */
    if (always && has_extension()) set_association(g_apps[g_selected].path);

    ShellExecuteW(g_hwnd, L"open", g_apps[g_selected].path, params, NULL, SW_SHOWNORMAL);
    DestroyWindow(g_hwnd);
}

/* ============================== 窗口消息 ============================== */

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        apply_theme(hwnd);
        return 0;
    case WM_NCCALCSIZE:
        if (wp) return 0;
        break;
    case WM_NCHITTEST: {
        POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
        ScreenToClient(hwnd, &p);
        int bx = (int)p.x - SHADOW_PAD, by = (int)p.y - SHADOW_PAD;
        if (!body_alpha_at(bx, by)) return HTNOWHERE;
        if (by < PX(65)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps);
        render_layered_window(hwnd); return 0;
    }
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&t);
        int bx = GET_X_LPARAM(lp) - SHADOW_PAD;
        int by = GET_Y_LPARAM(lp) - SHADOW_PAD;
        if (g_scrollDragging) {
            stop_scroll_timer(hwnd);
            set_scroll_from_thumb_top(by - g_scrollDragOffset);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        int hit = hit_test(bx, by);
        if (hit != g_hover) { g_hover = hit; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hover = HIT_NONE; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && (g_hover == HIT_BROWSE || g_hover == HIT_ALWAYS || g_hover == HIT_ONCE ||
            (g_hover >= 0 && g_hover < g_count))) {
            SetCursor(LoadCursorW(NULL, IDC_HAND)); return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        int bx = GET_X_LPARAM(lp) - SHADOW_PAD;
        int by = GET_Y_LPARAM(lp) - SHADOW_PAD;
        RECT track, thumb;
        if (get_scrollbar_geometry(&track, &thumb) && point_in_scrollbar_hitbox(bx, by, 0)) {
            stop_scroll_timer(hwnd);
            g_scrollTarget = g_scroll;
            g_wheelRemainder = 0;
            if (point_in_scrollbar_hitbox(bx, by, 1)) {
                g_scrollDragging = 1;
                g_scrollDragOffset = by - thumb.top;
            } else {
                /* 点轨道空白处时先把滑块跳到鼠标位置，然后直接进入拖动。 */
                int thumbH = thumb.bottom - thumb.top;
                set_scroll_from_thumb_top(by - thumbH / 2);
                get_scrollbar_geometry(&track, &thumb);
                g_scrollDragging = 1;
                g_scrollDragOffset = by - thumb.top;
            }
            g_pressed = HIT_NONE;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        g_pressed = hit_test(bx, by);
        SetCapture(hwnd); InvalidateRect(hwnd, NULL, FALSE); return 0;
    }
    case WM_LBUTTONUP: {
        if (g_scrollDragging) {
            g_scrollDragging = 0;
            g_scrollDragOffset = 0;
            g_pressed = HIT_NONE;
            ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        int hit = hit_test(GET_X_LPARAM(lp) - SHADOW_PAD, GET_Y_LPARAM(lp) - SHADOW_PAD);
        int pressed = g_pressed; g_pressed = HIT_NONE; ReleaseCapture();
        if (hit == pressed) {
            if (hit == HIT_BROWSE) browse_for_app(hwnd);
            else if (hit == HIT_ALWAYS) execute_selected(1);
            else if (hit == HIT_ONCE) execute_selected(0);
            else if (hit >= 0 && hit < g_count) g_selected = hit;
        }
        InvalidateRect(hwnd, NULL, FALSE); return 0;
    }
    case WM_LBUTTONDBLCLK: {
        int hit = hit_test(GET_X_LPARAM(lp) - SHADOW_PAD, GET_Y_LPARAM(lp) - SHADOW_PAD);
        if (hit >= 0 && hit < g_count) { g_selected = hit; execute_selected(0); }
        return 0;
    }
    case WM_CAPTURECHANGED:
        g_scrollDragging = 0; g_scrollDragOffset = 0; g_pressed = HIT_NONE;
        g_scrollTarget = g_scroll;
        return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        /* 支持高精度滚轮 delta；每个标准刻度只推进约 38px，不再整行跳。 */
        g_wheelRemainder += -delta * PX(38);
        int move = g_wheelRemainder / WHEEL_DELTA;
        g_wheelRemainder -= move * WHEEL_DELTA;
        if (move != 0) {
            g_scrollTarget = clamp_scroll_pos(g_scrollTarget + move);
            g_hover = HIT_NONE;
            start_scroll_timer(hwnd);
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == SCROLL_TIMER_ID) {
            int diff = g_scrollTarget - g_scroll;
            if (diff == 0) {
                stop_scroll_timer(hwnd);
                return 0;
            }
            /* 15ms 一帧的 ease-out，列表按像素连续移动。 */
            int step = diff / 4;
            if (step == 0) step = diff > 0 ? 1 : -1;
            if ((step > 0 && step > diff) || (step < 0 && step < diff)) step = diff;
            g_scroll = clamp_scroll_pos(g_scroll + step);
            if ((diff > 0 && g_scroll >= g_scrollTarget) ||
                (diff < 0 && g_scroll <= g_scrollTarget)) g_scroll = g_scrollTarget;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        UINT newDpi = (UINT)LOWORD(wp);
        if (newDpi < 96 || newDpi > 768) newDpi = 96;
        if (newDpi != g_dpi) {
            RECT suggested = *(RECT *)lp;
            UINT oldDpi = g_dpi ? g_dpi : 96;
            int oldScroll = g_scroll;
            int oldTarget = g_scrollTarget;
            g_dpi = newDpi;
            g_scroll = clamp_scroll_pos((oldScroll * (int)newDpi + (int)oldDpi / 2) / (int)oldDpi);
            g_scrollTarget = clamp_scroll_pos((oldTarget * (int)newDpi + (int)oldDpi / 2) / (int)oldDpi);
            g_scrollDragging = 0;
            g_scrollDragOffset = 0;
            stop_scroll_timer(hwnd);
            recreate_fonts();
            resize_for_dpi(hwnd, &suggested);
            InvalidateRect(hwnd, NULL, FALSE);
            render_layered_window(hwnd);
        }
        return 0;
    }
    case WM_SETTINGCHANGE:
        refresh_window_shadow(hwnd);
        apply_theme(hwnd); return 0;
    case WM_THEMECHANGED:
        apply_theme(hwnd); return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && !g_allowDeactivate) DestroyWindow(hwnd);
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE) { DestroyWindow(hwnd); return 0; }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        else if (wp == VK_RETURN && g_selected >= 0) execute_selected(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}


/* ============================== DPI ============================== */

/*
 * 新版 WinPE 如果带有 Win10/11 的 user32，会优先进入 Per-Monitor V2。
 * 老 PE 没有这些导出时自动退回 SetProcessDPIAware；整个过程都用动态取址，
 * 不会因为精简 PE 缺少新 API 而导致程序无法启动。
 */
static void init_dpi_awareness(void) {
    HMODULE user = LoadLibraryW(L"user32.dll");
    if (user) {
        typedef BOOL (WINAPI *PFN_SET_PROCESS_DPI_AWARENESS_CONTEXT)(HANDLE);
        PFN_SET_PROCESS_DPI_AWARENESS_CONTEXT setContext =
            (PFN_SET_PROCESS_DPI_AWARENESS_CONTEXT)GetProcAddress(user, "SetProcessDpiAwarenessContext");
        if (setContext) {
            /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = (HANDLE)-4 */
            if (setContext((HANDLE)(LONG_PTR)-4)) {
                FreeLibrary(user);
                return;
            }
        }
        FreeLibrary(user);
    }
    SetProcessDPIAware();
}

static UINT query_system_dpi(void) {
    UINT dpi = 96;
    HMODULE user = LoadLibraryW(L"user32.dll");
    if (user) {
        typedef UINT (WINAPI *PFN_GET_DPI_FOR_SYSTEM)(void);
        PFN_GET_DPI_FOR_SYSTEM getDpi =
            (PFN_GET_DPI_FOR_SYSTEM)GetProcAddress(user, "GetDpiForSystem");
        if (getDpi) {
            UINT value = getDpi();
            if (value >= 96 && value <= 768) dpi = value;
        } else {
            /* 老版 PE 没有 GetDpiForSystem，就从屏幕 DC 读取 LOGPIXELSX。 */
            typedef HDC (WINAPI *PFN_GET_DC)(HWND);
            typedef int (WINAPI *PFN_RELEASE_DC)(HWND, HDC);
            HMODULE gdi = LoadLibraryW(L"gdi32.dll");
            PFN_GET_DC getDc = (PFN_GET_DC)GetProcAddress(user, "GetDC");
            PFN_RELEASE_DC releaseDc = (PFN_RELEASE_DC)GetProcAddress(user, "ReleaseDC");
            if (gdi && getDc && releaseDc) {
                typedef int (WINAPI *PFN_GET_DEVICE_CAPS)(HDC, int);
                PFN_GET_DEVICE_CAPS getCaps =
                    (PFN_GET_DEVICE_CAPS)GetProcAddress(gdi, "GetDeviceCaps");
                if (getCaps) {
                    HDC dc = getDc(NULL);
                    if (dc) {
                        int value = getCaps(dc, 88); /* LOGPIXELSX */
                        if (value >= 96 && value <= 768) dpi = (UINT)value;
                        releaseDc(NULL, dc);
                    }
                }
            }
            if (gdi) FreeLibrary(gdi);
        }
        FreeLibrary(user);
    }
    return dpi;
}

static void recreate_fonts(void) {
    if (g_titleFont) { DeleteObject(g_titleFont); g_titleFont = NULL; }
    if (g_textFont)  { DeleteObject(g_textFont);  g_textFont = NULL; }
    if (g_linkFont)  { DeleteObject(g_linkFont);  g_linkFont = NULL; }

    g_titleFont = CreateFontW(-PX(18), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_textFont = CreateFontW(-PX(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_linkFont = CreateFontW(-PX(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

static void resize_for_dpi(HWND hwnd, const RECT *suggested) {
    HMODULE user = LoadLibraryW(L"user32.dll");
    if (!user) return;
    typedef BOOL (WINAPI *PFN_SET_WINDOW_POS)(HWND, HWND, int, int, int, int, UINT);
    PFN_SET_WINDOW_POS setWindowPos =
        (PFN_SET_WINDOW_POS)GetProcAddress(user, "SetWindowPos");
    if (setWindowPos) {
        int x = suggested ? suggested->left : 0;
        int y = suggested ? suggested->top : 0;
        setWindowPos(hwnd, NULL, x, y, LAYER_W, LAYER_H, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    FreeLibrary(user);
}

/* ============================== 安装与入口 ============================== */

/* /i：复制到 System32\OpenWith.exe，并接管 Unknown/Undecided 的打开命令。 */
static int install_self(void) {
    wchar_t self[PATH_CAP], systemDir[PATH_CAP], target[PATH_CAP], command[PATH_CAP * 2];
    GetModuleFileNameW(NULL, self, PATH_CAP);
    GetSystemDirectoryW(systemDir, PATH_CAP);
    _snwprintf(target, PATH_CAP - 1, L"%s\\OpenWith.exe", systemDir); target[PATH_CAP - 1] = 0;
    if (_wcsicmp(self, target) != 0 && !CopyFileW(self, target, FALSE)) return 1;
    _snwprintf(command, PATH_CAP * 2 - 1, L"\"%s\" \"%%1\"", target); command[PATH_CAP * 2 - 1] = 0;
    write_string(HKEY_CURRENT_USER, L"Software\\Classes\\Unknown\\shell\\openas\\command", NULL, command);
    write_string(HKEY_CURRENT_USER, L"Software\\Classes\\Undecided\\shell\\open\\command", NULL, command);
    return 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR commandLine, int show) {
    (void)previous; (void)show;
    init_dpi_awareness();
    g_dpi = query_system_dpi();
    g_shadowEnabled = query_window_shadow_enabled();
    g_osBuild = query_os_build();
    g_useRounded = g_osBuild >= 22000;
    int argc = 0; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 2) return 0;
    if (_wcsicmp(argv[1], L"/i") == 0) { LocalFree(argv); return install_self(); }
    safe_copy(g_file, PATH_CAP, argv[1]); LocalFree(argv);
    if (!path_exists(g_file)) return 0;
    get_extension(); enum_apps();

    recreate_fonts();

    WNDCLASSEXW wc; ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = wndproc; wc.hInstance = instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.hbrBackground = NULL;
    wc.lpszClassName = L"GPE.OpenWith.Native";
    RegisterClassExW(&wc);

    int x = (GetSystemMetrics(SM_CXSCREEN) - WIN_W) / 2 - SHADOW_PAD;
    int y = (GetSystemMetrics(SM_CYSCREEN) - WIN_H) / 2 - SHADOW_PAD;
    /* ToolWindow 不进入任务栏；窗口本体仍保持无边框分层绘制。 */
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, wc.lpszClassName, L"打开方式",
        WS_POPUP,
        x, y, LAYER_W, LAYER_H, NULL, NULL, instance, NULL);
    if (!g_hwnd) return 2;
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    render_layered_window(g_hwnd);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }

    for (int i = 0; i < g_count; ++i) if (g_apps[i].icon) DestroyIcon(g_apps[i].icon);
    DeleteObject(g_titleFont); DeleteObject(g_textFont); DeleteObject(g_linkFont);
    return (int)msg.wParam;
}

/* 无 CRT 启动入口：保持精简 WinPE 中只依赖系统 DLL。 */
void mainCRTStartup(void) {
    int code = wWinMain(GetModuleHandleW(NULL), NULL, GetCommandLineW(), SW_SHOWNORMAL);
    ExitProcess((UINT)code);
}
