/* ================================================================
   shell/sysapp.c  --  the System app.  v0.9.5.

   It replaces four apps that had grown up separately and showed
   nearly the same thing four ways -- Kyan: "the system dashboards
   monitor and plus version are all almost same make it universal":

     Dashboard       (desktop.c, and a second copy in shell.c behind
                      `dashboard`/`htop`)  clock, memory, a stress test
     System Monitor  (desktop.c)          memory and uptime, refreshing
     System Info     (desktop.c)          version, memory, disk
     Info+           (packages.c)         the same, with a border
     plus `sysinfo`, which printed it once more as text, still
     announcing an "80x25 VGA Text Mode" on framebuffer machines.

   All of them were drawn on fixed 80x25 coordinates, and between them
   they still said "Multitasking: planned v0.8.x".

   This is one app with five tabs, laid out for the console it runs
   in (it is registered `fits`, so on a big panel it gets the whole
   panel), refreshing itself once a second. Every value is read from
   the subsystem that owns it at the moment it is drawn; where there
   is nothing to show, it says so instead of naming hardware.

   Opened from the desktop (System), from the shell (`system`, and the
   old names `dashboard`, `htop`, `sysinfo`, `sysinfo-plus`, `sysmon`),
   or straight onto a tab: `system network`.
   ================================================================ */
#include "types.h"
#include "string.h"
#include "vga.h"
#include "version.h"
#include "keys.h"
#include "theme.h"
#include "pci.h"
#include "usb.h"
#include "blkdev.h"
#include "netdev.h"
#include "task.h"      /* v0.9.6: the Tasks line */
#include "net.h"
#include "netstat.h"
#include "rtc.h"

extern int      keyboard_getchar(void);
extern void     sleep_ms(uint32_t);
extern uint32_t get_ticks(void);
extern uint32_t boot_ms(void);
extern uint32_t pmm_total_blocks(void);
extern uint32_t pmm_free_blocks(void);
extern void     heap_stats(uint32_t *free_bytes, uint32_t *used_bytes);
extern uint32_t heap_pool_bytes(void);
extern int      vga_fb_active(void);
extern uint32_t vga_fb_width(void);
extern uint32_t vga_fb_height(void);
extern int      vfs_using_disk(void);
extern int      vfs_using_fat16(void);
extern int      vfs_using_fat32(void);
extern int      boot_is_live(void);
extern uint32_t fat32_total_clusters(void);
extern uint32_t fat32_cluster_bytes(void);
extern uint32_t fat32_free_clusters(void);
extern int      fat32_sys_begin(void);
extern void     fat32_sys_end(void);
extern uint32_t fat16_total_clusters(void);
extern uint32_t fat16_cluster_bytes(void);
extern uint32_t fat16_free_clusters(void);
extern int      bpkg_installed_count(void);
extern const char *current_username(void);
extern int      current_user_admin(void);
extern const char *cfg_hostname(void);
extern int      i2c_hid_count(void);
extern const char *pci_describe(const pci_dev_t *d);

/* ── text helpers: no snprintf in this kernel ─────────────────────── */
typedef struct { char b[160]; int n; } sb_t;
static void sb_init(sb_t *s){ s->n=0; s->b[0]=0; }
static void sb_s(sb_t *s, const char *t){ while(t&&*t&&s->n<(int)sizeof s->b-1) s->b[s->n++]=*t++; s->b[s->n]=0; }
static void sb_u(sb_t *s, uint32_t v){
    char t[12]; int k=0;
    do{ t[k++]=(char)('0'+v%10); v/=10; }while(v&&k<11);
    while(k--&&s->n<(int)sizeof s->b-1) s->b[s->n++]=t[k];
    s->b[s->n]=0;
}
static void sb_2(sb_t *s, uint32_t v){ if(v<10) sb_s(s,"0"); sb_u(s,v); }
static void sb_hex(sb_t *s, uint32_t v, int digits){
    static const char hx[]="0123456789abcdef";
    for(int i=digits-1;i>=0;i--){ if(s->n<(int)sizeof s->b-1) s->b[s->n++]=hx[(v>>(i*4))&15]; }
    s->b[s->n]=0;
}
static void sb_ip(sb_t *s, uint32_t ip){
    sb_u(s, ip&0xFF); sb_s(s,"."); sb_u(s,(ip>>8)&0xFF); sb_s(s,".");
    sb_u(s,(ip>>16)&0xFF); sb_s(s,"."); sb_u(s,(ip>>24)&0xFF);
}
/* KB as "512 KB", "12.5 MB", "7.4 GB" -- 32-bit arithmetic only. */
static void sb_kb(sb_t *s, uint32_t kb){
    if(kb < 1024){ sb_u(s,kb); sb_s(s," KB"); return; }
    uint32_t mb=kb/1024;
    if(mb < 1024){
        sb_u(s,mb);
        if(mb<100){ sb_s(s,"."); sb_u(s,((kb%1024)*10)/1024); }
        sb_s(s," MB"); return;
    }
    sb_u(s,mb/1024); sb_s(s,"."); sb_u(s,((mb%1024)*10)/1024); sb_s(s," GB");
}

