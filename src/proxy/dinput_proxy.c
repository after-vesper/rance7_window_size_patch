/*
 * dinput.dll proxy for Sengoku Rance (System40) multi-resolution patch.
 *
 * The windowed renderer of this game is GDI-based: the engine draws the
 * 800x600 main surface into a DIB section and blits it to the window DC
 * with BitBlt. This proxy forwards DirectInput exports to the real
 * dinput.dll and patches the exe's IAT entry for GDI32!BitBlt so that
 * the final blit is stretched to the configured output size.
 * The game window is resized to match.
 *
 * Config: WindowSize.ini next to the game exe
 *   [Display]
 *   Width  = 1600
 *   Height = 1200
 *   Filter = 0    ; 0 = nearest (COLORONCOLOR / point),
 *                 ; 1 = smooth (HALFTONE / bilinear)
 *   ScaleMode = 1 ; 0 = stretch, 1 = keep 4:3 (fractional scale),
 *                 ; 2 = largest integer scale (avoids jitter)
 *   GpuPresent = 1 ; 1 = D3D9Ex GPU present (scaling is free),
 *                  ; 0 = GDI StretchBlt software path
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>
#include <d3d9.h>

static int strncmp_l(const char *x, const char *y, int n)
{
    while (n-- > 0 && *x && *x == *y) { x++; y++; }
    return n < 0 ? 0 : (unsigned char)*x - (unsigned char)*y;
}

/* CRT 無しビルドのため最低限のランタイムを自前提供する */
int _fltused = 1;
void *memcpy(void *d, const void *s, size_t n)
{
    DWORD *dd = (DWORD *)d;
    const DWORD *ss = (const DWORD *)s;
    size_t q = n / 4;
    while (q--)
        *dd++ = *ss++;
    {
        BYTE *db = (BYTE *)dd;
        const BYTE *sb = (const BYTE *)ss;
        for (n &= 3; n--; )
            *db++ = *sb++;
    }
    return d;
}

static int g_viewW = 800;
static int g_viewH = 600;
#define GAME_W g_viewW
#define GAME_H g_viewH

static HMODULE g_dinput;
static HINSTANCE g_hInst;
static char g_iniPath[MAX_PATH];
static int g_enabled;
static int g_outW, g_outH, g_smooth, g_mode, g_full;
static HWND g_hwnd;
static int g_hooked;
static int g_borderless_applied;
static int g_appliedW, g_appliedH;
static int g_sizing;
static int g_log;
static int g_gpu;               /* [Display] GpuPresent */
/* HD バックバッファ実験用 */
static int g_renderscale;
static HDC g_dib_dc;
static void *g_dib_bits;
static DWORD g_dib_pitch;
static DWORD *g_bb_surf;

static void logfmt(const char *fmt, ...);
static const char *mod_name_of(const void *addr, char *buf, DWORD bufsz);

typedef BOOL (WINAPI *PFN_BitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);
typedef BOOL (WINAPI *PFN_StretchBlt)(HDC, int, int, int, int, HDC,
                                      int, int, int, int, DWORD);
typedef BOOL (WINAPI *PFN_ScreenToClient)(HWND, LPPOINT);
typedef BOOL (WINAPI *PFN_SetCursorPos)(int, int);
typedef BOOL (WINAPI *PFN_GetCursorPos)(LPPOINT);
typedef BOOL (WINAPI *PFN_ClientToScreen)(HWND, LPPOINT);
static PFN_BitBlt g_origBitBlt;
static PFN_StretchBlt g_origStretchBlt;
static PFN_ScreenToClient g_origScreenToClient;
static PFN_SetCursorPos g_origSetCursorPos;
static PFN_GetCursorPos g_origGetCursorPos;
static PFN_ClientToScreen g_origClientToScreen;

static void logmsg(const char *s)
{
    HANDLE f;
    if (!g_log)
        return;
    f = CreateFileA("windowsize_log.txt", GENERIC_WRITE, 0, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD n;
    if (f == INVALID_HANDLE_VALUE) return;
    SetFilePointer(f, 0, NULL, FILE_END);
    WriteFile(f, s, lstrlenA(s), &n, NULL);
    WriteFile(f, "\r\n", 2, &n, NULL);
    CloseHandle(f);
}

#include <stdarg.h>
static void logfmt(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(buf, fmt, ap);
    va_end(ap);
    logmsg(buf);
}

static void load_config(void)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    while (n && path[n - 1] != '\\') n--;
    path[n] = 0;
    /* Prefer WindowSize.ini; fall back to the old MultiRes.ini name so
       installs made by pre-rename versions keep working. */
    lstrcatA(path, "WindowSize.ini");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        char old[MAX_PATH];
        lstrcpyA(old, path);
        old[lstrlenA(old) - lstrlenA("WindowSize.ini")] = 0;
        lstrcatA(old, "MultiRes.ini");
        if (GetFileAttributesA(old) != INVALID_FILE_ATTRIBUTES)
            lstrcpyA(path, old);
    }
    lstrcpyA(g_iniPath, path);
    g_outW = GetPrivateProfileIntA("Display", "Width", GAME_W, path);
    g_outH = GetPrivateProfileIntA("Display", "Height", GAME_H, path);
    g_smooth = GetPrivateProfileIntA("Display", "Filter", 0, path);
    g_mode = GetPrivateProfileIntA("Display", "ScaleMode", 1, path);
    g_full = GetPrivateProfileIntA("Display", "Fullscreen", 0, path);
    g_log = GetPrivateProfileIntA("Debug", "Log", 0, path);
    g_gpu = GetPrivateProfileIntA("Display", "GpuPresent", 1, path);
    /* HD 実験: バックバッファだけを N 倍 DIB に差し替える。
       座標変換は行わず、まずエンジンの動作を観察する。 */
    g_renderscale = GetPrivateProfileIntA("HD", "RenderScale", 0, path);
    /* System40.ini の ViewWidth/Height が書き換えられている場合は
       エンジンがネイティブにそのサイズで合成する。それに合わせて
       present 基準サイズを更新する。 */
    {
        char s40[MAX_PATH];
        int vw_unused; (void)vw_unused;
        lstrcpyA(s40, path);
        {
            char *sep = s40 + lstrlenA(s40);
            while (sep > s40 && sep[-1] != '\\') sep--;
            if (sep > s40)
                lstrcpyA(sep, "System40.ini");
        }
        {
            HANDLE hf = CreateFileA(s40, GENERIC_READ,
                                    FILE_SHARE_READ, NULL,
                                    OPEN_EXISTING, 0, NULL);
            if (hf != INVALID_HANDLE_VALUE) {
                char buf[2048];
                DWORD n = 0;
                ReadFile(hf, buf, sizeof(buf) - 1, &n, NULL);
                CloseHandle(hf);
                buf[n] = 0;
                {
                    int a = 0, b = 0;
                    char *q;
                    for (q = buf; q[0]; q++) {
                        if (!strncmp_l(q, "ViewWidth", 9) ||
                            !strncmp_l(q, "ViewHeight", 10)) {
                            int *t = (q[4] == 'W') ? &a : &b;
                            while (*q && *q != '=') q++;
                            if (*q) q++;
                            while (*q == ' ' || *q == '	') q++;
                            while (*q >= '0' && *q <= '9')
                                *t = *t * 10 + *q++ - '0';
                        }
                    }
                    if (a > 0 && b > 0) {
                        g_viewW = a;
                        g_viewH = b;
                    }
                }
            }
        }
    }
    /* Hooks stay enabled even at 800x600: the path degenerates to a
       1:1 transfer identical to the original behavior, and keeping it
       on avoids a special "disabled" state. */
    g_enabled = 1;
}

/* --- window management -------------------------------------------------- */

static BOOL CALLBACK find_game_window(HWND hwnd, LPARAM lp)
{
    char cls[64];
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd))
        return TRUE;
    if (GetClassNameA(hwnd, cls, sizeof(cls)) &&
        lstrcmpA(cls, "Sys40WindowClass") == 0) {
        *(HWND *)lp = hwnd;
        return FALSE;
    }
    return TRUE;
}

/* Viewport: the rect inside the client area that receives the scaled
   800x600 frame. ScaleMode: 0=stretch to client, 1=largest 4:3 rect
   (fractional scale), 2=largest integer-scale 4:3 rect (no fraction
   jitter but possibly small). All logical<->client conversions go
   through this rect. */
static void get_viewport(RECT *vp)
{
    RECT rc;
    int w, h;
    GetClientRect(g_hwnd, &rc);
    if (g_mode == 0) {          /* stretch */
        *vp = rc;
        return;
    }
    if (g_mode == 2) {          /* integer scale */
        int sx = rc.right / GAME_W;
        int sy = rc.bottom / GAME_H;
        int s = sx < sy ? sx : sy;
        if (s < 1) s = 1;
        w = GAME_W * s;
        h = GAME_H * s;
    } else if (rc.right * GAME_H > rc.bottom * GAME_W) {
        /* client wider than 4:3: pillarbox */
        h = rc.bottom;
        w = MulDiv(rc.bottom, GAME_W, GAME_H);
    } else {
        /* client taller than 4:3: letterbox */
        w = rc.right;
        h = MulDiv(rc.right, GAME_H, GAME_W);
    }
    vp->left = (rc.right - w) / 2;
    vp->top = (rc.bottom - h) / 2;
    vp->right = vp->left + w;
    vp->bottom = vp->top + h;
}

static void ensure_window_size(void);
static void save_resolution(void);
static void show_resolution_dialog(void);

/* Subclass the game window so mouse-message client coords arrive in the
   800x600 logical space. Without this the game reads raw client coords
   (e.g. as the start point of cursor warps), mixing coordinate spaces. */
static WNDPROC g_origWndProc;

/* Menu command id used for our borderless fullscreen entry, appended
   to the System submenu. Chosen high to avoid colliding with the
   game's own command ids (they reach ~40100). */
#define CMD_BORDERLESS 40200
#define CMD_RESOLUTION 40201

/* True when the engine's own exclusive fullscreen is active and we
   did not apply the borderless style. The engine strips the caption
   and shrinks the window to the 800x600 display mode it sets; a mere
   caption-less window sized like a monitor (e.g. the state the engine
   restores after leaving fullscreen while we were borderless) is not
   native fullscreen. While native is active every hook passes
   through so the engine renders undisturbed. */
static int native_fullscreen(void)
{
    RECT r;
    if (!g_hwnd || g_borderless_applied)
        return 0;
    if (GetWindowLongA(g_hwnd, GWL_STYLE) & WS_CAPTION)
        return 0;
    GetWindowRect(g_hwnd, &r);
    return r.left <= 0 && r.top <= 0 &&
           r.right - r.left <= 1024 && r.bottom - r.top <= 768;
}

