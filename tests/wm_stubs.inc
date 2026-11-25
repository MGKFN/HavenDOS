/* tests/wm_stubs.inc -- the window manager, OFF (v0.9.8).
 *
 * drivers/mouse.c, kernel/task.c, shell/chrome.c, shell/uimouse.c and
 * shell/ui.c call into drivers/wm_core.inc, which lives inside
 * drivers/vga.c. A suite that compiles those files without vga.c gets
 * these: the window manager is off, so every caller takes the path it
 * took before 0.9.8 -- which is what these suites were written against.
 * Weak, so a suite that does compile vga.c gets the real ones. tests/
 * test_wm.c is where the window manager itself is tested. */
#define WM_W __attribute__((weak))
WM_W int  vga_wm_on(void){ return 0; }
WM_W void vga_wm_front(int s){ (void)s; }
WM_W int  vga_wm_cur_map(int x, int y, int *cx, int *cy){ (void)x; (void)y; (void)cx; (void)cy; return -1; }
WM_W int  vga_wm_click_is_app(int x, int y){ (void)x; (void)y; return 1; }
WM_W void vga_mouse_px(int c, int r, int fx, int fy, int *x, int *y){ *x = c * 8 + fx; *y = r * 16 + fy; }
WM_W void vga_wm_solo(int on){ (void)on; }
WM_W void vga_wm_full(int s, int on){ (void)s; (void)on; }
WM_W void vga_wm_desk_icons_begin(void){ }
WM_W void vga_wm_desk_icon(int a, int b, int c, int d, const char *i, const char *n, int s){ (void)a;(void)b;(void)c;(void)d;(void)i;(void)n;(void)s; }
WM_W void vga_wm_desk_icons_end(void){ }
WM_W void vga_wm_open(int s, const char *t, const char *i){ (void)s; (void)t; (void)i; }
WM_W void vga_wm_close(int s){ (void)s; }
WM_W void vga_wm_geometry(int s, int a, int b, int c, int d, int x, int y, int m){ (void)s;(void)a;(void)b;(void)c;(void)d;(void)x;(void)y;(void)m; }
WM_W int  vga_wm_is_open(int s){ (void)s; return 0; }
WM_W int  vga_wm_is_min(int s){ (void)s; return 0; }
WM_W int  vga_wm_title_rows(void){ return 1; }
#ifndef WM_HAVE_CHROME
WM_W void wm_pointer_update(void){ }
#endif
WM_W const char *desktop_app_icon_id(const char *n){ (void)n; return "_app"; }
WM_W void vga_wm_get(int s, int *x, int *y, int *w, int *h){ (void)s; if(x)*x=0; if(y)*y=0; if(w)*w=0; if(h)*h=0; }
WM_W void vga_wm_move(int s, int x, int y){ (void)s; (void)x; (void)y; }
WM_W void vga_wm_minimize(int s, int on){ (void)s; (void)on; }
WM_W void vga_wm_buttons(int s, int h, int d){ (void)s; (void)h; (void)d; }
WM_W int  vga_wm_hit(int x, int y, int *s, int *p){ (void)x; (void)y; if(s)*s=0; if(p)*p=1; return 1; }
WM_W int  vga_wm_front_slot(void){ return 0; }
WM_W void vga_wm_repaint_bar(void){ }
WM_W int  vga_screen_w(void){ return 640; }
WM_W int  vga_screen_h(void){ return 400; }
WM_W int  vga_wm_title_h(void){ return 16; }
WM_W int  vga_wm_bar_h(void){ return 16; }
WM_W int  taskbar_hit(int x, int y, int *i){ (void)x; (void)y; (void)i; return 0; }
WM_W int  taskbar_hover(int x, int y){ (void)x; (void)y; return 0; }
WM_W uint32_t taskbar_sig(void){ return 0; }
WM_W void startmenu_run(void){ }
WM_W void notif_flyout_run(void){ }
WM_W int  vga_cell_w(void){ return 8; }
WM_W int  vga_cell_h(void){ return 16; }
#ifndef WM_HAVE_MOUSE
WM_W void mouse_get_px(int *x, int *y){ *x = 0; *y = 0; }
WM_W int  mouse_buttons_raw(void){ return 0; }
WM_W int  mouse_take_wm_clicks(int *x, int *y){ (void)x; (void)y; return 0; }
WM_W void mouse_repost_clicks(int e){ (void)e; }
#endif
WM_W int  desktop_open_app(const char *id){ (void)id; return 0; }
#undef WM_W
