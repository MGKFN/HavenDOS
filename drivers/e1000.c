/*
 * e1000.c — Intel 82540EM (e1000) NIC driver for BOOT OS v0.5.6
 * MMIO-based, poll mode, no interrupts.
 * Works with QEMU/UTM SE "Intel Gigabit Ethernet (e1000)".
 */
#include "../include/types.h"
#include "../include/io.h"
#include "../include/e1000.h"
#include "../include/string.h"

/* ── Register offsets (MMIO) ──────────────────────────────── */
#define E1000_CTRL    0x0000
#define E1000_STATUS  0x0008
#define E1000_EECD    0x0010
#define E1000_EERD    0x0014
#define E1000_ICR     0x00C0
#define E1000_IMS     0x00D0
#define E1000_IMC     0x00D8
#define E1000_RCTL    0x0100
#define E1000_TCTL    0x0400
#define E1000_TIPG    0x0410
#define E1000_RDBAL   0x2800
#define E1000_RDBAH   0x2804
#define E1000_RDLEN   0x2808
#define E1000_RDH     0x2810
#define E1000_RDT     0x2818
#define E1000_TDBAL   0x3800
#define E1000_TDBAH   0x3804
#define E1000_TDLEN   0x3808
#define E1000_TDH     0x3810
#define E1000_TDT     0x3818
#define E1000_MTA     0x5200  /* 128 x 4-byte multicast table */
#define E1000_RAL     0x5400  /* Receive Address Low  */
#define E1000_RAH     0x5404  /* Receive Address High */

/* CTRL bits */
#define CTRL_RST      (1<<26)
#define CTRL_SLU      (1<<6)   /* Set Link Up */
#define CTRL_ASDE     (1<<5)

/* RCTL bits */
#define RCTL_EN       (1<<1)
#define RCTL_SBP      (1<<2)
#define RCTL_UPE      (1<<3)   /* unicast promisc */
#define RCTL_MPE      (1<<4)   /* multicast promisc */
#define RCTL_BAM      (1<<15)  /* broadcast accept */
#define RCTL_BSIZE_2K (0<<16)
#define RCTL_SECRC    (1<<26)  /* strip CRC */

/* TCTL bits */
#define TCTL_EN       (1<<1)
#define TCTL_PSP      (1<<3)
#define TCTL_CT_SHIFT 4
#define TCTL_COLD_SHIFT 12

/* TX descriptor command bits */
#define TDESC_CMD_EOP  (1<<0)
#define TDESC_CMD_FCS  (1<<1)
#define TDESC_CMD_RS   (1<<3)
#define TDESC_STA_DD   (1<<0)

/* RX descriptor status */
#define RDESC_STA_DD   (1<<0)
#define RDESC_STA_EOP  (1<<1)

#define RX_DESC_COUNT  32
#define TX_DESC_COUNT  8
#define PKT_SIZE       2048

/* ── Descriptor structs ───────────────────────────────────── */
typedef struct {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed)) rx_desc_t;

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed)) tx_desc_t;

/* ── Static buffers ───────────────────────────────────────── */
static rx_desc_t rx_descs[RX_DESC_COUNT] __attribute__((aligned(16)));
static tx_desc_t tx_descs[TX_DESC_COUNT] __attribute__((aligned(16)));
static uint8_t   rx_bufs [RX_DESC_COUNT][PKT_SIZE] __attribute__((aligned(16)));
static uint8_t   tx_bufs [TX_DESC_COUNT][PKT_SIZE] __attribute__((aligned(16)));

static uint32_t  mmio    = 0;
static int       nic_ok  = 0;
static uint8_t   nic_mac[ETH_ALEN];
static uint32_t  rx_tail = 0;
static uint32_t  tx_tail = 0;

/* ── MMIO helpers ─────────────────────────────────────────── */
static uint32_t e_read (uint32_t reg){ return *(volatile uint32_t*)(mmio+reg); }
static void     e_write(uint32_t reg, uint32_t v){ *(volatile uint32_t*)(mmio+reg)=v; }

/* ── EEPROM read ──────────────────────────────────────────── */
static uint16_t eeprom_read(uint8_t addr)
{
    e_write(E1000_EERD, ((uint32_t)addr<<8) | 1);
    uint32_t v;
    int t=10000;
    do { v=e_read(E1000_EERD); } while(!(v&(1<<4)) && --t);
    return (uint16_t)(v>>16);
}