/* ── the screen ───────────────────────────────────────────────────── */
static int W, H, Y;                 /* grid, and the next body row  */
static int COLX;                    /* where values start           */

static void put(int x, int y, const char *s, vga_color_t fg){
    if(y<0||y>=H) return;
    for(;*s && x<W-1; s++, x++) vga_putchar_at(*s,x,y,fg,VGA_BLACK);
}
static void row_kv(const char *k, const char *v, vga_color_t vc){
    if(Y>=H-1) return;
    put(2,Y,k,VGA_DARK_GREY);
    put(COLX,Y,v,vc);
    Y++;
}
static void heading(const char *h){
    if(Y>=H-1) return;
    if(Y>3) Y++;
    put(2,Y,h,ui_theme()->accent);
    Y++;
}
/* a*mul/total without a 64-bit divide (the kernel does not link libgcc).
   mul is at most 1000 and a <= total, so shrinking both until total fits
   in 22 bits keeps a*mul inside 32. */
static uint32_t scale(uint32_t a, uint32_t mul, uint32_t total){
    while(total > 0x003FFFFFu){ a >>= 1; total >>= 1; }
    if(a > total) a = total;
    return total ? (a*mul)/total : 0;
}

/* A bar `w` wide, `used` of `total` filled, then the label after it. */
static void bar(int x, int y, int w, uint32_t used, uint32_t total, const char *label){
    if(y>=H-1) return;
    if(w<8) w=8;
    int fill = (int)scale(used,(uint32_t)w,total);
    if(fill>w) fill=w;
    uint32_t pct = scale(used,100u,total);
    vga_color_t c = pct>=90 ? VGA_LIGHT_RED : pct>=70 ? VGA_YELLOW : VGA_LIGHT_GREEN;
    vga_putchar_at('[',x,y,VGA_DARK_GREY,VGA_BLACK);
    for(int i=0;i<w;i++) vga_putchar_at(i<fill?'#':'.',x+1+i,y,i<fill?c:VGA_DARK_GREY,VGA_BLACK);
    vga_putchar_at(']',x+1+w,y,VGA_DARK_GREY,VGA_BLACK);
    put(x+w+3,y,label,VGA_WHITE);
}

/* ── the values ───────────────────────────────────────────────────── */
static void cpu_name(char *out, int max){
    uint32_t a,b,c,d; int n=0; out[0]=0;
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0x80000000u));
    if(a>=0x80000004u){
        for(uint32_t leaf=0x80000002u; leaf<=0x80000004u; leaf++){
            __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(leaf));
            uint32_t r[4]={a,b,c,d};
            for(int i=0;i<4;i++) for(int k=0;k<4;k++){
                char ch=(char)((r[i]>>(8*k))&0xFF);
                if(ch && n<max-1) out[n++]=ch;
            }
        }
        out[n]=0;
        int s=0; while(out[s]==' ') s++;
        if(s){ int k=0; while(out[s]) out[k++]=out[s++]; out[k]=0; }
        if(out[0]) return;
    }
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0u));
    uint32_t v[3]={b,d,c}; n=0;
    for(int i=0;i<3;i++) for(int k=0;k<4;k++){
        char ch=(char)((v[i]>>(8*k))&0xFF); if(ch&&n<max-1) out[n++]=ch;
    }
    out[n]=0;
}

/* The system volume's size and free space in KB. free_kb is 0xFFFFFFFF
   when FAT32's FSInfo does not say (counting would walk the whole FAT).
   Returns the filesystem's name, or 0 with no disk mounted. */
