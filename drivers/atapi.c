/*
 * atapi.c — Polling ATAPI CD-ROM driver for HavenDOS
 *
 * Probes primary and secondary IDE buses for ATAPI devices.
 * Pure polling — no IRQs. Uses ATAPI PACKET + READ(12) via PIO.
 *
 * UTM SE / QEMU notes:
 *  - CD drive often appears on secondary bus master (0x170)
 *  - Signature check (0x14/0xEB) may not be set until after SRST
 *  - We do a soft reset on each channel before probing
 *  - Fall through to IDENTIFY PACKET if signature absent
 */

#include "../include/types.h"
#include "../include/io.h"
#include "../include/string.h"

/* ── IDE port bases ─────────────────────────────────────────── */
#define ATA_PRI_BASE   0x1F0
#define ATA_PRI_CTRL   0x3F6
#define ATA_SEC_BASE   0x170
#define ATA_SEC_CTRL   0x376

/* ATA register offsets */
#define ATA_DATA       0
#define ATA_ERR        1   /* write: features */
#define ATA_NSECT      2
#define ATA_LBA0       3
#define ATA_LBA1       4   /* byte count low */
#define ATA_LBA2       5   /* byte count high */
#define ATA_DRVSEL     6
#define ATA_STATUS     7   /* write: command */

/* Status bits */
#define ATA_SR_BSY     0x80
#define ATA_SR_DRDY    0x40
#define ATA_SR_DRQ     0x08
#define ATA_SR_ERR     0x01

/* Commands */
#define ATA_CMD_SRST        0x08   /* device control: soft reset */
#define ATAPI_CMD_PACKET    0xA0
#define ATAPI_CMD_IDENTIFY  0xA1

/* SCSI opcodes */
#define SCSI_READ12     0xA8

#define ATAPI_SECTOR_SIZE 2048

/* ── Internal state ─────────────────────────────────────────── */
static int      g_atapi_found = 0;
static uint16_t g_base        = 0;
static uint16_t g_ctrl        = 0;
static uint8_t  g_slave       = 0;

/* ── Timing helpers ─────────────────────────────────────────── */
static void atapi_400ns(void) {
    inb(g_ctrl); inb(g_ctrl); inb(g_ctrl); inb(g_ctrl);
}

static void atapi_spin(uint32_t n) {
    for (volatile uint32_t i = 0; i < n; i++)
        __asm__ volatile("pause");
}

static int atapi_wait_not_busy(uint16_t base, uint16_t ctrl) {
    for (int i = 0; i < 2000000; i++) {
        uint8_t st = inb(base + ATA_STATUS);
        if (st == 0xFF) return 0;          /* floating bus */
        if (!(st & ATA_SR_BSY)) return 1;
        inb(ctrl); inb(ctrl);
    }
    return 0;
}

static int atapi_wait_drq(uint16_t base, uint16_t ctrl) {
    for (int i = 0; i < 2000000; i++) {
        uint8_t st = inb(base + ATA_STATUS);
        if (st & ATA_SR_ERR) return 0;
        if (st & ATA_SR_DRQ) return 1;
        inb(ctrl); inb(ctrl);
    }
    return 0;
}

/* ── Soft reset a channel ───────────────────────────────────── */
static void channel_srst(uint16_t base, uint16_t ctrl) {
    outb(ctrl, 0x04);  /* SRST bit */
    atapi_spin(50000);
    outb(ctrl, 0x00);  /* clear SRST */
    atapi_spin(200000);
    atapi_wait_not_busy(base, ctrl);
}

/* ── Probe one drive position ───────────────────────────────── */
static int probe_atapi(uint16_t base, uint16_t ctrl, uint8_t slave) {
    /* Floating bus check */
    uint8_t st0 = inb(base + ATA_STATUS);
    if (st0 == 0xFF) return 0;

    /* Select drive */
    uint8_t sel = slave ? 0xB0 : 0xA0;
    outb(base + ATA_DRVSEL, sel);
    atapi_spin(100000);

    /* Check status again after select */
    uint8_t st1 = inb(base + ATA_STATUS);
    if (st1 == 0xFF) return 0;

    /* Read ATAPI signature */
    uint8_t lo = inb(base + ATA_LBA1);
    uint8_t hi = inb(base + ATA_LBA2);

    /* Classic ATAPI signature */
    if (lo == 0x14 && hi == 0xEB) return 1;

    /* After SRST, QEMU sometimes sets 0x00/0x00 then signature appears */
    if (lo == 0x00 && hi == 0x00) {
        /* Try IDENTIFY PACKET — if the drive responds with DRQ it's ATAPI */
        outb(base + ATA_STATUS, ATAPI_CMD_IDENTIFY);
        atapi_spin(100000);
        if (!atapi_wait_not_busy(base, ctrl)) return 0;
        lo = inb(base + ATA_LBA1);
        hi = inb(base + ATA_LBA2);
        if (lo == 0x14 && hi == 0xEB) return 1;
        /* DRQ means it responded to IDENTIFY PACKET — accept it */
        uint8_t st2 = inb(base + ATA_STATUS);
        if (st2 & ATA_SR_DRQ) {
            /* drain the identify data */
            for (int i = 0; i < 256; i++) inw(base + ATA_DATA);
            return 1;
        }
        return 0;
    }

    /* Some QEMU configs report 0x01/0x01 or other after reset */
    /* Accept anything that isn't clearly an ATA disk (ATA=0x00/0x00 after non-reset) */
    /* Try sending IDENTIFY PACKET and see if drive accepts */
    if (st1 & ATA_SR_DRDY) {
        /* Drive ready but wrong signature — could be ATA, skip */
        return 0;
    }

    return 0;
}