static LRESULT CALLBACK GameWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    {
        /* Manual resizing: the window gets WS_THICKFRAME at injection
           time. While the user drags the frame we adopt the live
           client size as the output resolution so ensure_window_size
           does not snap it back, and persist on WM_EXITSIZEMOVE. */
        if (msg == WM_ENTERSIZEMOVE)
            g_sizing = 1;
        else if (msg == WM_SIZE && g_sizing && !g_full &&
                 !native_fullscreen()) {
            g_outW = (short)LOWORD(lp);
            g_outH = (short)HIWORD(lp);
        } else if (msg == WM_EXITSIZEMOVE) {
            if (g_sizing)
                save_resolution();
            g_sizing = 0;
        }
        /* Resolution menu opens the picker dialog. Menu commands must
           be handled even while scaling is disabled (800x600), or the
           user could never turn it back on. */
        if (msg == WM_COMMAND && LOWORD(wp) == CMD_RESOLUTION &&
            !native_fullscreen()) {
            show_resolution_dialog();
            return 0;
        }
        /* Our own borderless menu entry / Alt+B toggles the mode both
           ways. Alt+B is also the way back while borderless (the menu
           is hidden there). */
        if ((msg == WM_COMMAND && LOWORD(wp) == CMD_BORDERLESS) ||
            (msg == WM_SYSKEYDOWN && wp == 'B')) {
            if (native_fullscreen()) {
                /* Leave the engine's fullscreen first (its toggle is
                   command 127), then enter borderless. g_full is set
                   rather than toggled so the modes stay exclusive. */
                CallWindowProcA(g_origWndProc, hwnd, WM_COMMAND,
                                127, 0);
                g_full = 1;
            } else {
                g_full = !g_full;
            }
            ensure_window_size();
            return 0;
        }
        /* Inside borderless mode the game's fullscreen accelerator
           (Alt+Enter -> command 127) is interpreted as leaving
           borderless. If the engine still engages its native
           fullscreen through a non-message path, the WM_SIZE check
           below yields control so the two never stack. */
        if (g_full &&
            ((msg == WM_COMMAND && LOWORD(wp) == 127) ||
             (msg == WM_SYSKEYDOWN && wp == VK_RETURN &&
              (lp & (1 << 29))))) {
            g_full = 0;
            ensure_window_size();
            return 0;
        }
        /* Native fullscreen taking over while we were borderless:
           the engine resizes the window to its 800x600 mode. Release
           our borderless state so only native fullscreen is active;
           the saved windowed style is restored when it exits. */
        if (msg == WM_SIZE && g_borderless_applied &&
            (LOWORD(lp) != (WORD)g_appliedW ||
             HIWORD(lp) != (WORD)g_appliedH)) {
            g_full = 0;
            g_borderless_applied = 0;
        }
    }
    if (g_enabled && !native_fullscreen()) {
        switch (msg) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        case WM_MOUSEHOVER: {
            RECT vp;
            short x, y;
            get_viewport(&vp);
            x = (short)LOWORD(lp);
            y = (short)HIWORD(lp);
            x = (short)MulDiv(x - vp.left, GAME_W, vp.right - vp.left);
            y = (short)MulDiv(y - vp.top, GAME_H, vp.bottom - vp.top);
            lp = MAKELPARAM((WORD)x, (WORD)y);
            break;
        }
        default:
            break;
        }
    }
    return CallWindowProcA(g_origWndProc, hwnd, msg, wp, lp);
}

/* Borderless fullscreen: strip the frame and cover the monitor. The
   viewport logic still applies, so a 16:9 monitor gets pillarboxed
   4:3 output. Original style/menu are saved for restore on toggle. */
static LONG g_savedStyle = -1;
static HMENU g_savedMenu;
static RECT g_savedRect;

static void apply_fullscreen_state(void)
{
    LONG st;
    RECT rc, mon;
    MONITORINFO mi;
    HMONITOR hm;
    if (!g_hwnd)
        return;
    if (g_full) {
        hm = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
        mi.cbSize = sizeof(mi);
        GetMonitorInfoA(hm, &mi);
        mon = mi.rcMonitor;
        st = GetWindowLongA(g_hwnd, GWL_STYLE);
        if (st & WS_CAPTION) {
            g_savedStyle = st;
            g_savedMenu = GetMenu(g_hwnd);
            GetWindowRect(g_hwnd, &g_savedRect);
            SetWindowLongA(g_hwnd, GWL_STYLE,
                           (st & ~(WS_CAPTION | WS_THICKFRAME |
                                   WS_SYSMENU | WS_MINIMIZEBOX |
                                   WS_MAXIMIZEBOX)) | WS_POPUP);
            if (g_savedMenu)
                SetMenu(g_hwnd, NULL);
        }
        GetWindowRect(g_hwnd, &rc);
        if (rc.left != mon.left || rc.top != mon.top ||
            rc.right != mon.right || rc.bottom != mon.bottom)
            SetWindowPos(g_hwnd, HWND_TOP, mon.left, mon.top,
                         mon.right - mon.left, mon.bottom - mon.top,
                         SWP_NOACTIVATE | SWP_FRAMECHANGED);
        g_borderless_applied = 1;
        g_appliedW = mon.right - mon.left;
        g_appliedH = mon.bottom - mon.top;
    } else if (g_savedStyle != -1) {
        /* Keep the window resizable after restoring its style. */
        SetWindowLongA(g_hwnd, GWL_STYLE, g_savedStyle | WS_THICKFRAME);
        if (g_savedMenu)
            SetMenu(g_hwnd, g_savedMenu);
        SetWindowPos(g_hwnd, NULL, g_savedRect.left, g_savedRect.top,
                     g_savedRect.right - g_savedRect.left,
                     g_savedRect.bottom - g_savedRect.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        g_savedStyle = -1;
        g_borderless_applied = 0;
    }
}

static void ensure_window_size(void)
{
    RECT rc;
    if (!g_hwnd)
        return;
    apply_fullscreen_state();
    if (g_full)
        return;
    {
        /* Skip once the client already matches the target. Compare the
           client rect rather than the window rect so an oversized
           window that the user dragged to a new position keeps its
           position instead of being recentered every frame. */
        RECT cr;
        GetClientRect(g_hwnd, &cr);
        if (cr.right - cr.left == g_outW &&
            cr.bottom - cr.top == g_outH)
            return;
    }
    rc.left = rc.top = 0;
    rc.right = g_outW;
    rc.bottom = g_outH;
    AdjustWindowRectEx(&rc, GetWindowLongA(g_hwnd, GWL_STYLE),
                       GetMenu(g_hwnd) != NULL,
                       GetWindowLongA(g_hwnd, GWL_EXSTYLE));
    /* The game centers the small 800x600 window at startup, so growing
       it with SWP_NOMOVE would leave it shifted toward the bottom
       right. Recenter the resized window on its monitor's work area
       instead. */
    {
        HMONITOR hm = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        RECT wa;
        int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
        mi.cbSize = sizeof(mi);
        GetMonitorInfoA(hm, &mi);
        wa = mi.rcWork;
        {
            int x = wa.left + (wa.right - wa.left - ww) / 2;
            int y = wa.top + (wa.bottom - wa.top - wh) / 2;
            /* Oversized windows must not spill past the top/left edge
               or the title bar becomes unreachable. */
            if (y < wa.top)
                y = wa.top;
            if (x < wa.left)
                x = wa.left;
            SetWindowPos(g_hwnd, NULL, x, y, ww, wh,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            {
                /* AdjustWindowRectEx under-counts a wrapped menu, so
                   measure the real client deficit and grow once. */
                RECT cr;
                GetClientRect(g_hwnd, &cr);
                if (cr.right - cr.left != g_outW ||
                    cr.bottom - cr.top != g_outH) {
                    SetWindowPos(g_hwnd, NULL, x, y,
                                 ww + g_outW - (cr.right - cr.left),
                                 wh + g_outH - (cr.bottom - cr.top),
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                    GetClientRect(g_hwnd, &cr);
                    if (cr.right - cr.left != g_outW ||
                        cr.bottom - cr.top != g_outH) {
                        /* The window cannot grow further (the game
                           caps it to the work area). Adopt the actual
                           client size as the output resolution or we
                           would recenter every frame and fight user
                           drags. */
                        g_outW = cr.right - cr.left;
                        g_outH = cr.bottom - cr.top;
                    }
                }
            }
        }
    }
}

/* --- resolution picker dialog ------------------------------------------- */

/* Persist the current output resolution so the next launch restores
   the same size. */
static void save_resolution(void)
{
    char b[16];
    wsprintfA(b, "%d", g_outW);
    WritePrivateProfileStringA("Display", "Width", b, g_iniPath);
    wsprintfA(b, "%d", g_outH);
    WritePrivateProfileStringA("Display", "Height", b, g_iniPath);
}

/* Apply a new output resolution immediately and persist it to
   WindowSize.ini so the same size is used on the next launch. */
static void apply_resolution(int w, int h)
{
    g_outW = w;
    g_outH = h;
    save_resolution();
    ensure_window_size();
}

/* Parse "WxH" / "W x H" text from the combo box. */
static int parse_resolution(const char *s, int *w, int *h)
{
    int a = 0, b = 0;
    while (*s && (*s < '0' || *s > '9')) s++;
    while (*s >= '0' && *s <= '9') a = a * 10 + *s++ - '0';
    while (*s && (*s < '0' || *s > '9')) s++;
    while (*s >= '0' && *s <= '9') b = b * 10 + *s++ - '0';
    if (a < GAME_W || b < GAME_H)
        return 0;
    *w = a;
    *h = b;
    return 1;
}

/* The game ships no dialog resource for this, so the template is
   built in memory: a combo box plus OK/Cancel buttons. */
static BYTE g_dlgTpl[1024];

static WORD *dlg_put_str(WORD *p, const WCHAR *s)
{
    while ((*p++ = *s++))
        ;
    return p;
}

static WORD *dlg_item(WORD *p, WORD cls, DWORD style, int x, int y,
                      int cx, int cy, WORD id, const WCHAR *text)
{
    DLGITEMTEMPLATE *it;
    p = (WORD *)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3);
    it = (DLGITEMTEMPLATE *)p;
    it->style = style;
    it->dwExtendedStyle = 0;
    it->x = (short)x;
    it->y = (short)y;
    it->cx = (short)cx;
    it->cy = (short)cy;
    it->id = id;
    p = (WORD *)(it + 1);
    *p++ = 0xFFFF;
    *p++ = cls;
    p = dlg_put_str(p, text);
    *p++ = 0;
    return p;
}

#define IDC_RES_COMBO 1001

static void build_dialog_template(void)
{
    WORD *p = (WORD *)g_dlgTpl;
    DLGTEMPLATE *t = (DLGTEMPLATE *)p;
    t->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME |
               DS_SETFONT;
    t->dwExtendedStyle = 0;
    t->cdit = 3;
    t->x = 0;
    t->y = 0;
    t->cx = 210;
    t->cy = 62;
    p = (WORD *)(t + 1);
    *p++ = 0;
    *p++ = 0;
    /* dialog caption: "window size" */
    p = dlg_put_str(p, L"ウィンドウサイズ");
    *p++ = 9;
    p = dlg_put_str(p, L"MS UI Gothic");
    p = dlg_item(p, 0x0085,
                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                     CBS_DROPDOWNLIST,
                 10, 10, 190, 200, IDC_RES_COMBO, L"");
    p = dlg_item(p, 0x0080,
                 WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                     BS_DEFPUSHBUTTON,
                 55, 38, 50, 14, IDOK, L"OK");
    dlg_item(p, 0x0080,
             WS_CHILD | WS_VISIBLE | WS_TABSTOP,
             110, 38, 50, 14, IDCANCEL, L"Cancel");
}

static INT_PTR CALLBACK ResDlgProc(HWND dlg, UINT msg, WPARAM wp,
                                   LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        static const int res[][2] = {
            {800, 600},   {1024, 768},  {1200, 900},  {1280, 960},
            {1440, 1080}, {1600, 1200}, {1920, 1440}, {2400, 1800},
            {2880, 2160}, {3200, 2400},
        };
        HWND cb = GetDlgItem(dlg, IDC_RES_COMBO);
        char b[32];
        int i, sel = -1;
        for (i = 0; i < 10; i++) {
            wsprintfA(b, "%d x %d", res[i][0], res[i][1]);
            SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)b);
            if (res[i][0] == g_outW && res[i][1] == g_outH)
                sel = i;
        }
        if (sel < 0) {
            wsprintfA(b, "%d x %d", g_outW, g_outH);
            SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)b);
            sel = i;
        }
        SendMessageA(cb, CB_SETCURSEL, sel, 0);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            char b[32];
            int w, h;
            GetDlgItemTextA(dlg, IDC_RES_COMBO, b, sizeof(b));
            if (parse_resolution(b, &w, &h))
                apply_resolution(w, h);
            EndDialog(dlg, 0);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void show_resolution_dialog(void)
{
    build_dialog_template();
    DialogBoxIndirectParamW(g_hInst, (LPCDLGTEMPLATEW)g_dlgTpl,
                          g_hwnd, ResDlgProc, 0);
}

