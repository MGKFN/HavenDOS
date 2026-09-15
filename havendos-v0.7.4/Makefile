CC      = gcc
LD      = ld
CFLAGS  = -m32 -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
          -fno-asynchronous-unwind-tables \
          -Wall -O2 -std=gnu99 -Iinclude -DHAVE_GRUB_EMBED -DHAVE_KERNEL_ELF
LDFLAGS = -m elf_i386 -T boot/linker.ld --oformat elf32-i386 \
          --strip-debug

SRCS_C  = kernel/main.c kernel/config.c shell/login.c shell/bootanim.c shell/screensaver.c shell/tetris.c \
          drivers/speaker.c drivers/mode13.c drivers/vesa.c \
          kernel/string.c kernel/idt.c kernel/pit.c kernel/pmm.c kernel/heap.c kernel/errors.c \
          drivers/vga.c drivers/keyboard.c drivers/mouse.c \
          drivers/ata.c drivers/atapi.c drivers/pci.c drivers/virtio.c \
          drivers/rtl8139.c drivers/e1000.c drivers/rtc.c \
          fs/ramfs.c fs/fat12.c fs/fat16.c fs/vfs.c fs/install.c \
          fs/iso9660.c fs/update.c \
          net/net.c net/ethernet.c net/arp.c net/ipv4.c net/icmp.c net/udp.c net/tcp.c net/dns.c net/dhcp.c net/http.c net/tls_crypto.c net/tls.c \
          shell/ui.c shell/shell.c shell/desktop.c shell/bpkg.c shell/packages.c shell/boot_lang.c shell/iso_installer.c shell/setup.c shell/man.c shell/toast.c shell/shell_v072.c shell/shell_v073.c

SRCS_S  = boot/boot.S
OBJS_C  = $(SRCS_C:.c=.o)
OBJS_S  = $(SRCS_S:.S=.o)
OBJS    = $(OBJS_S) $(OBJS_C)

all: havendos.iso

# ── grub_embed.h: pre-generated, rebuild manually if GRUB changes ──────────
include/grub_embed.h:
	grub-mkimage -O i386-pc -o /tmp/havendos_core.img \
	  -p '(hd0,msdos1)/boot/grub' \
	  biosdisk part_msdos fat multiboot normal configfile ls echo reboot halt
	python3 tools/make_grub_embed.py \
	  /usr/lib/grub/i386-pc/boot.img /tmp/havendos_core.img include/grub_embed.h

# ── Two-pass build for kernel ELF embedding ─────────────────────────────────
#
# PROBLEM: the installer must write a valid ELF file to /boot/havendos.elf.
# At runtime, 0x100000 holds the LOADED image (starts with multiboot magic,
# not ELF magic). GRUB cannot load that.
#
# SOLUTION: embed the ELF file itself as kernel_elf_data[] in kernel_elf.h.
# Build is two passes:
#   Pass 1: build without HAVE_KERNEL_ELF (install.c returns -8 = placeholder)
#           → produces havendos_pass1.elf (~370KB)
#   Pass 2: generate kernel_elf.h from pass-1 ELF, rebuild with HAVE_KERNEL_ELF
#           → produces final havendos.elf (~740KB, contains embedded ELF)
#           → installer copies kernel_elf_data[] (pass-1 ELF) to disk
#           → GRUB loads it correctly since it's a valid ELF file
#
# The installed ELF is pass-1 but contains identical code — the only difference
# is pass-2 has the extra .rodata from kernel_elf.h which is not needed at runtime.

CFLAGS_PASS1 = -m32 -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
               -fno-asynchronous-unwind-tables \
               -Wall -O2 -std=gnu99 -Iinclude -DHAVE_GRUB_EMBED

# Pass-1 objects (compiled without HAVE_KERNEL_ELF)
OBJS_PASS1 = $(SRCS_C:.c=.pass1.o) boot/boot.pass1.o

%.pass1.o: %.c include/grub_embed.h
	$(CC) $(CFLAGS_PASS1) -c $< -o $@

boot/boot.pass1.o: boot/boot.S
	$(CC) $(CFLAGS_PASS1) -c $< -o $@

havendos_pass1.elf: include/grub_embed.h $(OBJS_PASS1)
	$(LD) $(LDFLAGS) -o $@ $(OBJS_PASS1)

include/kernel_elf.h: havendos_pass1.elf
	python3 tools/make_kernel_embed.py havendos_pass1.elf include/kernel_elf.h

%.o: %.c include/grub_embed.h include/kernel_elf.h
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

havendos.elf: include/grub_embed.h include/kernel_elf.h $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

havendos.iso: havendos.elf
	cp havendos.elf isodir/boot/havendos.elf
	grub-mkrescue -o havendos.iso isodir
	@echo ""
	@echo "==================================="
	@echo "  HavenDOS v0.7.4 ISO built successfully!"
	@echo "  Size: $$(du -h havendos.iso | cut -f1)"
	@echo "==================================="

clean:
	rm -f $(OBJS) $(OBJS_PASS1) havendos.elf havendos_pass1.elf havendos.iso \
	      isodir/boot/havendos.elf include/kernel_elf.h

.PHONY: all clean
