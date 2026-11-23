/* ════════════════════════════════════════════════════════════════════
   retropc/host_win32.c -- Retro PC's window on Windows.  v0.9.7.5.

   One RetroPC.exe and nothing beside it: built with mingw-w64 and
   linked statically, it calls only what every Windows since Vista
   has -- kernel32, user32, gdi32, shell32, comdlg32, winmm. Kyan runs
   Windows without git or a compiler; a program he has to install
   something for is a program he cannot use.

     double-click            the list
     drop a file on the .exe boots it (Windows passes it as argv[1])
     drop a file on the list boots it
     O in the list           Windows' own Open dialog
     Alt+Enter               full screen and back
     F4 inside an old PC     back to the list, as in HavenDOS

   Started from a Command Prompt, it attaches to that console, so
   `RetroPC --help` prints and the log lines are visible.
   ════════════════════════════════════════════════════════════════════ */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>
#include "retropc.h"
#include "keys.h"

static HWND     g_wnd;
static uint32_t g_frame[RP_W * RP_H];
static int      g_have;
static int      g_full;
static WINDOWPLACEMENT g_place;
static char     g_drop[MAX_PATH];

#define KQ 64
static int kq[KQ], kh, kt;
static void kpush(int k){ int n = (kt + 1) % KQ; if(n != kh){ kq[kt] = k; kt = n; } }

static void paint(HDC dc){
    RECT rc;
    int ww, wh, dw, dh, ox, oy;
    BITMAPINFO bi;
    GetClientRect(g_wnd, &rc);
    ww = rc.right; wh = rc.bottom;
    dw = ww; dh = ww * RP_H / RP_W;
    if(dh > wh){ dh = wh; dw = wh * RP_W / RP_H; }
    if(dw >= RP_W){ int s = dw / RP_W; dw = RP_W * s; dh = RP_H * s; }
    ox = (ww - dw) / 2; oy = (wh - dh) / 2;
    /* the bars around it */
    PatBlt(dc, 0, 0, ww, oy, BLACKNESS);
    PatBlt(dc, 0, oy + dh, ww, wh - oy - dh, BLACKNESS);
    PatBlt(dc, 0, oy, ox, dh, BLACKNESS);
    PatBlt(dc, ox + dw, oy, ww - ox - dw, dh, BLACKNESS);
    if(!g_have) { PatBlt(dc, ox, oy, dw, dh, BLACKNESS); return; }
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = RP_W;
    bi.bmiHeader.biHeight = -RP_H;            /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, COLORONCOLOR);       /* nearest: sharp pixels */
    StretchDIBits(dc, ox, oy, dw, dh, 0, 0, RP_W, RP_H, g_frame, &bi, DIB_RGB_COLORS, SRCCOPY);
}

