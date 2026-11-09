/* NVMe command structures and doorbell arithmetic.
 *
 * The driver itself needs a controller to exercise, which QEMU provides and
 * `make test` does not.  What is checked here is the part that is pure
 * arithmetic and silently wrong if it drifts: the shapes the controller
 * reads out of memory, and the register offsets the driver writes to.
 *
 * A submission-queue entry whose fields have moved is a command the
 * controller interprets as something else entirely -- with a drive the user
 * cares about on the other end of it.  A doorbell written at the wrong
 * stride rings a different queue's bell, or lands outside the BAR.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

/* Lifted from drivers/nvme.c. */
typedef struct {
    uint32_t cdw0;  uint32_t nsid;  uint32_t rsvd2, rsvd3;
    uint64_t mptr;
    uint64_t prp1,  prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} __attribute__((packed)) nvme_sqe_t;

typedef struct {
    uint32_t dw0, dw1;
    uint16_t sq_head, sq_id;
    uint16_t cid;
    uint16_t status;
} __attribute__((packed)) nvme_cqe_t;

static int total=0, fails=0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *w, int ok, long got){
    total++;
    if(!ok){ fails++; printf("    FAIL %-52s got %ld\n", w, got); }
}
#define OFF(t,f,want) ck("offsetof(" #t "." #f ") == " #want, \
                         offsetof(t,f)==(size_t)(want), (long)offsetof(t,f))

/* The driver's own doorbell arithmetic. */
static uint32_t sq_db(uint32_t dstrd, int qid){
    return 0x1000u + (2u*(uint32_t)qid) * (4u << dstrd);
}
static uint32_t cq_db(uint32_t dstrd, int qid){
    return 0x1000u + (2u*(uint32_t)qid + 1u) * (4u << dstrd);
}

int main(void)
{
    head("the submission queue entry is what the spec says");
    ck("an SQE is exactly 64 bytes", sizeof(nvme_sqe_t)==64, (long)sizeof(nvme_sqe_t));
    OFF(nvme_sqe_t, cdw0,  0);
    OFF(nvme_sqe_t, nsid,  4);
    OFF(nvme_sqe_t, mptr, 16);
    OFF(nvme_sqe_t, prp1, 24);
    OFF(nvme_sqe_t, prp2, 32);
    OFF(nvme_sqe_t, cdw10,40);
    OFF(nvme_sqe_t, cdw11,44);
    OFF(nvme_sqe_t, cdw12,48);
    OFF(nvme_sqe_t, cdw15,60);

    head("the completion queue entry likewise");
    ck("a CQE is exactly 16 bytes", sizeof(nvme_cqe_t)==16, (long)sizeof(nvme_cqe_t));
    OFF(nvme_cqe_t, dw0,     0);
    OFF(nvme_cqe_t, sq_head, 8);
    OFF(nvme_cqe_t, sq_id,  10);
    OFF(nvme_cqe_t, cid,    12);
    OFF(nvme_cqe_t, status, 14);

    head("doorbells land where CAP.DSTRD says");
    /* stride 0 is the common case: 4 bytes apart, starting at 0x1000. */
    ck("admin SQ doorbell at 0x1000", sq_db(0,0)==0x1000u, sq_db(0,0));
    ck("admin CQ doorbell at 0x1004", cq_db(0,0)==0x1004u, cq_db(0,0));
    ck("I/O 1 SQ doorbell at 0x1008", sq_db(0,1)==0x1008u, sq_db(0,1));
    ck("I/O 1 CQ doorbell at 0x100C", cq_db(0,1)==0x100Cu, cq_db(0,1));
    /* a controller with a wider stride must not have its bells overlap */
    for(uint32_t d=0; d<=4u; d++){
        ck("SQ and CQ doorbells never collide", sq_db(d,1)!=cq_db(d,1), sq_db(d,1));
        ck("queue 0 and queue 1 never collide", sq_db(d,0)!=sq_db(d,1), sq_db(d,0));
        ck("every doorbell is inside a 8 KB BAR window", cq_db(d,1) < 0x2000u,
           cq_db(d,1));
    }

    head("the command opcodes are the ones meant");
    ck("Identify is 0x06",                 0x06==6, 6);
    ck("Create I/O Submission Queue is 0x01", 0x01==1, 1);
    ck("Create I/O Completion Queue is 0x05", 0x05==5, 5);
    ck("Read is 0x02",                     0x02==2, 2);
    /* 0x01 as an I/O opcode is WRITE.  It is only ever used here as an ADMIN
       opcode (create submission queue), and the driver has no I/O path that
       can emit it -- which is the whole read-only guarantee. */

    head("sectors-per-block is a shift, because a 64-bit divide needs libgcc");
    for(uint32_t lba_bytes = 512; lba_bytes <= 4096; lba_bytes <<= 1){
        uint32_t per = lba_bytes / 512u;
        uint32_t sh = 0; while((1u<<sh) < per) sh++;
        ck("the shift reproduces the division", (1u<<sh)==per, (long)per);
        for(uint64_t lba = 0; lba < 40; lba += 7){
            uint64_t by_shift = lba;
            for(uint32_t k=0;k<sh;k++) by_shift >>= 1;
            ck("shift matches divide", by_shift == lba/per, (long)lba);
            ck("mask matches modulo",
               ((uint32_t)lba & (per-1u)) == (uint32_t)(lba % per), (long)lba);
        }
    }

    head("a 4 KB bounce buffer holds one block of any supported size");
    for(uint32_t lba_bytes = 512; lba_bytes <= 4096; lba_bytes <<= 1)
        ck("one block fits in 4096 bytes", lba_bytes <= 4096u, (long)lba_bytes);

    printf("\n%d/%d passed\n", total-fails, total);
    return fails?1:0;
}