/* --- BitBlt hook --------------------------------------------------------- */

/* Logical 800x600 back buffer. Every game blit is mirrored into it at
   1:1 (no rounding), then the whole buffer is presented to the window
   with a single StretchBlt. This avoids seams between partial updates
   that fractional dest-rect rounding would otherwise produce. */
static HDC g_backDC;
static HBITMAP g_backBmp;
static HGDIOBJ g_backOld;
static void *g_back_bits;       /* DIB ピクセル (D3D 転送用) */
static DWORD g_back_pitch;

static void ensure_backbuffer(HDC dst)
{
    BITMAPINFO bi;
    if (g_backDC)
        return;
    /* 32bpp トップダウン DIB。GDI 転送先として使えて、ピクセルへ
       直接アクセスできる (D3D テクスチャへの転送用) */
    g_backDC = CreateCompatibleDC(dst);
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = GAME_W;
    bi.bmiHeader.biHeight = -GAME_H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_backBmp = CreateDIBSection(dst, &bi, DIB_RGB_COLORS,
                                 &g_back_bits, NULL, 0);
    g_back_pitch = GAME_W * 4;
    g_backOld = SelectObject(g_backDC, g_backBmp);
}

/* --- D3D9 プレゼンター ---------------------------------------------------
   ミラー済みフレームをテクスチャへ転送し、フル画面クアッドを 1 枚
   描いて Present する。拡縮は GPU のサンプラーが行うので、出力
   サイズに関わらず CPU コストはほぼフレーム 1 枚分のコピーのみ。
   初期化に失敗した環境、およびデバイスロスト中のフレームは従来の
   GDI 経路へフォールバックする (フック内で常にどちらか一方が
   クライアント領域全体を担当するので両者の描画は混ざらない)。 */

static IDirect3D9Ex *g_d3d;
static IDirect3DDevice9Ex *g_d3ddev;
static IDirect3DTexture9 *g_d3dtex;
static int g_d3dW, g_d3dH;      /* swapchain (=クライアント) サイズ */
static int g_d3d_lost;
static int g_d3d_failed;        /* 作成失敗 → 以降 GDI 固定 */

typedef struct { float x, y, z, rhw, u, v; } QVERTEX;
#define QVF (D3DFVF_XYZRHW | D3DFVF_TEX1)

static void d3d_fill_pp(D3DPRESENT_PARAMETERS *pp, const RECT *rc)
{
    ZeroMemory(pp, sizeof(*pp));
    pp->BackBufferWidth = rc->right;
    pp->BackBufferHeight = rc->bottom;
    pp->BackBufferFormat = D3DFMT_X8R8G8B8;
    pp->BackBufferCount = 1;
    pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp->hDeviceWindow = g_hwnd;
    pp->Windowed = TRUE;
    pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
}

/* テクスチャ作成と描画状態の設定 (作成直後と Reset 後で共通) */
static int d3d_make_target(void)
{
    if (FAILED(g_d3ddev->lpVtbl->CreateTexture(
            g_d3ddev, GAME_W, GAME_H, 1, D3DUSAGE_DYNAMIC,
            D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_d3dtex, NULL)))
        return 0;
    g_d3ddev->lpVtbl->SetRenderState(g_d3ddev, D3DRS_LIGHTING, FALSE);
    g_d3ddev->lpVtbl->SetRenderState(g_d3ddev, D3DRS_CULLMODE,
                                   D3DCULL_NONE);
    g_d3ddev->lpVtbl->SetRenderState(g_d3ddev, D3DRS_ZENABLE,
                                   D3DZB_FALSE);
    g_d3ddev->lpVtbl->SetFVF(g_d3ddev, QVF);
    return 1;
}

/* デバイスの確保。クライアントサイズ変化・ロスト時は Reset する。
   使えない/ロスト中は 0 を返し、呼び出し側は GDI で描く */
static int d3d_ensure(void)
{
    RECT rc;
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;
    if (!g_gpu || g_d3d_failed || !g_hwnd)
        return 0;
    GetClientRect(g_hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0)
        return 0;
    if (!g_d3ddev) {
        if (!g_d3d &&
            FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &g_d3d))) {
            g_d3d_failed = 1;
            return 0;
        }
        d3d_fill_pp(&pp, &rc);
        /* FPU_PRESERVE: D3D は既定で FPU 精度を下げるので、
           x87 演算に依存する古いエンジンへの影響を防ぐ */
        hr = g_d3d->lpVtbl->CreateDeviceEx(
            g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd,
            D3DCREATE_HARDWARE_VERTEXPROCESSING |
            D3DCREATE_NOWINDOWCHANGES | D3DCREATE_FPU_PRESERVE,
            &pp, NULL, &g_d3ddev);
        if (FAILED(hr) || !d3d_make_target()) {
            if (g_d3ddev) {
                g_d3ddev->lpVtbl->Release(g_d3ddev);
                g_d3ddev = NULL;
            }
            g_d3d_failed = 1;
            return 0;
        }
        g_d3dW = rc.right;
        g_d3dH = rc.bottom;
        logmsg("d3d: device created");
        return 1;
    }
    if (g_d3d_lost || rc.right != g_d3dW || rc.bottom != g_d3dH) {
        if (g_d3dtex) {
            g_d3dtex->lpVtbl->Release(g_d3dtex);
            g_d3dtex = NULL;
        }
        d3d_fill_pp(&pp, &rc);
        /* ロスト継続中は Reset が DEVICELOST を返す。その間は
           GDI が描画を担当するのでここは諦めるだけでよい */
        if (FAILED(g_d3ddev->lpVtbl->ResetEx(g_d3ddev, &pp, NULL)) ||
            !d3d_make_target()) {
            g_d3d_lost = 1;
            return 0;
        }
        g_d3dW = rc.right;
        g_d3dH = rc.bottom;
        g_d3d_lost = 0;
    }
    return 1;
}