/* ── Public API ─────────────────────────────────────────────── */

int atapi_init(void) {
    g_atapi_found = 0;

    /* Reset both channels before probing */
    channel_srst(ATA_PRI_BASE, ATA_PRI_CTRL);
    channel_srst(ATA_SEC_BASE, ATA_SEC_CTRL);

    /* Try all four positions — QEMU usually puts CD on secondary master */
    /* Try secondary master first (most common QEMU/UTM position) */
    if (probe_atapi(ATA_SEC_BASE, ATA_SEC_CTRL, 0)) {
        g_base = ATA_SEC_BASE; g_ctrl = ATA_SEC_CTRL; g_slave = 0;
        g_atapi_found = 1; return 1;
    }
    /* Secondary slave */
    if (probe_atapi(ATA_SEC_BASE, ATA_SEC_CTRL, 1)) {
        g_base = ATA_SEC_BASE; g_ctrl = ATA_SEC_CTRL; g_slave = 1;
        g_atapi_found = 1; return 1;
    }
    /* Primary slave (VirtIO disk is primary master, so skip that) */
    if (probe_atapi(ATA_PRI_BASE, ATA_PRI_CTRL, 1)) {
        g_base = ATA_PRI_BASE; g_ctrl = ATA_PRI_CTRL; g_slave = 1;
        g_atapi_found = 1; return 1;
    }
    /* Primary master last (unlikely — VirtIO is there) */
    if (probe_atapi(ATA_PRI_BASE, ATA_PRI_CTRL, 0)) {
        g_base = ATA_PRI_BASE; g_ctrl = ATA_PRI_CTRL; g_slave = 0;
        g_atapi_found = 1; return 1;
    }
    return 0;
}

int atapi_detected(void) { return g_atapi_found; }

int atapi_read_sector(uint32_t lba, uint8_t *buf) {
    if (!g_atapi_found) return 0;

    /* Select drive */
    outb(g_base + ATA_DRVSEL, g_slave ? 0xB0 : 0xA0);
    atapi_spin(50000);

    if (!atapi_wait_not_busy(g_base, g_ctrl)) return 0;

    /* Set up for PIO transfer, byte count = 2048 */
    outb(g_base + ATA_ERR,  0x00);
    outb(g_base + ATA_LBA1, 0x00);       /* byte count low  = 0x00 */
    outb(g_base + ATA_LBA2, 0x08);       /* byte count high = 0x08 → 0x0800 = 2048 */
    outb(g_base + ATA_STATUS, ATAPI_CMD_PACKET);

    /* Wait for DRQ — drive wants 12-byte command packet */
    if (!atapi_wait_drq(g_base, g_ctrl)) return 0;

    /* READ(12) packet */
    uint8_t pkt[12];
    memset(pkt, 0, 12);
    pkt[0] = SCSI_READ12;
    pkt[2] = (lba >> 24) & 0xFF;
    pkt[3] = (lba >> 16) & 0xFF;
    pkt[4] = (lba >>  8) & 0xFF;
    pkt[5] = (lba      ) & 0xFF;
    pkt[9] = 1;   /* transfer 1 sector */

    /* Send 6 words */
    for (int i = 0; i < 6; i++)
        outw(g_base + ATA_DATA, (uint16_t)(pkt[i*2] | (pkt[i*2+1] << 8)));

    atapi_400ns();

    /* Wait for data DRQ */
    if (!atapi_wait_drq(g_base, g_ctrl)) return 0;

    /* Read 1024 words = 2048 bytes */
    uint16_t *dst = (uint16_t *)buf;
    for (int i = 0; i < ATAPI_SECTOR_SIZE / 2; i++)
        dst[i] = inw(g_base + ATA_DATA);

    atapi_wait_not_busy(g_base, g_ctrl);
    return 1;
}

int atapi_read_sectors(uint32_t lba, uint32_t count, uint8_t *buf) {
    for (uint32_t i = 0; i < count; i++) {
        if (!atapi_read_sector(lba + i, buf + i * ATAPI_SECTOR_SIZE))
            return (int)i;
    }
    return (int)count;
}
