#ifndef PCI_H
#define PCI_H
#include "types.h"
void        pci_init(void);
int         pci_ide_found(void);
uint16_t    pci_ide_pri_base(void);
uint16_t    pci_ide_pri_ctrl(void);
uint16_t    pci_ide_sec_base(void);
uint16_t    pci_ide_sec_ctrl(void);
const char *pci_get_log(void);
void        pci_scan_net(uint16_t *io_base_out);
void        pci_scan_net2(uint16_t *rtl_io, uint32_t *e1000_mmio);
#endif
