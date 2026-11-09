/*
 * rtl8139.c — RTL8139 NIC driver for BOOT OS v0.5.6
 *
 * Poll-based (no IRQ), works with QEMU/UTM SE user-mode networking.
 * TX: 4 descriptors, round-robin.
 * RX: 8KB ring buffer, WRAP mode.
 */

#include "../include/types.h"
#include "../include/io.h"
#include "../include/rtl8139.h"
#include "../include/string.h"

/* ── Register offsets ──────────────────────────────────────── */
#define RTL_IDR0        0x00   /* MAC bytes 0-5 */
#define RTL_MAR0        0x08   /* Multicast filter */
#define RTL_TSD0        0x10   /* TX status desc 0-3 (4 × 4 bytes) */
#define RTL_TSAD0       0x20   /* TX start addr desc 0-3 (4 × 4 bytes) */
#define RTL_RBSTART     0x30   /* RX buffer start */
#define RTL_ERBCR       0x34   /* Early RX byte count */
#define RTL_ERSR        0x36   /* Early RX status */
#define RTL_CR          0x37   /* Command register */
#define RTL_CAPR        0x38   /* Current address of pkt read */
#define RTL_CBR         0x3A   /* Current buffer address */
#define RTL_IMR         0x3C   /* Interrupt mask */
#define RTL_ISR         0x3E   /* Interrupt status */
#define RTL_TCR         0x40   /* TX config */
#define RTL_RCR         0x44   /* RX config */
#define RTL_TCTR        0x48   /* Timer count */
#define RTL_MPC         0x4C   /* Missed packet counter */
#define RTL_9346CR      0x50   /* 93C46 command */
#define RTL_CONFIG0     0x51
#define RTL_CONFIG1     0x52
#define RTL_MSR         0x58   /* Media status */
#define RTL_BMCR        0x62   /* Basic mode control */

/* CR bits */
#define CR_RST    0x10
#define CR_RE     0x08
#define CR_TE     0x04
#define CR_BUFE   0x01

/* TSD bits */
#define TSD_OWN   0x2000   /* driver owns descriptor (not DMA) */
#define TSD_TOK   0x8000   /* TX OK */

/* RX header bits */
#define RXH_ROK   0x0001
#define RXH_FAE   0x0002
#define RXH_CRC   0x0004
#define RXH_LONG  0x0008
#define RXH_RUNT  0x0010
#define RXH_ISE   0x0020

/* RCR flags */
#define RCR_AAP   (1<<0)   /* accept all physical */
#define RCR_APM   (1<<1)   /* accept physical match */
#define RCR_AM    (1<<2)   /* accept multicast */
#define RCR_AB    (1<<3)   /* accept broadcast */
#define RCR_WRAP  (1<<7)   /* ring buffer wrap */
#define RCR_MXDMA (7<<8)   /* max DMA burst = unlimited */
#define RCR_RBLEN (0<<11)  /* 8KB RX buffer */

#define RX_BUF_SIZE  (8192 + 16 + 1500)  /* 8K + header space + guard */
#define TX_BUF_SIZE  1536
#define TX_DESC_CNT  4

/* ── Static buffers (must be accessible by NIC DMA → static) ── */
static uint8_t  rx_buf[RX_BUF_SIZE]  __attribute__((aligned(4)));
static uint8_t  tx_buf[TX_DESC_CNT][TX_BUF_SIZE] __attribute__((aligned(4)));

static uint16_t io_base  = 0;
static int      nic_ok   = 0;
static uint8_t  nic_mac[ETH_ALEN];
static int      tx_cur   = 0;   /* next TX descriptor to use */
static uint16_t rx_ptr   = 0;   /* our read pointer into RX ring */

/* ── helpers ─────────────────────────────────────────────────── */
static void busy_wait_short(volatile uint32_t n){ while(n--) __asm__ volatile("nop"); }

static void rtl_write8 (uint8_t  reg, uint8_t  v){ outb(io_base+reg, v); }
static void rtl_write16(uint8_t  reg, uint16_t v){ outw(io_base+reg, v); }
static void rtl_write32(uint8_t  reg, uint32_t v){ outl(io_base+reg, v); }
static uint8_t  rtl_read8 (uint8_t reg){ return inb(io_base+reg); }
static uint16_t rtl_read16(uint8_t reg){ return inw(io_base+reg); }
static uint32_t rtl_read32(uint8_t reg){ return inl(io_base+reg); }

