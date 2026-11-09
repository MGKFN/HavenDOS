#pragma once
#include "types.h"
int      virtio_blk_init(void);
int      virtio_blk_read(uint64_t sector, uint8_t *buf);
int      virtio_blk_write(uint64_t sector, const uint8_t *buf);
int      virtio_blk_ready(void);
uint64_t virtio_blk_sectors(void);
int      virtio_scan_drives(void);
int      virtio_init_drive(int idx);
int      virtio_drive_count(void);
uint64_t virtio_drive_sectors(int idx);
uint16_t virtio_drive_io(int idx);
uint8_t  virtio_drive_bus(int idx);
uint8_t  virtio_drive_dev(int idx);
int      virtio_drive_read(int idx, uint64_t sector, uint8_t *buf);
int      virtio_drive_write(int idx, uint64_t sector, const uint8_t *buf);