const char *sys_volume_space(uint32_t *total_kb, uint32_t *free_kb){
    *total_kb=0; *free_kb=0xFFFFFFFFu;
    if(boot_is_live() || !vfs_using_disk()) return 0;
    uint32_t tc=0, fc=0, cb=0; const char *nm;
    if(vfs_using_fat32()){
        if(!fat32_sys_begin()) return "FAT32";
        tc=fat32_total_clusters(); fc=fat32_free_clusters(); cb=fat32_cluster_bytes();
        fat32_sys_end();
        nm="FAT32";
    } else if(vfs_using_fat16()){
        tc=fat16_total_clusters(); fc=fat16_free_clusters(); cb=fat16_cluster_bytes();
        nm="FAT16";
    } else return "FAT12";
    if(!cb) return nm;
    uint32_t kpc = cb/1024;                       /* KB per cluster */
    *total_kb = kpc ? tc*kpc : tc/2;
    if(fc!=0xFFFFFFFFu) *free_kb = kpc ? fc*kpc : fc/2;
    return nm;
}

static void uptime_str(sb_t *s){
    uint32_t t=boot_ms()/1000u;
    if(t>=86400u){ sb_u(s,t/86400u); sb_s(s,"d "); t%=86400u; }
    if(t>=3600u){ sb_u(s,t/3600u); sb_s(s,"h "); t%=3600u; }
    sb_u(s,t/60u); sb_s(s,"m "); sb_2(s,t%60u); sb_s(s,"s");
}

/* ── the tabs ─────────────────────────────────────────────────────── */
enum { T_OVERVIEW, T_MEMORY, T_STORAGE, T_NETWORK, T_DEVICES, T_N };
static const char *tab_names[T_N] = { "Overview","Memory","Storage","Network","Devices" };
static int dev_scroll;