/* ── Init ────────────────────────────────────────────────────── */
int rtl8139_init(uint16_t base)
{
    io_base = base;
    nic_ok  = 0;

    /* Power on */
    rtl_write8(RTL_CONFIG1, 0x00);
    io_wait();

    /* Software reset */
    rtl_write8(RTL_CR, CR_RST);
    int timeout = 1000;
    while((rtl_read8(RTL_CR) & CR_RST) && --timeout)
        busy_wait_short(1000);
    if(!timeout) return 0;   /* reset hung */

    /* Read MAC */
    for(int i = 0; i < ETH_ALEN; i++)
        nic_mac[i] = rtl_read8(RTL_IDR0 + i);

    /* Set RX buffer */
    rtl_write32(RTL_RBSTART, (uint32_t)rx_buf);

    /* Interrupts — mask all (we poll ISR manually) */
    rtl_write16(RTL_IMR, 0x0000);
    rtl_write16(RTL_ISR, 0xFFFF);  /* clear pending */

    /* TX config: IFG=3, max DMA=2048 */
    rtl_write32(RTL_TCR, 0x03000700);

    /* RX config: accept physical+broadcast+multicast, 8K buf, wrap */
    rtl_write32(RTL_RCR, RCR_APM|RCR_AB|RCR_AM|RCR_WRAP|RCR_MXDMA|RCR_RBLEN);

    /* TX descriptor addresses */
    for(int i = 0; i < TX_DESC_CNT; i++)
        rtl_write32(RTL_TSAD0 + i*4, (uint32_t)tx_buf[i]);

    /* Enable TX+RX */
    rtl_write8(RTL_CR, CR_RE | CR_TE);

    /* Unlock 93C46 so we can write config */
    rtl_write8(RTL_9346CR, 0xC0);
    rtl_write8(RTL_CONFIG1, 0x00);
    rtl_write8(RTL_9346CR, 0x00);

    rx_ptr = 0;
    tx_cur = 0;
    nic_ok = 1;
    return 1;
}

int      rtl8139_found(void)              { return nic_ok; }
uint16_t rtl8139_io_base(void)            { return io_base; }
void     rtl8139_get_mac(uint8_t m[ETH_ALEN]){
    for(int i=0;i<ETH_ALEN;i++) m[i]=nic_mac[i];
}

/* ── Send ────────────────────────────────────────────────────── */
int rtl8139_send(const void *data, uint16_t len)
{
    if(!nic_ok || len > TX_BUF_SIZE) return -1;

    /* Wait for descriptor to be free (OWN bit clear means NIC done) */
    int timeout = 10000;
    while(!(rtl_read32(RTL_TSD0 + tx_cur*4) & TSD_OWN) && --timeout)
        busy_wait_short(10);

    memcpy(tx_buf[tx_cur], data, len);
    if(len < 60) {
        /* pad to minimum Ethernet frame */
        memset(tx_buf[tx_cur]+len, 0, 60-len);
        len = 60;
    }

    /* Writing length clears OWN bit and kicks DMA */
    rtl_write32(RTL_TSD0 + tx_cur*4, len & 0x1FFF);
    tx_cur = (tx_cur + 1) % TX_DESC_CNT;
    return 0;
}

/* ── Receive ─────────────────────────────────────────────────── */
uint16_t rtl8139_recv(void *buf, uint16_t buf_len)
{
    if(!nic_ok) return 0;

    /* Check if RX buffer has data: BUFE bit in CR means empty */
    if(rtl_read8(RTL_CR) & CR_BUFE) return 0;

    /* Packet header is 4 bytes: status(2) + length(2) */
    uint16_t offset = rx_ptr % (8192);
    uint8_t *p = rx_buf + offset;

    uint16_t status = *(uint16_t*)(p);
    uint16_t pkt_len = *(uint16_t*)(p + 2);

    /* Sanity check */
    if(!(status & RXH_ROK) || pkt_len < 8 || pkt_len > 1514+4) {
        /* Bad packet — advance past it */
        rx_ptr = (uint16_t)((rtl_read16(RTL_CBR) + 16) & ~0x3);
        rtl_write16(RTL_CAPR, rx_ptr - 16);
        return 0;
    }

    uint16_t data_len = pkt_len - 4; /* strip CRC */
    if(data_len > buf_len) data_len = buf_len;

    /* Copy payload (after 4-byte header) */
    uint8_t *src = p + 4;
    uint16_t to_end = (uint16_t)(8192 - (offset + 4));
    if(data_len <= to_end) {
        memcpy(buf, src, data_len);
    } else {
        /* wraps around */
        memcpy(buf, src, to_end);
        memcpy((uint8_t*)buf + to_end, rx_buf, data_len - to_end);
    }

    /* Advance read pointer (align to 4 bytes) */
    rx_ptr = (uint16_t)((rx_ptr + pkt_len + 4 + 3) & ~3);
    rtl_write16(RTL_CAPR, (uint16_t)(rx_ptr - 16));

    return data_len;
}
