#!/usr/bin/env python3
"""check_fat32sys_suite.py -- prove tests/test_fat32sys.c can fail.

v0.9.3 "Gaia". An install above 2047 MB is FAT32, so fs/fat32.c's single
mounted volume became the system's own while four other parts of the
kernel go on borrowing it. The rules that keep the machine's filesystem
in the slot -- park on borrow, return on unmount, never drop the system,
one turn mid-borrow -- are a handful of lines each, and every one of
them is the kind that passes a happy-path test while losing somebody's
settings the first time `update` runs from the ESP.

So each rule is broken here once, plus fs/sysvol.c's return convention
(fs/update.c tested `<= 0` against fat16's byte counts once and called
every successful write a failure) and fat32_rename()'s refusals.

Run:  python3 tools/check_fat32sys_suite.py    (foreground, never &)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC = "fs/fat32.c"
SYSVOL = "fs/sysvol.c"
TEST = "tests/test_fat32sys.c"
CC = ["gcc", "-std=gnu99", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function"]

BUGS = [
    ("a borrow drops the system volume instead of parking it",
     "the 0.9.2 behaviour: after `fat32 ls` or an update from the ESP the "
     "system volume is gone until something remounts it",
     """    if(g_cur_is_sys && g_mounted){
        fat32_flush();
        state_save(&g_sys);
        g_sys_parked = 1;""",
     """    if(g_cur_is_sys && g_mounted){
        fat32_flush();
        state_save(&g_sys);
        g_sys_parked = 0;"""),

    ("unmount drops the system volume too",
     "every borrower's tidy-up takes the machine's own filesystem away",
     """    if(g_cur_is_sys && g_mounted) return;   /* see the block above        */""",
     """    /* (no guard) */"""),

    ("unmount does not bring the system back",
     "the borrow ends and the slot is empty",
     """    slot_drop();
    sys_return();
}""",
     """    slot_drop();
}"""),

    ("a failed borrow does not restore the system",
     "mounting a stick that is not FAT32 unmounts the system volume",
     """    if(!rd || !rd(ctx, part_lba, bs) || fat_probe_type(bs) != FATTYPE_32){
        sys_return();
        return 0;
    }""",
     """    if(!rd || !rd(ctx, part_lba, bs) || fat_probe_type(bs) != FATTYPE_32){
        return 0;
    }"""),

    # Not here: "the turn does not flush the borrowed volume first".
    # Every public fs/fat32.c operation ends in fat32_flush(), so between
    # two calls the FAT window and FSInfo are never dirty and that flush
    # cannot change a byte. It is kept as belt-and-braces; an injection
    # that cannot be observed would only teach the harness to lie.

    ("the system's turn is not handed back",
     "after one sysvol call the update source has vanished from the slot",
     """    state_save(&g_sys);
    g_sys_parked = 1;
    state_load(&g_borrow);""",
     """    state_save(&g_sys);
    g_sys_parked = 1;
    state_load(&g_sys);"""),

    ("the FAT window survives a swap",
     "a cached FAT sector of one volume is written into the other",
     """    g_free_known = s->free_known; g_fsinfo_dirty = s->fsinfo_dirty;
    /* The FAT window belonged to whichever volume was in the slot before;
       it was flushed when that one was put aside, and it is not this one's. */
    g_fatw_valid = 0; g_fatw_dirty = 0;""",
     """    g_free_known = s->free_known; g_fsinfo_dirty = s->fsinfo_dirty;"""),

    ("the parked state forgets where the data area starts",
     "a field of the mount left out of the struct -- the bug this design "
     "invites. The two images are different sizes so that their data "
     "areas start in different places; with the same geometry this "
     "defect is invisible, which is how the first version of this "
     "harness missed it",
     """    s->fat_start = g_fat_start; s->data_start = g_data_start;""",
     """    s->fat_start = g_fat_start;"""),

    ("sysvol_write returns 1, as fat32_write does",
     "fs/update.c's backup check reads a 1-byte write of a 3 MB kernel",
     """        return r ? (int)len : -1;""",
     """        return r ? 1 : -1;""",
     SYSVOL),

    ("sysvol takes no turn: it writes into whatever is mounted",
     "an update from the ESP writes the new kernel onto the ESP",
     """    case SYSVOL_FAT32:
        if(!fat32_sys_begin()) return -1;
        r = fat32_write(path, buf, len) == 1 && fat32_file_size(path) == len;""",
     """    case SYSVOL_FAT32:
        r = fat32_write(path, buf, len) == 1 && fat32_file_size(path) == len;""",
     SYSVOL),

    ("sysvol_read refuses a file larger than the buffer",
     "the first version of sysvol.c did this, and fs/update.c's 4-byte "
     "verify of the kernel it had just written failed every FAT32 update "
     "-- found by harness/esp_update.py, not by this suite",
     """        r = fat32_read(path, buf, max);""",
     """        if(fat32_file_size(path) > max){ fat32_sys_end(); return -1; }
        r = fat32_read(path, buf, max);""",
     SYSVOL),

    ("rename overwrites an existing target",
     "renaming onto cfg.txt loses cfg.txt's chain and cross-links the FAT",
     """    if(path_walk(to, &e2, &dst_dir, leaf, sizeof leaf) != 0) return 0;""",
     """    if(path_walk(to, &e2, &dst_dir, leaf, sizeof leaf) < 0) return 0;"""),

    ("rename leaves the old entry behind",
     "two names on one chain -- fsck.fat calls it a cross-link",
     """    if(!write_entry(at, leaf, n11, ntres, nlfn, e.attr, e.clus, e.size))
        return 0;
    if(!entry_erase(&e)) return 0;""",
     """    if(!write_entry(at, leaf, n11, ntres, nlfn, e.attr, e.clus, e.size))
        return 0;"""),

    ("rename moves a directory across parents",
     "its .. still names the old parent; fsck.fat reports it",
     """    if((e.attr & ATTR_DIRECTORY) && dst_dir != src_dir) return 0;""",
     """    /* (directories go anywhere) */"""),
]

# ── fat32_init()'s discovery ────────────────────────────────────────
BUGS += [
    ("the GPT is not searched (0.9.2's fat32_init)",
     "a FAT32 install made by the additive installer is never mounted, "
     "and the machine boots into the installer",
     """    if(find_gpt_fat32()) return 1;""",
     """    /* MBR only */"""),

    ("any FAT32 on a GPT disk will do",
     "the ESP is first on the disk and becomes the system volume",
     """        if(!ours || parts[i].first_lba == 0) continue;""",
     """        if(parts[i].first_lba == 0) continue;"""),

    ("the ESP is not skipped by name",
     "with the type check also gone, the ESP; with it, a belt nobody tests",
     """        if(parts[i].is_esp) continue;
        for(k = 0; k < 16; k++)
            if(parts[i].type_guid[k] != OURS[k]){ ours = 0; break; }
        if(!ours || parts[i].first_lba == 0) continue;""",
     """        for(k = 0; k < 16; k++)
            if(parts[i].type_guid[k] != OURS[k]){ ours = 0; break; }
        if(parts[i].first_lba == 0) continue;"""),

    ("the label pass is skipped on MBR",
     "somebody's FAT32 data partition ahead of ours becomes the system",
     """            if(pass == 0){
                if(!local_read(0, lba, bs) || !bpb_label_is_ours(bs)) continue;
            }""",
     """            (void)bs;"""),

    ("the legacy MBR pass is dropped",
     "a FAT32 disk that mounted before 0.9.3 stops mounting",
     """    for(int pass = 0; pass < 2; pass++){""",
     """    for(int pass = 0; pass < 1; pass++){"""),
]

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the driver changed",
    """int fat32_borrowed(void){ return g_mounted && !g_cur_is_sys; }""",
    """/* the slot holds somebody else's volume */
int fat32_borrowed(void){ return g_mounted && !g_cur_is_sys; }""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "fat32sys", SRC, TEST, CC, BUGS, HARMLESS, timeout=120,
        files=(SRC, SYSVOL, TEST),
        preamble="Proving tests/test_fat32sys.c can fail."))