static void toggle_full(void){
    DWORD st = (DWORD)GetWindowLongA(g_wnd, GWL_STYLE);
    if(!g_full){
        MONITORINFO mi;
        memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi; g_place.length = sizeof g_place;
        if(GetWindowPlacement(g_wnd, &g_place) && GetMonitorInfoA(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTOPRIMARY), &mi)){
            SetWindowLongA(g_wnd, GWL_STYLE, (LONG)(st & ~WS_OVERLAPPEDWINDOW));
            SetWindowPos(g_wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_full = 1;
        }
    } else {
        SetWindowLongA(g_wnd, GWL_STYLE, (LONG)(st | WS_OVERLAPPEDWINDOW));
        SetWindowPlacement(g_wnd, &g_place);
        SetWindowPos(g_wnd, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_full = 0;
    }
    InvalidateRect(g_wnd, 0, FALSE);
}

/* The keys WM_CHAR does not bring, as HavenDOS codes: 0x100 | the
   set-1 scancode, which is what HavenDOS's own drivers deliver. */
static int vk_code(WPARAM vk){
    switch(vk){
    case VK_UP: return KEY_UP;      case VK_DOWN: return KEY_DOWN;
    case VK_LEFT: return KEY_LEFT;  case VK_RIGHT: return KEY_RIGHT;
    case VK_HOME: return KEY_HOME;  case VK_END: return KEY_END;
    case VK_PRIOR: return KEY_PGUP; case VK_NEXT: return KEY_PGDN;
    case VK_INSERT: return KEY_INS; case VK_DELETE: return KEY_DEL;
    case VK_F11: return KEY_EXT(0x57);
    case VK_F12: return KEY_EXT(0x58);
    }
    if(vk >= VK_F1 && vk <= VK_F10) return KEY_EXT(0x3B + (int)(vk - VK_F1));
    return RP_KEY_NONE;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l){
    switch(m){
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint(dc);
        EndPaint(h, &ps);
        return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(h, 0, FALSE); return 0;
    case WM_CLOSE: kpush(RP_KEY_QUIT); return 0;   /* the program saves, then closes */
    case WM_SYSKEYDOWN:
        if(w == VK_RETURN){ toggle_full(); return 0; }
        if(w == VK_F10){ kpush(KEY_EXT(0x44)); return 0; }   /* not the menu bar */
        break;
    case WM_SYSCHAR:
        if(w == '\r') return 0;                    /* no beep for Alt+Enter */
        break;
    case WM_KEYDOWN: {
        int k = vk_code(w);
        if(k != RP_KEY_NONE){ kpush(k); return 0; }
        break; }
    case WM_CHAR:
        if(w == '\r') kpush('\n');
        else if(w == 127) ;                         /* Ctrl+Backspace */
        else if(w < 0x80) kpush((int)w);
        return 0;
    case WM_DROPFILES: {
        HDROP d = (HDROP)w;
        if(DragQueryFileA(d, 0, g_drop, sizeof g_drop)) kpush(RP_KEY_DROP);
        DragFinish(d);
        return 0; }
    }
    return DefWindowProcA(h, m, w, l);
}

int host_open(const char *title){
    WNDCLASSA wc;
    RECT rc, wa;
    int s = 1;
    HINSTANCE inst = GetModuleHandleA(0);
    SetProcessDPIAware();                          /* sharp, not blurred by Windows */
    timeBeginPeriod(1);                            /* Sleep(1) sleeps 1 ms */
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(0, (LPCSTR)IDC_ARROW);
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));
    wc.lpszClassName = "RetroPC";
    if(!RegisterClassA(&wc)) return -1;
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
    while(RP_W * (s + 1) <= (wa.right - wa.left) * 9 / 10 && RP_H * (s + 1) <= (wa.bottom - wa.top) * 9 / 10 && s < 3) s++;
    rc.left = 0; rc.top = 0; rc.right = RP_W * s; rc.bottom = RP_H * s;
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_wnd = CreateWindowExA(WS_EX_ACCEPTFILES, "RetroPC", title, WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                            0, 0, inst, 0);
    if(!g_wnd) return -1;
    ShowWindow(g_wnd, SW_SHOWNORMAL);
    UpdateWindow(g_wnd);
    return 0;
}

static void pump(void){
    MSG msg;
    while(PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)){
        if(msg.message == WM_QUIT){ kpush(RP_KEY_QUIT); continue; }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

void host_present(const uint32_t *px){
    HDC dc;
    memcpy(g_frame, px, sizeof g_frame);
    g_have = 1;
    dc = GetDC(g_wnd);
    paint(dc);
    ReleaseDC(g_wnd, dc);
}

int host_key(void){
    int k;
    pump();
    if(kh == kt) return RP_KEY_NONE;
    k = kq[kh]; kh = (kh + 1) % KQ;
    return k;
}

uint32_t host_ms(void){ return (uint32_t)GetTickCount64(); }
void host_sleep(uint32_t ms){ Sleep(ms); }
void host_title(const char *t){ if(g_wnd) SetWindowTextA(g_wnd, t); }
void host_close(void){
    if(g_wnd) DestroyWindow(g_wnd);
    g_wnd = 0;
    timeEndPeriod(1);
}

int host_pick_file(char *out, int max){
    OPENFILENAMEA o;
    out[0] = 0;
    memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o;
    o.hwndOwner = g_wnd;
    o.lpstrFilter = "Disk images (*.img, *.ima, *.iso)\0*.img;*.ima;*.iso\0All files\0*.*\0";
    o.lpstrFile = out;
    o.nMaxFile = (DWORD)max;
    o.lpstrTitle = "Retro PC: a floppy or CD image";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&o) ? 0 : 1;
}

const char *host_dropped(void){ return g_drop[0] ? g_drop : 0; }

/* ── folders and files ────────────────────────────────────────────── */
const char *plat_data_dir(void){
    static char dir[MAX_PATH];
    DWORD n;
    char *sl;
    if(dir[0]) return dir;
    n = GetModuleFileNameA(0, dir, sizeof dir);
    if(n == 0 || n >= sizeof dir){ strcpy(dir, "."); return dir; }
    sl = strrchr(dir, '\\');
    if(sl) *sl = 0;
    return dir;
}

int plat_mkdir(const char *path){ return CreateDirectoryA(path, 0) ? 0 : -1; }

int plat_list(const char *dir, void (*cb)(const char *, uint64_t, void *), void *ctx){
    char pat[MAX_PATH + 4];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if(h == INVALID_HANDLE_VALUE) return -1;
    do {
        if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        cb(fd.cFileName, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, ctx);
        n++;
    } while(FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

char plat_sep(void){ return '\\'; }

/* Started from a console: print there. Started from Explorer: there is
   no console, and nothing is opened for one. */
int main(int argc, char **argv){
    if(AttachConsole(ATTACH_PARENT_PROCESS)){
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
    return rp_main(argc, argv);
}