static void tab_overview(void){
    sb_t s; char cpu[64];
    COLX=14;
    put(2,Y,"HavenDOS " HAVENDOS_VER_NUM " " HAVENDOS_CHANNEL " \"" HAVENDOS_CODENAME "\"",VGA_WHITE);
    if(W>=70) put(W-1-(int)strlen(HAVENDOS_VENDOR),Y,HAVENDOS_VENDOR,VGA_DARK_GREY);
    Y+=2;

    sb_init(&s); uptime_str(&s); row_kv("Uptime",s.b,VGA_WHITE);
    {
        uint8_t h,m,sec,d,mo; uint16_t yr;
        rtc_get_time(&h,&m,&sec); rtc_get_date(&d,&mo,&yr);
        sb_init(&s); sb_u(&s,yr); sb_s(&s,"-"); sb_2(&s,mo); sb_s(&s,"-"); sb_2(&s,d);
        sb_s(&s,"  "); sb_2(&s,h); sb_s(&s,":"); sb_2(&s,m); sb_s(&s,":"); sb_2(&s,sec);
        row_kv("Clock",s.b,VGA_WHITE);
    }
    cpu_name(cpu,sizeof cpu); row_kv("CPU",cpu,VGA_WHITE);
    {
        uint32_t t=pmm_total_blocks(), f=pmm_free_blocks();
        sb_init(&s); sb_kb(&s,(t-f)*4u); sb_s(&s," of "); sb_kb(&s,t*4u); sb_s(&s," used");
        put(2,Y,"Memory",VGA_DARK_GREY);
        bar(COLX,Y,W>=100?30:16,t-f,t,s.b); Y++;
    }
    {
        uint32_t tk,fk; const char *fs=sys_volume_space(&tk,&fk);
        put(2,Y,"Storage",VGA_DARK_GREY);
        if(!fs){ put(COLX,Y,boot_is_live()?"live session -- running from RAM, no disk written":"no system volume",VGA_LIGHT_GREY); }
        else if(fk==0xFFFFFFFFu || !tk){
            sb_init(&s); sb_s(&s,fs); sb_s(&s,", "); sb_kb(&s,tk); sb_s(&s,"  (free space not recorded)");
            put(COLX,Y,s.b,VGA_WHITE);
        } else {
            sb_init(&s); sb_kb(&s,tk-fk); sb_s(&s," of "); sb_kb(&s,tk); sb_s(&s," used, "); sb_s(&s,fs);
            bar(COLX,Y,W>=100?30:16,tk-fk,tk,s.b);
        }
        Y++;
    }
    {
        sb_init(&s);
        if(vga_fb_active()){ sb_u(&s,vga_fb_width()); sb_s(&s,"x"); sb_u(&s,vga_fb_height()); sb_s(&s," framebuffer, "); }
        else sb_s(&s,"VGA text, ");
        sb_s(&s,"console "); sb_u(&s,(uint32_t)vga_phys_cols()); sb_s(&s,"x"); sb_u(&s,(uint32_t)vga_phys_rows());
        row_kv("Display",s.b,VGA_WHITE);
    }
    {
        sb_init(&s);
        if(netdev_count()<=0) sb_s(&s,"no network adapter");
        else{
            sb_s(&s,netdev_bus_name(0));
            if(!netdev_link_up(0)) sb_s(&s,", link down");
            sb_s(&s,", "); sb_ip(&s,net_get_ip());
            int src=net_config_source();
            sb_s(&s, src==NET_CFG_DHCP?" from DHCP":src==NET_CFG_MANUAL?" set by hand":" (fallback -- does not route)");
        }
        row_kv("Network",s.b,VGA_WHITE);
    }
    sb_init(&s); sb_s(&s,current_username()); sb_s(&s,current_user_admin()?" (administrator)":" (standard user)");
    sb_s(&s," on "); sb_s(&s,cfg_hostname());
    row_kv("User",s.b,VGA_WHITE);
    row_kv("Theme",ui_theme_name(ui_current_theme()),VGA_WHITE);
    sb_init(&s); sb_u(&s,(uint32_t)bpkg_installed_count()); sb_s(&s," installed");
    row_kv("Packages",s.b,VGA_WHITE);
    sb_init(&s);
    if(boot_is_live()) sb_s(&s,"live session from the ISO -- nothing here is kept");
    else{
        int b=blk_boot_dev();
        sb_s(&s,"installed");
        if(b>=0){ sb_s(&s,", "); sb_s(&s,blk_bus_name(b)); sb_s(&s," disk "); sb_u(&s,(uint32_t)b); }
    }
    row_kv("Running",s.b,VGA_WHITE);
    /* v0.9.6: the line that used to say "one app at a time". */
    sb_init(&s);
    if(!task_ready()) sb_s(&s,"off -- one app at a time (safe mode or `notasks`)");
    else {
        int n=task_count(), run=0;
        for(int t=1;t<TASK_MAX;t++) if(task_running_behind(t)) run++;
        sb_u(&s,(uint32_t)n); sb_s(&s," open");
        if(run){ sb_s(&s,", "); sb_u(&s,(uint32_t)run); sb_s(&s," running behind"); }
        sb_s(&s," -- Alt+Tab switches, `ps` lists");
    }
    row_kv("Tasks",s.b,VGA_WHITE);
}

static void tab_memory(void){
    sb_t s; COLX=20;
    uint32_t t=pmm_total_blocks(), f=pmm_free_blocks();
    heading("Physical memory");
    sb_init(&s); sb_kb(&s,(t-f)*4u); sb_s(&s," used of "); sb_kb(&s,t*4u);
    bar(4,Y,W>=100?50:30,t-f,t,s.b); Y++;
    sb_init(&s); sb_kb(&s,f*4u); row_kv("Free",s.b,VGA_WHITE);
    sb_init(&s); sb_u(&s,t); sb_s(&s," pages of 4 KB, "); sb_u(&s,t-f); sb_s(&s," in use");
    row_kv("Pages",s.b,VGA_WHITE);

    uint32_t hf=0, hu=0; heap_stats(&hf,&hu);
    heading("Kernel heap");
    sb_init(&s); sb_kb(&s,hu/1024u); sb_s(&s," used of "); sb_kb(&s,heap_pool_bytes()/1024u);
    bar(4,Y,W>=100?50:30,hu,heap_pool_bytes(),s.b); Y++;
    sb_init(&s); sb_kb(&s,hf/1024u); row_kv("Free",s.b,VGA_WHITE);

    heading("What uses it");
    row_kv("Kernel","code, data and the boot stack, below 4 MB",VGA_LIGHT_GREY);
    row_kv("Console","a shadow buffer of up to 240x135 cells",VGA_LIGHT_GREY);
    row_kv("Buffers","disk, USB and network buffers, taken as needed",VGA_LIGHT_GREY);
}

