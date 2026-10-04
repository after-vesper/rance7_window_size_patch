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
 * Config: MultiRes.ini next to the game exe
 *   [Display]
 *   Width  = 1600
 *   Height = 1200
 *   Filter = 1    ; 0 = nearest (COLORONCOLOR), 1 = smooth (HALFTONE)
 *   ScaleMode = 1 ; 0 = stretch, 1 = keep 4:3 (fractional scale),
 *                 ; 2 = largest integer scale (avoids jitter)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#define GAME_W 800
#define GAME_H 600

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

typedef BOOL (WINAPI *PFN_BitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);
typedef BOOL (WINAPI *PFN_ScreenToClient)(HWND, LPPOINT);
typedef BOOL (WINAPI *PFN_SetCursorPos)(int, int);
typedef BOOL (WINAPI *PFN_GetCursorPos)(LPPOINT);
typedef BOOL (WINAPI *PFN_ClientToScreen)(HWND, LPPOINT);
static PFN_BitBlt g_origBitBlt;
static PFN_ScreenToClient g_origScreenToClient;
static PFN_SetCursorPos g_origSetCursorPos;
static PFN_GetCursorPos g_origGetCursorPos;
static PFN_ClientToScreen g_origClientToScreen;

static void logmsg(const char *s)
{
    HANDLE f;
    if (!g_log)
        return;
    f = CreateFileA("multires_log.txt", GENERIC_WRITE, 0, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD n;
    if (f == INVALID_HANDLE_VALUE) return;
    SetFilePointer(f, 0, NULL, FILE_END);
    WriteFile(f, s, lstrlenA(s), &n, NULL);
    WriteFile(f, "\r\n", 2, &n, NULL);
    CloseHandle(f);
}

static void load_config(void)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    while (n && path[n - 1] != '\\') n--;
    path[n] = 0;
    lstrcatA(path, "MultiRes.ini");
    lstrcpyA(g_iniPath, path);
    g_outW = GetPrivateProfileIntA("Display", "Width", GAME_W, path);
    g_outH = GetPrivateProfileIntA("Display", "Height", GAME_H, path);
    g_smooth = GetPrivateProfileIntA("Display", "Filter", 1, path);
    g_mode = GetPrivateProfileIntA("Display", "ScaleMode", 1, path);
    g_full = GetPrivateProfileIntA("Display", "Fullscreen", 0, path);
    g_log = GetPrivateProfileIntA("Debug", "Log", 0, path);
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
   MultiRes.ini so the same size is used on the next launch. */
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

static void ensure_backbuffer(HDC dst)
{
    if (g_backDC)
        return;
    g_backDC = CreateCompatibleDC(dst);
    g_backBmp = CreateCompatibleBitmap(dst, GAME_W, GAME_H);
    g_backOld = SelectObject(g_backDC, g_backBmp);
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
                                L"\x30A6\x30A3\x30F3\x30C9\x30A6"
                                L"\x30B5\x30A4\x30BA(&Z)...");
                }
            }
        }
        if (w && g_hwnd && w == g_hwnd && g_enabled &&
            !native_fullscreen()) {
            RECT vp;
            ensure_window_size();
            ensure_backbuffer(dst);
            get_viewport(&vp);
            /* Mirror the update into the logical back buffer. */
            g_origBitBlt(g_backDC, x, y, cx, cy, src, sx, sy, rop);
            /* Paint the letterbox/pillarbox bars black. The class
               background brush can erase them, so repaint each frame. */
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
            SetStretchBltMode(dst, g_smooth ? HALFTONE : COLORONCOLOR);
            if (g_smooth)
                /* Keep a fixed halftone phase: per-blit origins shift
                   the dither pattern and show up as shimmer. */
                SetBrushOrgEx(dst, 0, 0, NULL);
            return StretchBlt(dst, vp.left, vp.top,
                              vp.right - vp.left, vp.bottom - vp.top,
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
    FARPROC realBitBlt, realS2C, realSCP, realGCP;

    gdi = GetModuleHandleA("GDI32.dll");
    u32 = GetModuleHandleA("USER32.dll");
    realBitBlt = gdi ? GetProcAddress(gdi, "BitBlt") : NULL;
    realS2C = u32 ? GetProcAddress(u32, "ScreenToClient") : NULL;
    realSCP = u32 ? GetProcAddress(u32, "SetCursorPos") : NULL;
    realGCP = u32 ? GetProcAddress(u32, "GetCursorPos") : NULL;
    if (realBitBlt) g_origBitBlt = (PFN_BitBlt)realBitBlt;
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
            load_config();
            hook_iat();
        }
    }
    return TRUE;
}