/* ミラー DIB の内容を viewport へ present する。0 = GDI で描くべき */
static int d3d_present(const RECT *vp)
{
    D3DLOCKED_RECT lr;
    QVERTEX q[4];
    HRESULT hr;
    int y;
    if (!d3d_ensure())
        return 0;
    hr = g_d3dtex->lpVtbl->LockRect(g_d3dtex, 0, &lr, NULL,
                                    D3DLOCK_DISCARD);
    if (FAILED(hr))
        return 0;
    for (y = 0; y < GAME_H; y++)
        memcpy((BYTE *)lr.pBits + y * lr.Pitch,
               (BYTE *)g_back_bits + y * g_back_pitch, GAME_W * 4);
    g_d3dtex->lpVtbl->UnlockRect(g_d3dtex, 0);
    /* RHW 座標はクライアント基準のピクセル単位。テクセル中心を
       合わせるため -0.5 のオフセットを入れる */
    q[0].x = (float)vp->left - 0.5f;
    q[0].y = (float)vp->top - 0.5f;
    q[0].z = 0; q[0].rhw = 1; q[0].u = 0; q[0].v = 0;
    q[1].x = (float)vp->right - 0.5f;
    q[1].y = (float)vp->top - 0.5f;
    q[1].z = 0; q[1].rhw = 1; q[1].u = 1; q[1].v = 0;
    q[2].x = (float)vp->left - 0.5f;
    q[2].y = (float)vp->bottom - 0.5f;
    q[2].z = 0; q[2].rhw = 1; q[2].u = 0; q[2].v = 1;
    q[3].x = (float)vp->right - 0.5f;
    q[3].y = (float)vp->bottom - 0.5f;
    q[3].z = 0; q[3].rhw = 1; q[3].u = 1; q[3].v = 1;
    g_d3ddev->lpVtbl->Clear(g_d3ddev, 0, NULL, D3DCLEAR_TARGET,
                            0xff000000, 1.0f, 0);
    if (FAILED(g_d3ddev->lpVtbl->BeginScene(g_d3ddev)))
        return 0;
    g_d3ddev->lpVtbl->SetTexture(g_d3ddev, 0,
        (IDirect3DBaseTexture9 *)g_d3dtex);
    /* Filter 設定はサンプラーに対応 */
    g_d3ddev->lpVtbl->SetSamplerState(g_d3ddev, 0, D3DSAMP_MAGFILTER,
        g_smooth ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    g_d3ddev->lpVtbl->SetSamplerState(g_d3ddev, 0, D3DSAMP_MINFILTER,
        g_smooth ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    g_d3ddev->lpVtbl->DrawPrimitiveUP(g_d3ddev, D3DPT_TRIANGLESTRIP,
                                      2, q, sizeof(QVERTEX));
    g_d3ddev->lpVtbl->EndScene(g_d3ddev);
    hr = g_d3ddev->lpVtbl->Present(g_d3ddev, NULL, NULL, NULL, NULL);
    if (hr == D3DERR_DEVICELOST) {
        g_d3d_lost = 1;
        return 0;
    }
    return SUCCEEDED(hr);
}

/* StretchBlt 経路の観察用: 何も変えずにログだけ取る */
static BOOL WINAPI StretchBlt_Hook(HDC dst, int x, int y, int cx, int cy,
                                   HDC src, int sx, int sy, int sw, int sh,
                                   DWORD rop)
{
    static DWORD seen[64]; static int seen_n;
    DWORD key = ((DWORD)cx << 20) ^ ((DWORD)cy << 8) ^ ((DWORD)sw << 4) ^
                (DWORD)sh ^ ((DWORD)sx << 16);
    int i;
    for (i = 0; i < seen_n; i++) if (seen[i] == key) break;
    if (i == seen_n && seen_n < 64)
        seen[seen_n++] = key,
        logfmt("sblit: dst=%d,%d %dx%d src=%d,%d %dx%d",
               x, y, cx, cy, sx, sy, sw, sh);
    return g_origStretchBlt(dst, x, y, cx, cy, src, sx, sy, sw, sh, rop);
}

static BOOL WINAPI BitBlt_Hook(HDC dst, int x, int y, int cx, int cy,
                               HDC src, int sx, int sy, DWORD rop)
{
    if (cx > 0 && cy > 0 &&
        sx >= 0 && sy >= 0 && sx + cx <= GAME_W && sy + cy <= GAME_H) {
        HWND w;
        if (!g_hwnd)
            EnumWindows(find_game_window, (LPARAM)&g_hwnd);
        w = WindowFromDC(dst);
        /* 実際の present 呼び出しパラメータを記録 (重複排除) */
        {
            static DWORD seen[64]; static int seen_n;
            DWORD key = ((DWORD)cx << 20) ^ ((DWORD)cy << 8) ^
                        ((DWORD)sx << 4) ^ (DWORD)sy ^ ((DWORD)x << 16);
            int i;
            for (i = 0; i < seen_n; i++) if (seen[i] == key) break;
            if (i == seen_n && seen_n < 64) {
                seen[seen_n++] = key;
                logfmt("blit[%ums]: dst=%p wnd=%d x,y=%d,%d cx,cy=%d,%d sx,sy=%d,%d",
                       GetTickCount(), dst, w == g_hwnd, x, y,
                       cx, cy, sx, sy);
            }
        }
        /* Window detection, subclassing and menu injection run even
           while scaling is disabled so the settings menu exists. */
        if (w && g_hwnd && w == g_hwnd && !g_origWndProc) {
            HMENU m, sys;
            LONG st;
            g_origWndProc = (WNDPROC)SetWindowLongPtrA(
                g_hwnd, GWLP_WNDPROC, (LONG_PTR)GameWndProc);
            /* Make the window user-resizable; the scaler follows the
               live client size. */
            st = GetWindowLongA(g_hwnd, GWL_STYLE);
            if (!(st & WS_THICKFRAME)) {
                SetWindowLongA(g_hwnd, GWL_STYLE, st | WS_THICKFRAME);
                SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
            }
            /* Insert our borderless entry right under the game's
               own fullscreen item in the System submenu, using
               half-width katakana to match its style. */
            m = GetMenu(g_hwnd);
            sys = m ? GetSubMenu(m, 0) : NULL;
            if (sys &&
                GetMenuState(sys, CMD_BORDERLESS, MF_BYCOMMAND)
                    == (UINT)-1) {
                InsertMenuW(sys, 1, MF_BYPOSITION | MF_STRING,
                            CMD_BORDERLESS,
                            L"\xFF8E\xFF9E\xFF70\xFF80\xFF9E\xFF70"
                            L"\xFF9A\xFF7D\xFF8C\xFF99\xFF7D\xFF78"
                            L"\xFF98\xFF70\xFF9D(&B)      Alt+B");
            }
            /* The resolution item goes into the Settings (設定)
               submenu, right below its "fullscreen mode" entry
               (command id 129), preceded by a separator. */
            {
                HMENU set = m ? GetSubMenu(m, 1) : NULL;
                int i, n, pos = -1;
                if (set &&
                    GetMenuState(set, CMD_RESOLUTION, MF_BYCOMMAND)
                        == (UINT)-1) {
                    n = GetMenuItemCount(set);
                    for (i = 0; i < n; i++)
                        if (GetMenuItemID(set, i) == 129) {
                            pos = i;
                            break;
                        }
                    if (pos < 0)
                        pos = n - 1;
                    InsertMenuW(set, pos + 1,
                                MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
                    InsertMenuW(set, pos + 2,
                                MF_BYPOSITION | MF_STRING,
                                CMD_RESOLUTION,
                                L"\xFF73\xFF68\xFF9D\xFF84\xFF9E"
                                L"\xFF73\xFF7B\xFF72\xFF7D\xFF9E"
                                L"(&Z)...");
                }
            }
        }
        if (w && g_hwnd && w == g_hwnd && g_enabled &&
            !native_fullscreen()) {
            RECT vp;
            int vw, vh;
            ensure_window_size();
            get_viewport(&vp);
            vw = vp.right - vp.left;
            vh = vp.bottom - vp.top;
            if (g_renderscale > 1 && g_dib_dc) {
                /* HD モード: 差し替えた N 倍 DIB をそのまま表示。
                   ゲーム側 srcDC は旧バッファを指すため使わない。 */
                RECT rc;
                GetClientRect(w, &rc);
                if (vp.left > 0)
                    PatBlt(dst, 0, 0, vp.left, rc.bottom, BLACKNESS);
                if (vp.right < rc.right)
                    PatBlt(dst, vp.right, 0, rc.right - vp.right,
                           rc.bottom, BLACKNESS);
                if (vp.top > 0)
                    PatBlt(dst, vp.left, 0, vp.right - vp.left,
                           vp.top, BLACKNESS);
                if (vp.bottom < rc.bottom)
                    PatBlt(dst, vp.left, vp.bottom,
                           vp.right - vp.left,
                           rc.bottom - vp.bottom, BLACKNESS);
                SetStretchBltMode(dst,
                                  g_smooth ? HALFTONE : COLORONCOLOR);
                if (g_smooth)
                    SetBrushOrgEx(dst, 0, 0, NULL);
                return StretchBlt(dst, vp.left, vp.top,
                                  vp.right - vp.left,
                                  vp.bottom - vp.top,
                                  g_dib_dc, 0, 0,
                                  GAME_W * g_renderscale,
                                  GAME_H * g_renderscale,
                                  SRCCOPY);
            }
            /* GPU present: フックした blit を全てミラー DIB へ 1:1
               で写し、その内容をテクスチャ経由で Present する。
               拡縮は GPU 任せなのでサイズによらず軽い。失敗した
               フレームだけ従来の GDI 経路へ落ちる。 */
            if (g_gpu && !g_d3d_failed) {
                ensure_backbuffer(dst);
                g_origBitBlt(g_backDC, x, y, cx, cy,
                             src, sx, sy, rop);
                if (d3d_present(&vp))
                    return TRUE;
            }
            /* 黒帯を毎回塗り直す。クラスの背景ブラシが消してしまう
               ため。viewport の内側はこれ以降の転送が上書きする。 */
            {
                RECT rc;
                GetClientRect(w, &rc);
                if (vp.left > 0)
                    PatBlt(dst, 0, 0, vp.left, rc.bottom, BLACKNESS);
                if (vp.right < rc.right)
                    PatBlt(dst, vp.right, 0, rc.right - vp.right,
                           rc.bottom, BLACKNESS);
                if (vp.top > 0)
                    PatBlt(dst, vp.left, 0, vp.right - vp.left,
                           vp.top, BLACKNESS);
                if (vp.bottom < rc.bottom)
                    PatBlt(dst, vp.left, vp.bottom,
                           vp.right - vp.left,
                           rc.bottom - vp.bottom, BLACKNESS);
            }
            /* 等倍 fast path: ミラーと StretchBlt を介さず直接
               転送する。HD (ViewWidth>800) 環境ではここが主経路。 */
            if (vw == GAME_W && vh == GAME_H &&
                x >= 0 && y >= 0 &&
                x + cx <= GAME_W && y + cy <= GAME_H)
                return g_origBitBlt(dst, vp.left + x, vp.top + y,
                                    cx, cy, src, sx, sy, rop);
            /* 全画面 present fast path: ソースが完成済みフレーム
               なのでミラーを介さず直接スケーリングする。部分更新と
               違い継ぎ目の心配が無い。 */
            if (x == 0 && y == 0 && sx == 0 && sy == 0 &&
                cx == GAME_W && cy == GAME_H) {
                SetStretchBltMode(dst,
                                  g_smooth ? HALFTONE : COLORONCOLOR);
                if (g_smooth)
                    SetBrushOrgEx(dst, 0, 0, NULL);
                return StretchBlt(dst, vp.left, vp.top, vw, vh,
                                  src, 0, 0, GAME_W, GAME_H, rop);
            }
            /* 部分更新: 論理バックバッファへミラーしてから全体を
               引き伸ばす (部分 StretchBlt の丸め誤差で生じる
               継ぎ目を避けるため) */
            ensure_backbuffer(dst);
            g_origBitBlt(g_backDC, x, y, cx, cy, src, sx, sy, rop);
            SetStretchBltMode(dst, g_smooth ? HALFTONE : COLORONCOLOR);
            if (g_smooth)
                /* Keep a fixed halftone phase: per-blit origins shift
                   the dither pattern and show up as shimmer. */
                SetBrushOrgEx(dst, 0, 0, NULL);
            return StretchBlt(dst, vp.left, vp.top, vw, vh,
                              g_backDC, 0, 0, GAME_W, GAME_H, SRCCOPY);
        }
    }
    return g_origBitBlt(dst, x, y, cx, cy, src, sx, sy, rop);
}

/* Convert the scaled client coordinate back to the 800x600 game space. */
static BOOL WINAPI ScreenToClient_Hook(HWND hwnd, LPPOINT pt)
{
    BOOL r;
    r = g_origScreenToClient(hwnd, pt);
    /* Inputs are already logical screen coords once GetCursorPos is
       hooked, so the client-space result is logical too. Do not rescale
       here or the position would be converted twice. */
    (void)g_hwnd;
    return r;
}

/* The game auto-moves the cursor with SetCursorPos using positions in
   the unscaled 800x600 space. Re-map screen coordinates that fall inside
   the game window to the scaled position. */
static BOOL WINAPI SetCursorPos_Hook(int x, int y)
{
    if (g_enabled && g_hwnd && !native_fullscreen()) {
        POINT pt;
        pt.x = x;
        pt.y = y;
        g_origScreenToClient(g_hwnd, &pt);
        {
            /* The game emits warp targets in 800x600 logical space.
               Scale everything uniformly; out-of-range results are
               clamped by SetCursorPos anyway. */
            RECT vp;
            get_viewport(&vp);
            pt.x = vp.left + MulDiv(pt.x, vp.right - vp.left, GAME_W);
            pt.y = vp.top + MulDiv(pt.y, vp.bottom - vp.top, GAME_H);
            g_origClientToScreen(g_hwnd, &pt);
            return g_origSetCursorPos(pt.x, pt.y);
        }
    }
    return g_origSetCursorPos(x, y);
}

/* Report the cursor position in 800x600 logical space (expressed as
   screen coords) so the game computes consistent positions even through
   paths that use raw screen coordinates. */
static BOOL WINAPI GetCursorPos_Hook(LPPOINT pt)
{
    BOOL r = g_origGetCursorPos(pt);
    if (r && g_enabled && g_hwnd && !native_fullscreen()) {
        RECT vp;
        POINT c = *pt;
        g_origScreenToClient(g_hwnd, &c);
        get_viewport(&vp);
        c.x = MulDiv(c.x - vp.left, GAME_W, vp.right - vp.left);
        c.y = MulDiv(c.y - vp.top, GAME_H, vp.bottom - vp.top);
        g_origClientToScreen(g_hwnd, &c);
        *pt = c;
    }
    return r;
}

static void patch_import_thunk(PIMAGE_IMPORT_DESCRIPTOR imp,
                               BYTE *base, const char *dllname,
                               FARPROC real, void *hook,
                               const char *label)
{
    for (; imp->Name; imp++) {
        const char *name = (const char *)(base + imp->Name);
        if (lstrcmpiA(name, dllname) == 0)
            break;
    }
    if (!imp->Name)
        return;
    {
        DWORD *thunk = (DWORD *)(base + imp->FirstThunk);
        DWORD i;
        for (i = 0; thunk[i]; i++) {
            if (thunk[i] == (DWORD)(UINT_PTR)real) {
                DWORD old;
                VirtualProtect(&thunk[i], sizeof(DWORD),
                               PAGE_EXECUTE_READWRITE, &old);
                thunk[i] = (DWORD)(UINT_PTR)hook;
                VirtualProtect(&thunk[i], sizeof(DWORD), old, &old);
                FlushInstructionCache(GetCurrentProcess(),
                                      &thunk[i], sizeof(DWORD));
                logmsg(label);
                return;
            }
        }
    }
}

static void hook_module(HMODULE mod, const char *dllname, FARPROC real,
                        void *hook, const char *label)
{
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)mod;
    PIMAGE_NT_HEADERS nt;
    PIMAGE_IMPORT_DESCRIPTOR imp;
    if (!mod || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    nt = (PIMAGE_NT_HEADERS)((BYTE *)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;
    if (!nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
            .VirtualAddress)
        return;
    imp = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE *)mod +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
            .VirtualAddress);
    patch_import_thunk(imp, (BYTE *)mod, dllname, real, hook, label);
}

/* Patch BitBlt/ScreenToClient thunks in every loaded module: mouse and
   blit code may live in engine DLLs (Sys42VM, SACT2, ...), not just exe. */
static void hook_iat(void)
{
    HANDLE snap;
    MODULEENTRY32 me;
    HMODULE gdi, u32;
    FARPROC realBitBlt, realS2C, realSCP, realGCP, realStretchBlt;

    gdi = GetModuleHandleA("GDI32.dll");
    u32 = GetModuleHandleA("USER32.dll");
    realBitBlt = gdi ? GetProcAddress(gdi, "BitBlt") : NULL;
    realStretchBlt = gdi ? GetProcAddress(gdi, "StretchBlt") : NULL;
    realS2C = u32 ? GetProcAddress(u32, "ScreenToClient") : NULL;
    realSCP = u32 ? GetProcAddress(u32, "SetCursorPos") : NULL;
    realGCP = u32 ? GetProcAddress(u32, "GetCursorPos") : NULL;
    if (realBitBlt) g_origBitBlt = (PFN_BitBlt)realBitBlt;
    if (realStretchBlt)
        g_origStretchBlt = (PFN_StretchBlt)realStretchBlt;
    if (realS2C) g_origScreenToClient = (PFN_ScreenToClient)realS2C;
    if (realSCP) g_origSetCursorPos = (PFN_SetCursorPos)realSCP;
    if (realGCP) g_origGetCursorPos = (PFN_GetCursorPos)realGCP;
    if (u32)
        g_origClientToScreen =
            (PFN_ClientToScreen)GetProcAddress(u32, "ClientToScreen");

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                    GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        hook_module(GetModuleHandleA(NULL), "GDI32.dll", realBitBlt,
                    BitBlt_Hook, "BitBlt IAT hooked");
        hook_module(GetModuleHandleA(NULL), "USER32.dll", realS2C,
                    ScreenToClient_Hook, "ScreenToClient IAT hooked");
        hook_module(GetModuleHandleA(NULL), "USER32.dll", realSCP,
                    SetCursorPos_Hook, "SetCursorPos IAT hooked");
        hook_module(GetModuleHandleA(NULL), "USER32.dll", realGCP,
                    GetCursorPos_Hook, "GetCursorPos IAT hooked");
        return;
    }
    me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            if (realBitBlt)
                hook_module(me.hModule, "GDI32.dll", realBitBlt,
                            BitBlt_Hook, "BitBlt IAT hooked");
            if (realStretchBlt &&
                me.hModule != (HMODULE)g_hInst)
                hook_module(me.hModule, "GDI32.dll", realStretchBlt,
                            StretchBlt_Hook, "StretchBlt IAT hooked");
            if (realS2C)
                hook_module(me.hModule, "USER32.dll", realS2C,
                            ScreenToClient_Hook, "ScreenToClient IAT hooked");
            if (realSCP)
                hook_module(me.hModule, "USER32.dll", realSCP,
                            SetCursorPos_Hook, "SetCursorPos IAT hooked");
            if (realGCP)
                hook_module(me.hModule, "USER32.dll", realGCP,
                            GetCursorPos_Hook, "GetCursorPos IAT hooked");
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    logmsg("IAT scan done");
}

/* --- dinput.dll forwards ------------------------------------------------- */

static void load_real(void)
{
    char sys[MAX_PATH];
    if (g_dinput) return;
    GetSystemDirectoryA(sys, MAX_PATH);
    lstrcatA(sys, "\\dinput.dll");
    g_dinput = LoadLibraryA(sys);
}

#define FORWARD(fn) \
    load_real(); \
    if (!g_dinput) return E_FAIL; \
    return ((PFN_##fn)GetProcAddress(g_dinput, #fn))

typedef HRESULT (WINAPI *PFN_DirectInputCreateA)(HINSTANCE, DWORD, LPVOID *, LPVOID);
typedef HRESULT (WINAPI *PFN_DirectInputCreateW)(HINSTANCE, DWORD, LPVOID *, LPVOID);
typedef HRESULT (WINAPI *PFN_DirectInputCreateEx)(HINSTANCE, DWORD, REFIID, LPVOID *, LPVOID);
typedef HRESULT (WINAPI *PFN_DllGetClassObject)(REFCLSID, REFIID, LPVOID *);
typedef HRESULT (WINAPI *PFN_DllCanUnloadNow)(void);
typedef HRESULT (WINAPI *PFN_DllRegisterServer)(void);
typedef HRESULT (WINAPI *PFN_DllUnregisterServer)(void);

HRESULT WINAPI DirectInputCreateA(HINSTANCE a, DWORD b, LPVOID *c, LPVOID d)
{ FORWARD(DirectInputCreateA)(a, b, c, d); }

HRESULT WINAPI DirectInputCreateW(HINSTANCE a, DWORD b, LPVOID *c, LPVOID d)
{ FORWARD(DirectInputCreateW)(a, b, c, d); }

HRESULT WINAPI DirectInputCreateEx(HINSTANCE a, DWORD b, REFIID c, LPVOID *d, LPVOID e)
{
    load_real();
    if (!g_dinput) return E_FAIL;
    return ((PFN_DirectInputCreateEx)GetProcAddress(g_dinput,
        "DirectInputCreateEx"))(a, b, c, d, e);
}

HRESULT WINAPI DllGetClassObject(REFCLSID a, REFIID b, LPVOID *c)
{ FORWARD(DllGetClassObject)(a, b, c); }

HRESULT WINAPI DllCanUnloadNow(void)
{ FORWARD(DllCanUnloadNow)(); }

HRESULT WINAPI DllRegisterServer(void)
{ FORWARD(DllRegisterServer)(); }

HRESULT WINAPI DllUnregisterServer(void)
{ FORWARD(DllUnregisterServer)(); }

/* --- HD バックバッファ (RenderScale) ------------------------------------
   最小実験: exe 側転送先 surface (SACT2 comp[0x24], 800x600 DIB) の
   ピクセルバッファだけを N 倍サイズの自前 DIB に差し替え、
   dims/pitch/bits を更新する。座標変換は一切行わない。
   まず「エンジンが物理 dims を読んでどうレイアウトするか」を観察する。 */

#define SACT2_SUPPLIER_OFF 0x40234
#define SPRITE_COMP_OFF    0x98

/* アドレスが属するモジュールのファイル名を返す */
static const char *mod_name_of(const void *addr, char *buf, DWORD bufsz)
{
    HMODULE m;
    char *p;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &m))
        return "?";
    if (!GetModuleFileNameA(m, buf, bufsz))
        return "?";
    p = buf + lstrlenA(buf);
    while (p > buf && p[-1] != '\\' && p[-1] != '/')
        p--;
    return p;
}

/* exe 側 surface クラスのフィールド:
   +0x0c 幅, +0x10 高さ, +0x18 1px バイト数, +0x1c pitch, +0x24 ピクセル
   +0x34 には関連オブジェクトがあり、bits/pitch をキャッシュする場合あり */
static int setup_hd_backbuffer(DWORD *surf, int n)
{
    int w, h, bpp;
    DWORD pitch, newpitch, oldbits;
    BITMAPINFO bmi;
    HDC dc;
    void *bits = NULL;
    HBITMAP bmp;
    if (!surf || IsBadReadPtr(surf, 0x28) || IsBadWritePtr(surf, 0x28))
        return 0;
    w = surf[0x0c / 4];
    h = surf[0x10 / 4];
    bpp = surf[0x18 / 4];
    pitch = surf[0x1c / 4];
    oldbits = surf[0x24 / 4];
    if (w <= 0 || h <= 0 || bpp < 1 || bpp > 4 || !oldbits)
        return 0;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w * n;
    bmi.bmiHeader.biHeight = -(h * n);   /* トップダウン */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = (WORD)(bpp * 8);
    bmi.bmiHeader.biCompression = BI_RGB;
    dc = CreateCompatibleDC(NULL);
    bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) {
        logfmt("hd: CreateDIBSection failed");
        return 0;
    }
    SelectObject(dc, bmp);
    if (!g_bb_surf) {
        g_dib_dc = dc;
        g_dib_bits = bits;
    }
    newpitch = ((w * n * bpp + 3) & ~3);
    g_dib_pitch = newpitch;
    /* オブジェクト先頭 0x40 バイト内の旧 bits/pitch を総なめで置換
       (別フィールドにキャッシュされている場合への対策) */
    {
        int k;
        for (k = 0; k < 0x40; k += 4)
            if (surf[k / 4] == oldbits)
                surf[k / 4] = (DWORD)bits;
    }
    /* 関連オブジェクト (surf+0x34) にキャッシュされた bits/pitch も
       追従する (GLEffectCopy 等が旧バッファへ書き込むのを防ぐ) */
    {
        DWORD *rel = (DWORD *)surf[0x34 / 4];
        int k;
        if (rel && !IsBadReadPtr(rel, 0x40) && !IsBadWritePtr(rel, 0x40))
            for (k = 0; k < 0x40; k += 4) {
                if (rel[k / 4] == oldbits)
                    rel[k / 4] = (DWORD)bits;
                else if (rel[k / 4] == pitch)
                    rel[k / 4] = newpitch;
            }
    }
    surf[0x0c / 4] = w * n;
    surf[0x10 / 4] = h * n;
    surf[0x1c / 4] = newpitch;
    surf[0x24 / 4] = (DWORD)bits;
    if (!g_bb_surf)
        g_bb_surf = surf;
    logfmt("hd: backbuffer resized to %dx%d pitch=%u bits=%p (surf=%p)",
           w * n, h * n, newpitch, bits, surf);
    /* クリップ矩形等の未確認フィールドを特定するため全フィールドを
       ダンプする (0x0c/0x10 の w,h 以外に 800x600 が残っていたら
       それがクリッピングの候補) */
    {
        int k;
        for (k = 0; k < 0x40; k += 0x10)
            logfmt("hd: surf+%02x: %08x %08x %08x %08x", k,
                   surf[k/4], surf[k/4+1], surf[k/4+2], surf[k/4+3]);
        {
            DWORD *rel = (DWORD *)surf[0x34 / 4];
            if (rel && !IsBadReadPtr(rel, 0x40))
                for (k = 0; k < 0x40; k += 0x10)
                    logfmt("hd: rel+%02x: %08x %08x %08x %08x", k,
                           rel[k/4], rel[k/4+1], rel[k/4+2], rel[k/4+3]);
        }
    }
    return 1;
}