static void tab_storage(void){
    sb_t s; COLX=20;
    uint32_t tk,fk; const char *fs=sys_volume_space(&tk,&fk);
    heading("System volume");
    if(!fs){
        row_kv("Volume",boot_is_live()?"none: a live session runs from RAM":"none mounted",VGA_LIGHT_GREY);
    } else {
        row_kv("Filesystem",fs,VGA_WHITE);
        sb_init(&s); sb_kb(&s,tk); row_kv("Size",s.b,VGA_WHITE);
        if(fk!=0xFFFFFFFFu && tk){
            sb_init(&s); sb_kb(&s,tk-fk); sb_s(&s," used, "); sb_kb(&s,fk); sb_s(&s," free");
            put(2,Y,"Used",VGA_DARK_GREY); bar(COLX,Y,W>=100?40:24,tk-fk,tk,s.b); Y++;
        } else row_kv("Free","not recorded by the volume (FSInfo)",VGA_LIGHT_GREY);
    }

    heading("Disks");
    int n=blk_count(), boot=blk_boot_dev();
    if(n<=0){ row_kv("","no disks found",VGA_LIGHT_GREY); return; }
    for(int i=0;i<n && Y<H-1;i++){
        sb_init(&s); sb_s(&s,"disk "); sb_u(&s,(uint32_t)i);
        put(2,Y,s.b,VGA_DARK_GREY);
        sb_init(&s); sb_s(&s,blk_bus_name(i)); sb_s(&s,"  ");
        sb_kb(&s,(uint32_t)(blk_sectors(i)>>1) );
        if(blk_removable(i)) sb_s(&s,"  removable");
        if(blk_readonly(i))  sb_s(&s,"  read-only");
        if(i==boot)          sb_s(&s,"  <- HavenDOS runs from this one");
        put(COLX,Y,s.b,i==boot?VGA_LIGHT_GREEN:VGA_WHITE);
        Y++;
    }
}

static void tab_network(void){
    sb_t s; COLX=20;
    heading("Adapters");
    int n=netdev_count();
    if(n<=0) row_kv("","no network adapter found -- `lspci` and `usb eth` say what is there",VGA_LIGHT_GREY);
    for(int i=0;i<n && Y<H-1;i++){
        uint8_t mac[6]; netdev_mac(i,mac);
        sb_init(&s); sb_s(&s,netdev_bus_name(i)); sb_s(&s,"  ");
        for(int k=0;k<6;k++){ sb_hex(&s,mac[k],2); if(k<5) sb_s(&s,":"); }
        sb_s(&s, netdev_link_up(i)?"  link up":"  link DOWN");
        row_kv(i==0?"In use":"Also",s.b,netdev_link_up(i)?VGA_WHITE:VGA_YELLOW);
    }
    if(n>0){
        heading("Address");
        int src=net_config_source();
        sb_init(&s); sb_ip(&s,net_get_ip()); sb_s(&s," / "); sb_ip(&s,net_get_mask());
        row_kv("IP",s.b,src==NET_CFG_FALLBACK?VGA_YELLOW:VGA_WHITE);
        sb_init(&s); sb_ip(&s,net_get_gw());  row_kv("Gateway",s.b,VGA_WHITE);
        sb_init(&s); sb_ip(&s,net_get_dns()); row_kv("DNS",s.b,VGA_WHITE);
        row_kv("From", src==NET_CFG_DHCP?"DHCP":src==NET_CFG_MANUAL?"set by hand":
                       "the built-in fallback -- nothing routes; `ifconfig dhcp`",
               src==NET_CFG_FALLBACK?VGA_YELLOW:VGA_WHITE);
    }
    heading("Traffic since boot");
    sb_init(&s); sb_u(&s,g_netstat.eth_rx); sb_s(&s," in, "); sb_u(&s,g_netstat.eth_tx); sb_s(&s," out");
    if(g_netstat.eth_tx_fail){ sb_s(&s,", "); sb_u(&s,g_netstat.eth_tx_fail); sb_s(&s," failed"); }
    row_kv("Frames",s.b,VGA_WHITE);
    sb_init(&s); sb_u(&s,g_netstat.tcp_seg_rx); sb_s(&s," in, "); sb_u(&s,g_netstat.tcp_seg_tx); sb_s(&s," out, ");
    sb_u(&s,g_netstat.tcp_rtx); sb_s(&s," resent");
    row_kv("TCP segments",s.b,VGA_WHITE);
    sb_init(&s); sb_u(&s,g_netstat.udp_rx); sb_s(&s," in, "); sb_u(&s,g_netstat.udp_tx); sb_s(&s," out");
    row_kv("UDP",s.b,VGA_WHITE);
    sb_init(&s); sb_u(&s,g_netstat.tls_rec_rx); sb_s(&s," records in, "); sb_u(&s,g_netstat.tls_alerts); sb_s(&s," alerts");
    row_kv("TLS",s.b,VGA_WHITE);
}

