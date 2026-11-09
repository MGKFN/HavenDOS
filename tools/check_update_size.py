#!/usr/bin/env python3
"""check_update_size.py -- refuse a kernel too big for `update` to carry.

WHY THIS CHECK EXISTS

fs/update.c stages the whole new kernel in one buffer before it writes a
byte of it, and that buffer has a fixed size. In v0.8.4 the cap was found
to be 2 MB while the kernel was 2.80 MB -- so `update` could not carry the
kernel at all, from either source, and had not been able to for some
releases.

Nothing noticed, and the reason is worth recording: the refusal it
produced was "Kernel too large for update buffer", which reads like a
corrupt or wrong ISO rather than like a kernel that quietly got bigger.
The limit was crossed by ordinary growth -- AHCI, NVMe, two embedded GRUB
EFI images -- with no single commit to blame.

A size that only fails at the moment somebody tries to upgrade a real
machine is exactly the class of bug this tree makes the BUILD refuse. So
the build refuses it.

    python3 tools/check_update_size.py havendos.elf fs/update.c
"""
import os
import re
import sys


def read_cap(src):
    """MAX_ELF_SIZE, evaluated rather than pattern-matched, so a change
    from (8 * 1024 * 1024) to any other arithmetic still reads."""
    text = open(src, encoding="utf-8").read()
    m = re.search(r'^#define\s+MAX_ELF_SIZE\s+(.+?)\s*$', text, re.M)
    if not m:
        sys.exit("check_update_size: no MAX_ELF_SIZE in %s" % src)
    expr = m.group(1).split("/*")[0].strip()
    if not re.fullmatch(r'[0-9()\*\+\s]+', expr):
        sys.exit("check_update_size: MAX_ELF_SIZE is not plain arithmetic: %s"
                 % expr)
    return int(eval(expr))          # noqa: S307 -- guarded by the regex above


def main():
    kernel = sys.argv[1] if len(sys.argv) > 1 else "havendos.elf"
    src = sys.argv[2] if len(sys.argv) > 2 else "fs/update.c"
    if not os.path.exists(kernel):
        sys.exit("check_update_size: no %s" % kernel)

    cap = read_cap(src)
    size = os.path.getsize(kernel)

    if size > cap:
        sys.stderr.write(
            "\ncheck_update_size: THE KERNEL IS TOO BIG FOR `update` TO CARRY.\n"
            "  %s is %.2f MB\n"
            "  MAX_ELF_SIZE in %s is %.2f MB\n\n"
            "  `update` stages the whole image before it writes any of it, so\n"
            "  this build could be installed but never upgraded onto. Raise\n"
            "  MAX_ELF_SIZE; the buffer is taken from the physical allocator\n"
            "  only while update or rollback runs, so it costs nothing at boot.\n"
            % (kernel, size / 1048576.0, src, cap / 1048576.0))
        return 1

    head = cap - size
    print("check_update_size: ok, kernel %.2f MB, update buffer %.2f MB, "
          "%.2f MB spare" % (size / 1048576.0, cap / 1048576.0,
                             head / 1048576.0))
    if head < 1048576:
        print("check_update_size: WARNING -- under 1 MB of headroom left")
    return 0


if __name__ == "__main__":
    sys.exit(main())