/* g_dib_bits の内容を bb_dump.bmp として保存する */
static void dump_bb_bmp(void)
{
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    HANDLE f;
    DWORD bw = GAME_W * g_renderscale;
    DWORD bh = GAME_H * g_renderscale;
    DWORD wr, y;
    if (!g_dib_bits || !g_dib_pitch)
        return;
    f = CreateFileA("bb_dump.bmp", GENERIC_WRITE, 0, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    ZeroMemory(&fh, sizeof(fh));
    fh.bfType = 0x4d42;
    fh.bfSize = sizeof(fh) + sizeof(ih) + g_dib_pitch * bh;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    ZeroMemory(&ih, sizeof(ih));
    ih.biSize = sizeof(ih);
    ih.biWidth = bw;
    ih.biHeight = bh;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    WriteFile(f, &fh, sizeof(fh), &wr, NULL);
    WriteFile(f, &ih, sizeof(ih), &wr, NULL);
    for (y = 0; y < bh; y++)
        WriteFile(f,
                  (BYTE *)g_dib_bits + (bh - 1 - y) * g_dib_pitch,
                  bw * 3, &wr, NULL);
    CloseHandle(f);
}


/* 任意の surface の中身を bmp へダンプする (デバッグ用) */
static int surf_dims(DWORD *s, DWORD *w, DWORD *h);

static void dump_surf_bmp(DWORD *surf, int idx)
{
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    HANDLE f;
    DWORD wr, y, w, h, pitch, bpp;
    BYTE *bits;
    char name[64];
    DWORD vw = 0, vh = 0;
    int k;
    if (!surf || IsBadReadPtr(surf, 0x60))
        return;
    /* getter で実寸を取り、bits 相当のポインタを先頭 0x60
       から探索する (クラスによりレイアウトが異なるため) */
    surf_dims(surf, &vw, &vh);
    w = vw; h = vh; bpp = 3; pitch = 0; bits = NULL;
    if (w && h) {
        DWORD need = w * h * 3;
        for (k = 0; k < 0x60; k += 4) {
            DWORD pv = surf[k / 4];
            if (pv > 0x10000 &&
                !IsBadReadPtr((void *)pv, need - 4096)) {
                bits = (BYTE *)pv;
                pitch = w * bpp;
                break;
            }
        }
    }
    if (!bits) {
        /* getter が無い/失敗した場合は旧レイアウトを試す */
        w = surf[0x0c / 4];
        h = surf[0x10 / 4];
        bpp = surf[0x18 / 4];
        pitch = surf[0x1c / 4];
        bits = (BYTE *)surf[0x24 / 4];
    }
    if (!w || !h || w > 8192 || h > 8192 || (bpp != 3 && bpp != 4) ||
        !pitch || IsBadReadPtr(bits, 16)) {
        logfmt("dump: surf=%p w=%u h=%u bpp=%u pitch=%u bits=%p"
               " (skipped)", surf, w, h, bpp, pitch, bits);
        return;
    }
    wsprintfA(name, "surf_dump_%d.bmp", idx);
    f = CreateFileA(name, GENERIC_WRITE, 0, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    ZeroMemory(&fh, sizeof(fh));
    fh.bfType = 0x4d42;
    fh.bfSize = sizeof(fh) + sizeof(ih) + pitch * h;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    ZeroMemory(&ih, sizeof(ih));
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    WriteFile(f, &fh, sizeof(fh), &wr, NULL);
    WriteFile(f, &ih, sizeof(ih), &wr, NULL);
    for (y = 0; y < h; y++)
        WriteFile(f, bits + (h - 1 - y) * pitch, w * bpp, &wr, NULL);
    CloseHandle(f);
}

/* --- レンダラー vtable フック (Graph.dll) --------------------------------- *
 * Graph.dll の描画レンダラーオブジェクトの vtable を全スロット差し替え、
 * dst がスクリーン級 surface (bb 以上の実寸) の呼び出しだけ
 * 座標を論理→物理へ変換する。エンジンのコピー系は完全な 1:1
 * ピクセルセマンティクス (w,h = src ピクセル数) なので w,h は触らない。
 * 座標は論理値 (<=800x600) で来るものと、dims getter 由来の物理値で
 * 来るものが混在するため、論理域内のものだけを変換する。 */

#define MAX_RD_SLOTS 384
#define VT_MAX_BYTES 0x140

typedef struct {
    DWORD vt;
    DWORD slot;
    DWORD orig;
    DWORD is_graph;   /* Graph.dll レンダラー由来か */
} RD_SLOT;

static RD_SLOT g_slots[MAX_RD_SLOTS];
static int g_slots_n;
static BYTE *g_stub_pool;
static int g_stub_used;

static DWORD rd_lookup(int ctx)
{
    return g_slots[ctx].orig;
}

/* thiscall ヘルパー: obj->vt[slot]() を呼ぶ。壊れたポインタは SEH で
   捨てる (IsBadReadPtr は例外プローブで VEH を汚染するため使わない) */
static DWORD call_this0(DWORD *obj, DWORD slot)
{
    DWORD ret = 0, fn;
    DWORD *vt;
    if (!obj || IsBadReadPtr(obj, 4))
        return 0;
    vt = (DWORD *)*obj;
    if (IsBadReadPtr(vt, slot + 4))
        return 0;
    fn = vt[slot / 4];
    if (IsBadCodePtr((FARPROC)fn))
        return 0;
    {
        /* 呼び出し先がロード済みモジュール内のコードであることを確認 */
        HMODULE fm = NULL;
        if (!GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)fn, &fm) || !fm)
            return 0;
    }
    __asm {
        mov ecx, obj
        call fn
        mov ret, eax
    }
    return ret;
}