static void tab_devices(void){
    sb_t s;
    int lines=0, first=dev_scroll;
    #define DEVLINE(txt,c) do{ if(lines++>=first && Y<H-1){ put(2,Y,(txt),(c)); Y++; } }while(0)
    sb_init(&s); sb_s(&s,"PCI -- "); sb_u(&s,(uint32_t)pci_count()); sb_s(&s," devices");
    DEVLINE(s.b,ui_theme()->accent);
    for(int i=0;i<pci_count();i++){
        const pci_dev_t *d=pci_get(i);
        sb_init(&s); sb_s(&s,"  ");
        sb_hex(&s,d->bus,2); sb_s(&s,":"); sb_hex(&s,d->dev,2); sb_s(&s,"."); sb_u(&s,d->fn);
        sb_s(&s,"  "); sb_hex(&s,d->vendor,4); sb_s(&s,":"); sb_hex(&s,d->device,4);
        sb_s(&s,"  "); sb_s(&s,pci_describe(d));
        DEVLINE(s.b,VGA_WHITE);
    }
    DEVLINE("",VGA_WHITE);
    /* bus-label-ok: the heading of the USB device list -- the bus itself,
       not a disk; disks on the Storage tab come from blk_bus_name(). */
    sb_init(&s); sb_s(&s,"USB -- "); sb_u(&s,(uint32_t)usb_device_count()); sb_s(&s," devices");
    DEVLINE(s.b,ui_theme()->accent);
    for(int i=0;i<usb_device_count();i++){
        const usb_dev_t *d=usb_device(i);
        if(!d) continue;
        sb_init(&s); sb_s(&s,"  "); sb_hex(&s,d->vendor,4); sb_s(&s,":"); sb_hex(&s,d->product,4);
        sb_s(&s, d->speed==USB_SPEED_SUPER?"  super":d->speed==USB_SPEED_HIGH?"  high":
                 d->speed==USB_SPEED_LOW?"  low":"  full");
        sb_s(&s,"-speed, class "); sb_hex(&s,d->dev_class,2);
        DEVLINE(s.b,VGA_WHITE);
    }
    if(usb_device_count()==0) DEVLINE("  nothing plugged in, or no controller came up (`usb -vv`)",VGA_LIGHT_GREY);
    DEVLINE("",VGA_WHITE);
    DEVLINE("Pointing",ui_theme()->accent);
    sb_init(&s); sb_s(&s,"  "); sb_u(&s,(uint32_t)i2c_hid_count()); sb_s(&s," I2C-HID device(s) -- `i2chid` for detail");
    DEVLINE(s.b,VGA_WHITE);
    if(dev_scroll>0 && lines<=first) dev_scroll=0;
    #undef DEVLINE
}

