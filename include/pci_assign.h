#ifndef PCI_ASSIGN_H
#define PCI_ASSIGN_H
/* drivers/pci_assign.c -- a memory window for a BAR the firmware left at
   zero (v0.9.6.6, for the HP's trackpad controllers). Its header says
   where the window goes and why nobody else's BAR is touched. */
#include "types.h"
#include "pci.h"

typedef struct {
    uint64_t base, end;     /* [base, end) */
    int      bar;           /* 1: a device's BAR (its bound may be trimmed); 0: a bridge window, exact */
} pci_range_t;

#define PCI_ASSIGN_MAX_RANGES 160

enum { PA_OK = 1, PA_NOT_MEM = -1, PA_32BIT = -2, PA_SIZE = -3, PA_TOO_BIG = -4,
       PA_NO_ROOM = -5, PA_STUCK = -6, PA_ASSIGNED = -7 };

uint64_t    pci_bar_bound(uint64_t base);
void        pci_trim_ranges(pci_range_t *r, int n);
uint64_t    pci_plan_window(const pci_range_t *used, int n, uint64_t size);
int         pci_used_ranges(pci_range_t *out, int max, const pci_dev_t *skip);
int         pci_assign_mem_bar(const pci_dev_t *d, int n, uint64_t *addr_out);
const char *pci_assign_why(int r);

/* drivers/pci.c: the table's copy of a BAR, after it was written */
void        pci_set_bar(const pci_dev_t *d, int n, uint32_t v);
#endif