static DWORD call_this2(DWORD *obj, DWORD slot, DWORD a, DWORD b)
{
    DWORD ret = 0, fn;
    DWORD *vt;
    if (!obj || IsBadReadPtr(obj, 4))
        return 0;
    vt = (DWORD *)*obj;
    if (IsBadReadPtr(vt, slot + 4))
        return 0;
    fn = vt[slot / 4];
    if (IsBadCodePtr((FARPROC)fn))
        return 0;
    __asm {
        mov ecx, obj
        push b
        push a
        call fn
        mov ret, eax
    }
    return ret;
}

/* surface の実寸を vtable getter (slot 0x10=w, 0x14=h) で取得する */
static int surf_dims(DWORD *s, DWORD *w, DWORD *h)
{
    DWORD vw = call_this0(s, 0x10);
    DWORD vh = call_this0(s, 0x14);
    if (vw <= 0 || vh <= 0 || vw > 16384 || vh > 16384)
        return 0;
    *w = vw;
    *h = vh;
    return 1;
}

/* クラッシュ解析用: 最後に通ったフック呼び出しを保持する */
static int g_last_slot = -1;
static DWORD g_last_self, g_last_args[8];

/* レンダラー呼び出しの座標補正。
   dst が bb または bb 級の大きな中間 surface の場合のみ変換。
   copy 系: slot 0x10 は (dst,sx,sy,src,dx,dy,w,h)、
            他は (dst,dx,dy,src,sx,sy,w,h)。w,h は src ピクセル数。
   fill 系: (dst,x,y,w,h,...)。全て論理域内のときだけ一括 N 倍。 */