/* ── Init ─────────────────────────────────────────────────── */
int e1000_init(uint32_t base)
{
    mmio   = base;
    nic_ok = 0;

    /* Reset */
    e_write(E1000_CTRL, e_read(E1000_CTRL) | CTRL_RST);
    volatile int d=100000; while(d--);   /* wait reset */

    /* Set link up */
    e_write(E1000_CTRL, e_read(E1000_CTRL) | CTRL_SLU | CTRL_ASDE);

    /* Mask all interrupts */
    e_write(E1000_IMC, 0xFFFFFFFF);
    e_write(E1000_ICR, 0xFFFFFFFF);

    /* Read MAC from EEPROM */
    uint16_t w0=eeprom_read(0), w1=eeprom_read(1), w2=eeprom_read(2);
    nic_mac[0]=(uint8_t)(w0);     nic_mac[1]=(uint8_t)(w0>>8);
    nic_mac[2]=(uint8_t)(w1);     nic_mac[3]=(uint8_t)(w1>>8);
    nic_mac[4]=(uint8_t)(w2);     nic_mac[5]=(uint8_t)(w2>>8);

    /* Program MAC into RAL/RAH */
    e_write(E1000_RAL, (uint32_t)nic_mac[0]|(uint32_t)nic_mac[1]<<8|
                       (uint32_t)nic_mac[2]<<16|(uint32_t)nic_mac[3]<<24);
    e_write(E1000_RAH, (uint32_t)nic_mac[4]|(uint32_t)nic_mac[5]<<8|(1u<<31));

    /* Clear multicast table */
    for(int i=0;i<128;i++) e_write(E1000_MTA+i*4, 0);

    /* ── RX setup ── */
    for(int i=0;i<RX_DESC_COUNT;i++){
        rx_descs[i].addr   = (uint32_t)rx_bufs[i];
        rx_descs[i].status = 0;
    }
    e_write(E1000_RDBAL, (uint32_t)rx_descs);
    e_write(E1000_RDBAH, 0);
    e_write(E1000_RDLEN, RX_DESC_COUNT * sizeof(rx_desc_t));
    e_write(E1000_RDH,   0);
    /* RDT = last descriptor index; NIC owns [RDH..RDT] */
    e_write(E1000_RDT,   RX_DESC_COUNT - 1);
    rx_tail = 0;   /* driver reads from 0 first */
    e_write(E1000_RCTL,  RCTL_EN|RCTL_BAM|RCTL_BSIZE_2K|RCTL_SECRC|RCTL_MPE);

    /* ── TX setup ── */
    for(int i=0;i<TX_DESC_COUNT;i++){
        tx_descs[i].addr   = (uint32_t)tx_bufs[i];
        tx_descs[i].status = TDESC_STA_DD;  /* mark free */
    }
    e_write(E1000_TDBAL, (uint32_t)tx_descs);
    e_write(E1000_TDBAH, 0);
    e_write(E1000_TDLEN, TX_DESC_COUNT * sizeof(tx_desc_t));
    e_write(E1000_TDH,   0);
    e_write(E1000_TDT,   0);
    tx_tail = 0;
    e_write(E1000_TCTL,  TCTL_EN|TCTL_PSP|(15<<TCTL_CT_SHIFT)|(63<<TCTL_COLD_SHIFT));
    e_write(E1000_TIPG,  0x0060200A);

    nic_ok = 1;
    return 1;
}

int  e1000_found(void)                   { return nic_ok; }
void e1000_get_mac(uint8_t m[ETH_ALEN])  { for(int i=0;i<ETH_ALEN;i++) m[i]=nic_mac[i]; }

/* ── Send ─────────────────────────────────────────────────── */
int e1000_send(const void *data, uint16_t len)
{
    if(!nic_ok || len>PKT_SIZE) return -1;

    /* Wait for descriptor to be free */
    int t=100000;
    while(!(tx_descs[tx_tail].status & TDESC_STA_DD) && --t);

    memcpy(tx_bufs[tx_tail], data, len);
    tx_descs[tx_tail].length = len;
    tx_descs[tx_tail].cmd    = TDESC_CMD_EOP|TDESC_CMD_FCS|TDESC_CMD_RS;
    tx_descs[tx_tail].status = 0;

    uint32_t old = tx_tail;
    tx_tail = (tx_tail+1) % TX_DESC_COUNT;
    e_write(E1000_TDT, tx_tail);
    (void)old;
    return 0;
}

/* ── Receive ──────────────────────────────────────────────── */
uint16_t e1000_recv(void *buf, uint16_t buf_len)
{
    if(!nic_ok) return 0;
    if(!(rx_descs[rx_tail].status & RDESC_STA_DD)) return 0;

    uint16_t len = rx_descs[rx_tail].length;
    if(len > buf_len) len = buf_len;
    memcpy(buf, rx_bufs[rx_tail], len);

    /* Clear status, advance our pointer, then give the consumed
       slot back to the NIC (RDT = last slot the driver owns) */
    rx_descs[rx_tail].status = 0;
    uint32_t consumed = rx_tail;
    rx_tail = (rx_tail + 1) % RX_DESC_COUNT;
    e_write(E1000_RDT, consumed);
    return len;
}
