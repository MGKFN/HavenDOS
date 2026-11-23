/* ════════════════════════════════════════════════════════════════════
   retropc/host_x11.c -- Retro PC's window on Linux.  v0.9.7.5.

   Plain Xlib, because every Linux desktop has it -- X.org itself, or
   XWayland under a Wayland session -- and so the program needs no
   library a user would have to install. The picture is scaled to the
   window in software, nearest-neighbour, keeping its 16:10 shape with
   black bars, and only sent when it changed (retropc.c checks).

   Alt+Enter: full screen and back, as in a Windows program.
   ════════════════════════════════════════════════════════════════════ */
#define _DEFAULT_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "retropc.h"
#include "keys.h"

static Display *dpy;
static Window   win;
static GC       gc;
static XImage  *img;
static int      ww, wh;                 /* the window's size          */
static Atom     a_delete, a_state, a_full;
static int      full;
static uint32_t frame[RP_W * RP_H];
static int      have_frame;

#define KQ 64
static int kq[KQ], kh, kt;
static void kpush(int k){ int n = (kt + 1) % KQ; if(n != kh){ kq[kt] = k; kt = n; } }

static void make_image(void){
    char *buf;
    if(img){ XDestroyImage(img); img = 0; }   /* frees its buffer too */
    if(ww < 1 || wh < 1) return;
    buf = (char *)calloc((size_t)ww * (size_t)wh, 4);
    if(!buf) return;
    img = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), (unsigned)DefaultDepth(dpy, DefaultScreen(dpy)),
                       ZPixmap, 0, buf, (unsigned)ww, (unsigned)wh, 32, 0);
    if(!img) free(buf);
}

/* the frame into the window: as large as fits, 16:10, centred */
static void draw(void){
    int dw, dh, ox, oy;
    uint32_t *o;
    if(!img || !have_frame) return;
    dw = ww; dh = ww * RP_H / RP_W;
    if(dh > wh){ dh = wh; dw = wh * RP_W / RP_H; }
    /* a whole multiple when one fits: no uneven pixels on an LCD */
    if(dw >= RP_W){ int s = dw / RP_W; dw = RP_W * s; dh = RP_H * s; }
    ox = (ww - dw) / 2; oy = (wh - dh) / 2;
    o = (uint32_t *)img->data;
    memset(o, 0, (size_t)ww * (size_t)wh * 4);
    for(int y = 0; y < dh; y++){
        const uint32_t *src = frame + (y * RP_H / dh) * RP_W;
        uint32_t *dst = o + (size_t)(oy + y) * (size_t)ww + ox;
        for(int x = 0; x < dw; x++) dst[x] = src[x * RP_W / dw];
    }
    XPutImage(dpy, win, gc, img, 0, 0, 0, 0, (unsigned)ww, (unsigned)wh);
    XFlush(dpy);
}

static void toggle_full(void){
    XEvent e;
    memset(&e, 0, sizeof e);
    full = !full;
    e.xclient.type = ClientMessage;
    e.xclient.window = win;
    e.xclient.message_type = a_state;
    e.xclient.format = 32;
    e.xclient.data.l[0] = full ? 1 : 0;           /* _NET_WM_STATE_ADD / REMOVE */
    e.xclient.data.l[1] = (long)a_full;
    XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    XFlush(dpy);
}

static int map_key(XKeyEvent *ev){
    char buf[8];
    KeySym ks;
    int n = XLookupString(ev, buf, sizeof buf, &ks, 0);
    switch(ks){
    case XK_Return: case XK_KP_Enter:
        if(ev->state & Mod1Mask){ toggle_full(); return RP_KEY_NONE; }
        return '\n';
    case XK_BackSpace: return 8;
    case XK_Tab: case XK_ISO_Left_Tab: return 9;
    case XK_Escape: return 27;
    case XK_Up:    case XK_KP_Up:    return KEY_UP;
    case XK_Down:  case XK_KP_Down:  return KEY_DOWN;
    case XK_Left:  case XK_KP_Left:  return KEY_LEFT;
    case XK_Right: case XK_KP_Right: return KEY_RIGHT;
    case XK_Home:  case XK_KP_Home:  return KEY_HOME;
    case XK_End:   case XK_KP_End:   return KEY_END;
    case XK_Prior: case XK_KP_Prior: return KEY_PGUP;
    case XK_Next:  case XK_KP_Next:  return KEY_PGDN;
    case XK_Insert: case XK_KP_Insert: return KEY_INS;
    case XK_Delete: case XK_KP_Delete: return KEY_DEL;
    case XK_F11: return KEY_EXT(0x57);
    case XK_F12: return KEY_EXT(0x58);
    }
    if(ks >= XK_F1 && ks <= XK_F10) return KEY_EXT(0x3B + (int)(ks - XK_F1));
    if(n == 1 && (unsigned char)buf[0] < 0x80) return (unsigned char)buf[0];
    return RP_KEY_NONE;
}