static void xform_rd(int ctx, void *self, DWORD ret, DWORD *args)
{
    DWORD slot;
    int i;
    (void)ret;
    if (ctx >= 0 && ctx < g_slots_n) {
        g_last_slot = g_slots[ctx].slot;
        g_last_self = (DWORD)self;
        for (i = 0; i < 8; i++)
            g_last_args[i] = args[i];
    }
    if (g_renderscale < 2 || !g_bb_surf)
        return;
    if (ctx < 0 || ctx >= g_slots_n)
        return;
    slot = g_slots[ctx].slot;
    /* Graph.dll 由来でない vtable の呼び出しは変換しない
       (ログのみ残す) */
    if (!g_slots[ctx].is_graph)
        slot |= 0x10000000;
    /* 全スロットの初出呼び出しを記録してクリップ設定等の
       状態系呼び出しを特定する (スロット毎に3件まで) */
    {
        static int slot_log_n[128];
        int si = (slot & 0xffff) / 4;
        if (si >= 0 && si < 128 && slot_log_n[si] < 20) {
            slot_log_n[si]++;
            {
                char extra[160];
                int ei = 0, k;
                extra[0] = 0;
                for (k = 0; k < 8 && ei < 130; k++) {
                    const char *s = (const char *)args[k];
                    int j;
                    if ((DWORD)s < 0x10000 || IsBadReadPtr(s, 4))
                        continue;
                    for (j = 0; j < 32; j++) {
                        unsigned char c = (unsigned char)s[j];
                        if (c == 0) break;
                        if (c < 0x20) break;
                    }
                    if (j >= 4 && s[j] == 0)
                        ei += wsprintfA(extra + ei, " a%d='%s'",
                                        k, s);
                }
                logfmt("rdall: s%02x%s args=%x %x %x %x %x %x %x %x%s",
                       slot & 0xffff,
                       (slot & 0x10000000) ? "!" : "",
                       args[0], args[1], args[2], args[3],
                       args[4], args[5], args[6], args[7], extra);
            }
        }
    }
    /* 大きな dst へのコピーで来た src surface をユニーク毎にダンプ
       (Graph.dll 経由の呼び出しのみ: 他モジュールは引数規約が違い
        args[0]/args[3] が surface ではない可能性がある) */
    if (g_slots[ctx].is_graph == 1 &&
        (slot == 0x10 || slot == 0x14 || slot == 0x28 ||
         slot == 0x38 || slot == 0x48)) {
        static DWORD *seen_src[64];
        static int seen_n;
        DWORD *s = (DWORD *)args[3];
        int k, dup = 0;
        for (k = 0; k < seen_n; k++)
            if (seen_src[k] == s)
                dup = 1;
        if (!dup && seen_n < 64) {
            seen_src[seen_n] = s;
            dump_surf_bmp(s, seen_n);
            seen_n++;
        }
    }
    /* dst==bb または bb 級中間 surface への呼び出しを記録 */
    {
        DWORD bw0 = GAME_W * g_renderscale, bh0 = GAME_H * g_renderscale;
        DWORD dbw = 0, dbh = 0;
        /* args[0] が surface ポインタと分かっているスロットのみ
           getter を呼ぶ (それ以外は int 等が来てクラッシュする) */
        int maybe_surf =
            g_slots[ctx].is_graph == 1 &&
            (slot == 0x10 || slot == 0x14 || slot == 0x28 ||
             slot == 0x38 || slot == 0x48 || slot == 0x54 ||
             slot == 0x58 || slot == 0x5c);
        int is_big_dst =
            args[0] == (DWORD)g_bb_surf ||
            (maybe_surf &&
             surf_dims((DWORD *)args[0], &dbw, &dbh) &&
             dbw >= bw0 && dbh >= bh0);
        if (is_big_dst && !dbw) { dbw = bw0; dbh = bh0; }
        /* 画面サイズ級の dst への呼び出しも拾う
           (getter が論理寸法を返す中間 surface を検出する) */
        if (!is_big_dst && maybe_surf && dbw >= GAME_W &&
            dbh >= GAME_H)
            is_big_dst = 1;
        /* 800x600 のまま残っている中間 surface を検出したら
           bb と同様にピクセルバッファを N 倍へ事後差し替える */
        if (maybe_surf && dbw == GAME_W && dbh == GAME_H) {
            static DWORD *upgraded[64];
            static int upgraded_n;
            DWORD *s = (DWORD *)args[0];
            int k, seen = 0;
            for (k = 0; k < upgraded_n; k++)
                if (upgraded[k] == s)
                    seen = 1;
            if (!seen) {
                if (upgraded_n < 64 &&
                    setup_hd_backbuffer(s, g_renderscale)) {
                    upgraded[upgraded_n++] = s;
                    logfmt("hd: late-upgraded surface %p",
                           s);
                    is_big_dst = 1;
                    dbw = bw0;
                    dbh = bh0;
                }
            }
        }
        if (is_big_dst) {
        static int bb_log_n[64];
        int li = (slot & 0xffff) / 4;
        if (li >= 0 && li < 64 && bb_log_n[li] < 30) {
            DWORD sw = 0, sh = 0;
            char sb[MAX_PATH], db[MAX_PATH];
            const char *smod = "?", *dmod = "?";
            bb_log_n[li]++;
            if (slot == 0x10 || slot == 0x14 || slot == 0x28 ||
                slot == 0x38 || slot == 0x48) {
                DWORD *so = (DWORD *)args[3];
                surf_dims(so, &sw, &sh);
                if (so && !IsBadReadPtr(so, 4)) {
                    DWORD *svt = *(DWORD **)so;
                    if (!IsBadReadPtr(svt, 4))
                        { mod_name_of(svt, sb, sizeof(sb)); smod = sb; }
                }
            }
            {
                DWORD *d = (DWORD *)args[0];
                if (d && !IsBadReadPtr(d, 4)) {
                    DWORD *dvt = *(DWORD **)d;
                    if (!IsBadReadPtr(dvt, 4))
                        { mod_name_of(dvt, db, sizeof(db)); dmod = db; }
                }
            }
            logfmt("bbcall: s%02x args=%x %x %x %x %x %x %x %x"
                   " srcdims=%ux%u dstdims=%ux%u"
                   " srcmod=%s dstmod=%s",
                   slot & 0xffff, args[0], args[1], args[2], args[3],
                   args[4], args[5], args[6], args[7], sw, sh,
                   dbw, dbh, smod, dmod);
        }
    }
    }
    slot &= 0xffff;
    /* SurfaceFactory: ゲーム側が論理サイズで要求する中間 surface
       (800x600) を物理サイズに書き換える。
       s0c = CreateSurface(w, h, bpp, ...) と推定 */
    if (g_renderscale > 1 &&
        g_slots[ctx].is_graph == 2 && slot == 0x0c &&
        args[0] == GAME_W && args[1] == GAME_H) {
        logfmt("sf: create %ux%u -> %ux%u", args[0], args[1],
               GAME_W * g_renderscale, GAME_H * g_renderscale);
        args[0] = GAME_W * g_renderscale;
        args[1] = GAME_H * g_renderscale;
    }
    if (!g_slots[ctx].is_graph || g_renderscale < 2)
        return;
    switch (slot) {
    case 0x10: case 0x14: case 0x28: case 0x38: case 0x48: {
        int di = (slot == 0x10) ? 4 : 1;
        DWORD bw = GAME_W * g_renderscale;
        DWORD bh = GAME_H * g_renderscale;
        DWORD dw = bw, dh = bh;
        DWORD *d = (DWORD *)args[0];
        int big = (d == g_bb_surf);
        if (!big)
            big = surf_dims(d, &dw, &dh) && dw >= bw && dh >= bh;
        if (!big) {
            /* 小さい dst へのはみ出しコピーだけクランプ */
            if (surf_dims(d, &dw, &dh)) {
                if (args[6] > dw) args[6] = dw;
                if (args[7] > dh) args[7] = dh;
            }
            return;
        }
        if (args[di] < GAME_W && args[di + 1] < GAME_H) {
            args[di] *= g_renderscale;
            args[di + 1] *= g_renderscale;
        }
        if (args[di] + args[6] > dw)
            args[6] = (args[di] < dw) ? dw - args[di] : 0;
        if (args[di + 1] + args[7] > dh)
            args[7] = (args[di + 1] < dh) ? dh - args[di + 1] : 0;
        return;
    }
    case 0x54: case 0x58: case 0x5c: {
        DWORD bw = GAME_W * g_renderscale;
        DWORD bh = GAME_H * g_renderscale;
        DWORD dw = bw, dh = bh;
        DWORD *d = (DWORD *)args[0];
        int big = (d == g_bb_surf);
        if (!big)
            big = surf_dims(d, &dw, &dh) && dw >= bw && dh >= bh;
        if (!big)
            return;
        if (args[1] < GAME_W && args[2] < GAME_H &&
            args[3] <= GAME_W && args[4] <= GAME_H) {
            args[1] *= g_renderscale;
            args[2] *= g_renderscale;
            args[3] *= g_renderscale;
            args[4] *= g_renderscale;
        }
        if (args[1] + args[3] > dw)
            args[3] = (args[1] < dw) ? dw - args[1] : 0;
        if (args[2] + args[4] > dh)
            args[4] = (args[2] < dh) ? dh - args[2] : 0;
        return;
    }
    }
}

/* ctx 番号を push して共通ハンドラへ飛ばす裸スタブから呼ばれる。
   スタック: [ctxidx][ecx(this)][edx][ctx][ret][args...] */
__declspec(naked) static void hook_rd_common(void)
{
    __asm {
        push edx                    /* edx 保存 */
        push ecx                    /* ecx(this) 保存 */
        mov eax, [esp + 8]          /* ctx idx */
        push eax
        call rd_lookup              /* eax = 元実装 */
        add esp, 4
        push eax                    /* [orig][ecx][edx][ctx][ret][args] */
        lea eax, [esp + 0x14]       /* args 先頭 */
        push eax                    /* args */
        mov eax, [esp + 0x14]       /* リターンアドレス */
        push eax                    /* ret */
        push dword ptr [esp + 0x0c] /* self (保存済み ecx) */
        push dword ptr [esp + 0x18] /* ctx 番号 */
        call xform_rd
        add esp, 16
        pop eax                     /* orig */
        pop ecx
        pop edx
        add esp, 4                  /* ctx を捨てる */
        jmp eax
    }
}