/* ── the frame ────────────────────────────────────────────────────── */
static void draw(int tab){
    const theme_t *th=ui_theme();
    W=vga_cols(); H=vga_rows();
    vga_begin_frame();
    for(int y=0;y<H;y++) vga_fill_row(y,' ',VGA_LIGHT_GREY,VGA_BLACK);
    vga_fill_row(0,' ',th->bar_fg,th->bar_bg);
    put(0,0," System",VGA_WHITE);
    for(int x=0;x<W;x++) vga_putchar_at(x<7?" System"[x]:' ',x,0,th->bar_fg,th->bar_bg);
    {
        uint8_t h,m,s; rtc_get_time(&h,&m,&s);
        char tb[6]={ (char)('0'+h/10),(char)('0'+h%10),':',(char)('0'+m/10),(char)('0'+m%10),0 };
        for(int i=0;i<5;i++) vga_putchar_at(tb[i],W-6+i,0,th->accent,th->bar_bg);
    }
    /* tabs */
    int x=2;
    for(int t=0;t<T_N;t++){
        int on=(t==tab), n=(int)strlen(tab_names[t]);
        if(x+n+4>=W) break;
        vga_putchar_at(on?'[':' ',x,1,th->accent,VGA_BLACK);
        vga_putchar_at((char)('1'+t),x+1,1,VGA_DARK_GREY,VGA_BLACK);
        for(int k=0;k<n;k++) vga_putchar_at(tab_names[t][k],x+3+k,1,on?VGA_WHITE:VGA_LIGHT_GREY,on?th->title_bg:VGA_BLACK);
        vga_putchar_at(on?']':' ',x+3+n,1,th->accent,VGA_BLACK);
        x+=n+6;
    }
    for(int xx=0;xx<W;xx++) vga_putchar_at('-',xx,2,VGA_DARK_GREY,VGA_BLACK);
    Y=3;
    switch(tab){
    case T_OVERVIEW: tab_overview(); break;
    case T_MEMORY:   tab_memory();   break;
    case T_STORAGE:  tab_storage();  break;
    case T_NETWORK:  tab_network();  break;
    case T_DEVICES:  tab_devices();  break;
    }
    vga_fill_row(H-1,' ',VGA_LIGHT_GREY,VGA_DARK_GREY);
    const char *keys = W>=72 ? " Left/Right or 1-5 tabs   R refresh   ESC close   (updates every second)"
                             : " </> tabs  R refresh  ESC close";
    for(int i=0;keys[i]&&i<W-1;i++) vga_putchar_at(keys[i],i,H-1,VGA_WHITE,VGA_DARK_GREY);
    if(tab==T_DEVICES && W>=90) {
        const char *k2="Up/Down scroll ";
        int kx=W-1-(int)strlen(k2);
        for(int i=0;k2[i];i++) vga_putchar_at(k2[i],kx+i,H-1,VGA_WHITE,VGA_DARK_GREY);
    }
    vga_end_frame();
}

static int tab_from_word(const char *w){
    if(!w||!w[0]) return T_OVERVIEW;
    static const struct { const char *w; int t; } m[] = {
        {"overview",T_OVERVIEW},{"memory",T_MEMORY},{"mem",T_MEMORY},
        {"storage",T_STORAGE},{"disk",T_STORAGE},{"disks",T_STORAGE},
        {"network",T_NETWORK},{"net",T_NETWORK},{"devices",T_DEVICES},
        {"hardware",T_DEVICES},{0,0}
    };
    for(int i=0;m[i].w;i++) if(!strcmp(m[i].w,w)) return m[i].t;
    return -1;
}

void app_system_open(const char *which){
    int tab=tab_from_word(which);
    if(tab<0){
        vga_printf("system: no tab '%s' -- overview, memory, storage, network or devices\n",which);
        return;
    }
    dev_scroll=0;
    uint32_t last=0;
    for(;;){
        draw(tab);
        last=get_ticks();
        int k=-1;
        /* Wait for a key, redrawing once a second. */
        while((k=keyboard_getchar())==-1){
            if(get_ticks()-last >= 100) break;
            sleep_ms(20);
        }
        if(k==-1) continue;
        if(k==27||k=='q'||k=='Q') break;
        else if(k==KEY_RIGHT||k=='\t') { tab=(tab+1)%T_N; dev_scroll=0; }
        else if(k==KEY_LEFT)           { tab=(tab+T_N-1)%T_N; dev_scroll=0; }
        else if(k>='1'&&k<='5')        { tab=k-'1'; dev_scroll=0; }
        else if(k==KEY_DOWN && tab==T_DEVICES) dev_scroll++;
        else if(k==KEY_UP   && tab==T_DEVICES && dev_scroll>0) dev_scroll--;
        else if(k==KEY_PGDN && tab==T_DEVICES) dev_scroll+=H-6;
        else if(k==KEY_PGUP && tab==T_DEVICES){ dev_scroll-=H-6; if(dev_scroll<0) dev_scroll=0; }
    }
    vga_clear();
}

void app_system(void){ app_system_open(""); }
