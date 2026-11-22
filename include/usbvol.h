#pragma once
#include "types.h"
/* fs/usbvol.c -- the first USB stick as the folder /USB (v0.9.7.3).
   Paths passed to the usbv_* calls are the part after /USB ("" is its
   root). Conventions follow the VFS: bytes or -1, 0 or -1, 1 or 0. */
int  usbv_present(void);
int  usbv_writable(void);              /* FAT32: yes; exFAT: read only */
int  usbv_path(const char *p, const char **rest);
int  usbv_read(const char *rest, char *buf, uint32_t max);
int  usbv_write(const char *rest, const char *data, uint32_t len);
int  usbv_exists(const char *rest);
int  usbv_is_dir(const char *rest);
void usbv_list_dir(const char *rest, void (*cb)(const char *, int, uint32_t));
int  usbv_mkdir(const char *rest);
int  usbv_delete(const char *rest);
int  usbv_rename(const char *from, const char *to);
const char *usbv_label(void);