/* ctx 埋め込みスタブを生成: push imm32 ; jmp rel32 (10 バイト) */
static void *make_stub(int ctx)
{
    BYTE *p;
    if (!g_stub_pool) {
        g_stub_pool = (BYTE *)VirtualAlloc(NULL, 0x1000,
                                           MEM_COMMIT | MEM_RESERVE,
                                           PAGE_EXECUTE_READWRITE);
        if (!g_stub_pool)
            return NULL;
    }
    if (g_stub_used + 10 > 0x1000)
        return NULL;
    p = g_stub_pool + g_stub_used;
    p[0] = 0x68;
    *(DWORD *)(p + 1) = (DWORD)ctx;
    p[5] = 0xe9;
    *(DWORD *)(p + 6) =
        (DWORD)((BYTE *)hook_rd_common - (p + 10));
    g_stub_used += 10;
    return p;
}

/* Graph.dll レンダラーの vtable を全スロットフックする */
static void patch_renderer_vtable(DWORD *rvt, int is_graph)
{
    DWORD old;
    int nbytes = VT_MAX_BYTES;
    int k;
    if (IsBadReadPtr(rvt, nbytes))
        nbytes = 0x58;
    if (IsBadReadPtr(rvt, 4))
        return;
    if (!VirtualProtect(rvt, nbytes, PAGE_READWRITE, &old))
        return;
    for (k = 0; k < nbytes / 4; k++) {
        DWORD fn = rvt[k];
        void *stub;
        int dup = 0, i;
        if (IsBadCodePtr((FARPROC)fn))
            continue;
        if (g_stub_pool &&
            fn >= (DWORD)g_stub_pool &&
            fn < (DWORD)g_stub_pool + g_stub_used)
            continue;
        for (i = 0; i < g_slots_n; i++)
            if (g_slots[i].vt == (DWORD)rvt &&
                g_slots[i].slot == (DWORD)(k * 4))
                dup = 1;
        if (dup)
            continue;
        if (g_slots_n >= MAX_RD_SLOTS)
            break;
        stub = make_stub(g_slots_n);
        if (!stub)
            break;
        g_slots[g_slots_n].vt = (DWORD)rvt;
        g_slots[g_slots_n].slot = k * 4;
        g_slots[g_slots_n].orig = fn;
        g_slots[g_slots_n].is_graph = is_graph;
        rvt[k] = (DWORD)stub;
        g_slots_n++;
    }
    VirtualProtect(rvt, nbytes, old, &old);
    FlushInstructionCache(GetCurrentProcess(), rvt, nbytes);
    {
        char mb[MAX_PATH];
        HMODULE mvt = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)rvt, &mvt);
        logfmt("hd: vtable %s+0x%x hooked (%d slots)",
               mvt ? mod_name_of(rvt, mb, sizeof(mb)) : "?",
               mvt ? (DWORD)rvt - (DWORD)mvt : (DWORD)rvt,
               g_slots_n);
    }
}

/* --- クラッシュロガー ----------------------------------------------------- */

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    char mb[MAX_PATH];
    HMODULE m = NULL;
    DWORD eip;
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode != 0xC0000005 &&
        ep->ExceptionRecord->ExceptionCode != 0xC00000FD &&
        ep->ExceptionRecord->ExceptionCode != 0xC000001D)
        return EXCEPTION_CONTINUE_SEARCH;
    eip = ep->ContextRecord->Eip;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)eip, &m);
    /* KERNEL32!RtlMoveMemory 内 (0x1ca5e) の例外は IsBadReadPtr 系
       プローブ由来のノイズが殆どなので捨てる */
    {
        HMODULE km = GetModuleHandleA("KERNEL32.dll");
        if (km && eip == (DWORD)km + 0x1ca5e)
            return EXCEPTION_CONTINUE_SEARCH;
    }
    /* (EIP, 違反アドレス) の組で重複排除 */
    {
        static DWORD seen_eip[64], seen_addr[64];
        static int sn;
        int i;
        DWORD fa =
            (DWORD)ep->ExceptionRecord->ExceptionInformation[1];
        for (i = 0; i < sn; i++)
            if (seen_eip[i] == eip && seen_addr[i] == fa)
                return EXCEPTION_CONTINUE_SEARCH;
        if (sn < 64) {
            seen_eip[sn] = eip;
            seen_addr[sn] = fa;
            sn++;
        }
    }
    logfmt("CRASH: code=%x addr=%p eip=%s+0x%x esp=%x eax=%x ebx=%x"
           " ecx=%x edx=%x esi=%x edi=%x ebp=%x",
           ep->ExceptionRecord->ExceptionCode,
           (void *)ep->ExceptionRecord->ExceptionInformation[1],
           m ? mod_name_of((void *)eip, mb, sizeof(mb)) : "?",
           m ? eip - (DWORD)m : eip,
           ep->ContextRecord->Esp, ep->ContextRecord->Eax,
           ep->ContextRecord->Ebx, ep->ContextRecord->Ecx,
           ep->ContextRecord->Edx, ep->ContextRecord->Esi,
           ep->ContextRecord->Edi, ep->ContextRecord->Ebp);
    if (g_last_slot >= 0)
        logfmt("CRASH-CTX: slot=%02x self=%p args=%x %x %x %x %x %x %x %x",
               g_last_slot, (void *)g_last_self,
               g_last_args[0], g_last_args[1], g_last_args[2],
               g_last_args[3], g_last_args[4], g_last_args[5],
               g_last_args[6], g_last_args[7]);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI hd_probe_thread(LPVOID lp)
{
    int i;
    int rd_hooked = 0;
    (void)lp;
    /* SACT2.dll がロードされ sprite コンポーネント供給元が
       初期化されるのを待つ */
    for (i = 0; i < 600 && (!g_bb_surf || !rd_hooked); i++) {
        HMODULE sact2 = GetModuleHandleA("SACT2.dll");
        if (sact2) {
            DWORD *supplier =
                *(DWORD **)((BYTE *)sact2 + SACT2_SUPPLIER_OFF);
            DWORD *comp;
            if (supplier && !IsBadReadPtr(supplier, 0xa0)) {
                comp = *(DWORD **)((BYTE *)supplier + SPRITE_COMP_OFF);
                if (comp) {
                    /* comp[0x24] = 転送先 (バックバッファ) surface */
                    if (!g_bb_surf &&
                        !IsBadReadPtr((BYTE *)comp + 0x24, 4)) {
                        DWORD *bb =
                            *(DWORD **)((BYTE *)comp + 0x24);
                        if (bb) {
                            logfmt("hd: bb candidate comp[0x24]=%p",
                                   bb);
                            setup_hd_backbuffer(bb, g_renderscale);
                        }
                    }
                    /* comp[0x24..0x40] のオブジェクトを列挙し、
                       Graph.dll 製 vtable (描画レンダラー) をフック */
                    if (!rd_hooked &&
                        !IsBadReadPtr((BYTE *)comp + 0x44, 4)) {
                        {
                            int f;
                            for (f = 0x24; f <= 0x40; f += 4) {
                                DWORD *obj =
                                    *(DWORD **)((BYTE *)comp + f);
                                DWORD *vt;
                                HMODULE vm = NULL;
                                char mb[MAX_PATH], vn[64];
                                if (!obj || IsBadReadPtr(obj, 4))
                                    continue;
                                vt = *(DWORD **)obj;
                                if (!vt)
                                    continue;
                                GetModuleHandleExA(
                                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    (LPCSTR)vt, &vm);
                                lstrcpynA(vn,
                                          vm ? mod_name_of(vt, mb,
                                                           sizeof(mb))
                                             : "",
                                          sizeof(vn));
                                logfmt("hd: comp[%02x]=%p vt=%s+0x%x",
                                       f, obj, vm ? vn : "?",
                                       vm ? (DWORD)vt - (DWORD)vm
                                          : (DWORD)vt);
                                if (lstrcmpiA(vn, "SurfaceFactory.dll")
                                        == 0)
                                    patch_renderer_vtable(vt, 2);
                                if (lstrcmpiA(vn, "ALDLoader.dll") == 0 ||
                                    lstrcmpiA(vn, "DecodeCG.dll") == 0 ||
                                    lstrcmpiA(vn, "DecodeCgEx.dll") == 0 ||
                                    lstrcmpiA(vn, "DrawPluginManager.dll")
                                        == 0 ||
                                    lstrcmpiA(vn, "SactEngine.dll") == 0)
                                    patch_renderer_vtable(vt, 0);
                                if (lstrcmpiA(vn, "Graph.dll") == 0) {
                                    int k;
                                    /* レンダラーオブジェクト内部の
                                       クリップ矩形 (0,0,800,600) を
                                       探すため全フィールドをダンプ */
                                    if (!IsBadReadPtr(obj, 0x100))
                                        for (k = 0; k < 0x100;
                                             k += 0x10)
                                            logfmt(
                                              "hd: rd+%02x: %08x %08x"
                                              " %08x %08x", k,
                                              obj[k/4], obj[k/4+1],
                                              obj[k/4+2], obj[k/4+3]);
                                    patch_renderer_vtable(vt, 1);
                                    rd_hooked = 1;
                                }
                            }
                        }
                    }
                }
            }
        }
        Sleep(10);
    }
    /* 10 秒毎に bb の内容をダンプする */
    for (;;) {
        Sleep(10000);
        dump_bb_bmp();
    }
    return 0;
}

BOOL WINAPI DllMainCRTStartup(HINSTANCE inst, DWORD reason, void *res)
{
    (void)inst; (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = inst;
        /* The game is DPI-unaware. On high-DPI monitors (e.g. 4K at
           150%) Windows would bitmap-scale the whole window, blurring
           the output. Declare per-monitor awareness early so monitor
           rects and window metrics are real pixels and our scaler
           renders at true output resolution. */
        if (!SetProcessDpiAwarenessContext(
                DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
            SetProcessDPIAware();
        logmsg("dinput proxy loaded");
        if (!g_hooked) {
            g_hooked = 1;
            SetUnhandledExceptionFilter(crash_filter);
            AddVectoredExceptionHandler(1,
                (PVECTORED_EXCEPTION_HANDLER)crash_filter);
            load_config();
            hook_iat();
            if (g_renderscale > 1)
                CreateThread(NULL, 0, hd_probe_thread, NULL, 0, NULL);
        }
    }
    return TRUE;
}