static void pump(void){
    while(dpy && XPending(dpy)){
        XEvent e;
        XNextEvent(dpy, &e);
        switch(e.type){
        case Expose: if(e.xexpose.count == 0) draw(); break;
        case ConfigureNotify:
            if(e.xconfigure.width != ww || e.xconfigure.height != wh){
                ww = e.xconfigure.width; wh = e.xconfigure.height;
                make_image(); draw();
            }
            break;
        case KeyPress: { int k = map_key(&e.xkey); if(k != RP_KEY_NONE) kpush(k); break; }
        case ClientMessage:
            if((Atom)e.xclient.data.l[0] == a_delete) kpush(RP_KEY_QUIT);
            break;
        }
    }
}

int host_open(const char *title){
    int scr, sw, sh, s = 1;
    XSizeHints hints;
    dpy = XOpenDisplay(0);
    if(!dpy){
        fprintf(stderr, "Retro PC needs a display, and there is none (DISPLAY is %s).\n"
                        "Without one, retropc-headless runs the same program and prints the screen.\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "not set");
        return -1;
    }
    scr = DefaultScreen(dpy);
    if(DefaultDepth(dpy, scr) < 24){
        fprintf(stderr, "Retro PC needs a 24-bit display; this one is %d-bit.\n", DefaultDepth(dpy, scr));
        return -1;
    }
    sw = DisplayWidth(dpy, scr); sh = DisplayHeight(dpy, scr);
    while(RP_W * (s + 1) <= sw * 9 / 10 && RP_H * (s + 1) <= sh * 9 / 10 && s < 3) s++;
    ww = RP_W * s; wh = RP_H * s;
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, 0, (unsigned)ww, (unsigned)wh, 0, 0, BlackPixel(dpy, scr));
    XStoreName(dpy, win, title);
    {
        XClassHint ch = { (char *)"retropc", (char *)"RetroPC" };
        XSetClassHint(dpy, win, &ch);
    }
    memset(&hints, 0, sizeof hints);
    hints.flags = PMinSize;
    hints.min_width = RP_W / 2; hints.min_height = RP_H / 2;
    XSetWMNormalHints(dpy, win, &hints);
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | StructureNotifyMask);
    a_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    a_state  = XInternAtom(dpy, "_NET_WM_STATE", False);
    a_full   = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    XSetWMProtocols(dpy, win, &a_delete, 1);
    gc = XCreateGC(dpy, win, 0, 0);
    make_image();
    XMapWindow(dpy, win);
    XFlush(dpy);
    return img ? 0 : -1;
}

void host_present(const uint32_t *px){
    memcpy(frame, px, sizeof frame);
    have_frame = 1;
    draw();
}

int host_key(void){
    int k;
    pump();
    if(kh == kt) return RP_KEY_NONE;
    k = kq[kh]; kh = (kh + 1) % KQ;
    return k;
}

uint32_t host_ms(void){
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u);
}

void host_sleep(uint32_t ms){
    struct timespec t = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&t, 0);
}

void host_title(const char *t){ if(dpy){ XStoreName(dpy, win, t); XFlush(dpy); } }

void host_close(void){
    if(!dpy) return;
    if(img) XDestroyImage(img);
    img = 0;
    XCloseDisplay(dpy);
    dpy = 0;
}

/* zenity, when the desktop has it; nothing pretends otherwise */
int host_pick_file(char *out, int max){
    FILE *f;
    int n;
    if(system("command -v zenity >/dev/null 2>&1") != 0) return -1;
    f = popen("zenity --file-selection --title='Retro PC: a floppy or CD image' "
              "--file-filter='Disk images | *.img *.IMG *.ima *.IMA *.iso *.ISO' "
              "--file-filter='All files | *' 2>/dev/null", "r");
    if(!f) return -1;
    if(!fgets(out, max, f)){ pclose(f); return 1; }
    pclose(f);
    n = (int)strlen(out);
    while(n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    return n ? 0 : 1;
}

const char *host_dropped(void){ return 0; }

int main(int argc, char **argv){ return rp_main(argc, argv); }
