CC      = gcc
LD      = ld
# -MMD -MP makes gcc write a .d file per object listing every header it
# actually included, and those .d files are pulled in at the bottom of this
# Makefile.  Before v0.7.20 the only headers any rule depended on were the
# three named by hand, so editing ANY other header -- vga.h, pci.h, string.h,
# version.h -- rebuilt nothing that included it.
#
# That produced an ISO whose version.txt said v0.7.20 while the kernel inside
# it still reported 0.7.19, because version.txt is GENERATED (so it updated)
# and the kernel is COMPILED (so it did not).  `update` then did exactly what
# it was told: found a newer ISO, installed its kernel, and left the machine
# on the old version, reporting success throughout.
CFLAGS  = -m32 -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
          -fno-asynchronous-unwind-tables -MMD -MP \
          -Wall -O2 -std=gnu99 -Iinclude -DHAVE_GRUB_EMBED -DHAVE_KERNEL_ELF \
          -DHAVE_GRUB_EFI_EMBED
LDFLAGS = -m elf_i386 -T boot/linker.ld --oformat elf32-i386 \
          --strip-debug

SRCS_C  = kernel/main.c kernel/highfb.c kernel/entropy.c kernel/config.c kernel/machine.c shell/login.c shell/bootanim.c shell/screensaver.c shell/tetris.c \
          drivers/speaker.c drivers/mode13.c drivers/vesa.c \
          kernel/string.c kernel/idt.c kernel/pit.c kernel/kpoll.c kernel/appguard.c kernel/bootsplash.c kernel/pmm.c kernel/heap.c kernel/errors.c kernel/klog.c \
          drivers/vga.c drivers/keyboard.c drivers/mouse.c \
          drivers/ata.c drivers/ahci.c drivers/atapi.c drivers/pci.c drivers/pci_bar.c drivers/uhci.c drivers/nvme.c drivers/virtio.c \
          drivers/rtl8139.c drivers/e1000.c drivers/i2c.c drivers/i2c_hid.c drivers/xedisp.c drivers/xegmbus.c drivers/xemode.c drivers/edid.c drivers/rtc.c \
          fs/blkdev.c fs/ramfs.c fs/gpt.c fs/gpt_write.c fs/esp.c fs/fat16fmt.c fs/fat32fmt.c fs/espmake.c fs/uninstall.c fs/fat12.c fs/fat16.c fs/fat32.c fs/exfat.c fs/vfs.c fs/sysvol.c fs/instsize.c fs/install.c \
          fs/iso9660.c fs/update.c fs/update_src.c \
          usb/core.c usb/hotplug.c usb/uhci_hc.c usb/xhci_hc.c usb/hid.c usb/hid_rd.c usb/hub.c usb/msc.c usb/ethernet.c usb/usbmux.c usb/lockdown.c usb/pair.c \
          net/netdev.c net/net.c net/net_tick.c net/netstat.c net/ethernet.c net/arp.c net/ipv4.c net/icmp.c net/udp.c net/tcp.c net/dns.c net/dhcp.c net/http.c net/bignum.c net/der.c net/x509.c net/plist.c net/b64.c net/der_read.c net/rsa.c net/tls_crypto.c net/tls.c \
          shell/ui.c shell/shell.c shell/desktop.c shell/bpkg.c shell/packages.c shell/boot_lang.c shell/iso_installer.c shell/uninstallcmd.c shell/setup.c shell/legal.c shell/man.c shell/toast.c shell/shell_v072.c shell/shell_v073.c shell/lspci.c shell/usb.c shell/shell_v076.c shell/chat.c shell/help.c shell/apps_extra.c shell/fat32cmd.c shell/fat16cmd.c shell/exfatcmd.c shell/rsacmd.c shell/i2ccmd.c shell/i2chidcmd.c shell/gfxcmd.c shell/paircmd.c shell/crashcmd.c shell/mines.c shell/mines_model.c shell/neofetch.c

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

# grub_disk_embed.h: disk-boot GRUB with biosdisk/fat/multiboot/search baked in
include/grub_disk_embed.h:
	grub-mkimage -O i386-pc -o /tmp/havendos_core_disk.img \
	  -p '(hd0,msdos1)/boot/grub' \
	  biosdisk part_msdos fat multiboot search search_fs_file search_label \
	  normal echo ls reboot halt
	python3 tools/make_grub_embed.py \
	  /usr/lib/grub/i386-pc/boot.img /tmp/havendos_core_disk.img \
	  include/grub_disk_embed.h

# grub_efi_embed.h: the UEFI loader HavenDOS INSTALLS.
#
# The two embeds above are both -O i386-pc, which is BIOS only. A UEFI-only
# machine -- which is every laptop made this decade -- will not execute
# either of them. HavenDOS has BOOTED under UEFI since 0.8 Beta, using the
# loader grub-mkrescue puts on the ISO; what it has never been able to do is
# install a system that boots that way, because it had no PE32+ loader of
# its own to write to an ESP. This is that loader.
#
# The module providing the `chainloader` command is called `chain` on
# x86_64-efi, and `fwsetup` is `efifwsetup` -- the i386-pc names above do
# not all carry over, and grub-mkimage fails loudly on a wrong one, which
# is the good outcome. chain is in the list because the grub.cfg written to
# the ESP offers Windows as well as HavenDOS, and without it that entry
# cannot work -- a "dual boot" that only boots one of the two is not one.
# search_label is how the menu finds the HavenDOS partition without
# hardcoding a disk number that breaks when a drive is added.
include/grub_efi_embed.h:
	grub-mkimage -O x86_64-efi -o /tmp/havendos_bootx64.efi \
	  -p '/EFI/HAVENDOS' \
	  part_gpt part_msdos fat normal multiboot multiboot2 chain \
	  configfile echo ls test search search_fs_uuid search_fs_file \
	  search_label all_video gfxterm font efifwsetup reboot halt
	python3 tools/make_grub_efi_embed.py \
	  /tmp/havendos_bootx64.efi include/grub_efi_embed.h

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
               -fno-asynchronous-unwind-tables -MMD -MP \
               -Wall -O2 -std=gnu99 -Iinclude -DHAVE_GRUB_EMBED \
               -DHAVE_GRUB_EFI_EMBED

# Pass-1 objects (compiled without HAVE_KERNEL_ELF)
OBJS_PASS1 = $(SRCS_C:.c=.pass1.o) boot/boot.pass1.o

%.pass1.o: %.c include/version.h include/grub_embed.h include/grub_disk_embed.h include/grub_efi_embed.h
	$(CC) $(CFLAGS_PASS1) -c $< -o $@

boot/boot.pass1.o: boot/boot.S
	$(CC) $(CFLAGS_PASS1) -c $< -o $@

include/font8x8_ext.h: include/font8x8.h tools/make_font_ext.py
	python3 tools/make_font_ext.py include/font8x8.h include/font8x8_ext.h

havendos_pass1.elf: include/font8x8_ext.h include/grub_embed.h include/grub_efi_embed.h $(OBJS_PASS1)
	$(LD) $(LDFLAGS) -o $@ $(OBJS_PASS1)

include/kernel_elf.h: havendos_pass1.elf
	python3 tools/make_kernel_embed.py havendos_pass1.elf include/kernel_elf.h

%.o: %.c include/version.h include/grub_embed.h include/grub_disk_embed.h include/kernel_elf.h include/grub_efi_embed.h
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

havendos.elf: include/grub_embed.h include/kernel_elf.h include/grub_efi_embed.h $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# v0.7.18: version.txt and the GRUB menu titles are generated from
# include/version.h so they cannot drift from the running kernel again.
.PHONY: version-sync
# v0.8.6.2: the generated fixture .inc files are prerequisites.
#
# check_harnesses.py below BUILDS every injection harness's suite, and
# three of those suites #include a fixture .inc that this Makefile
# generates. `make clean` deletes them, so `make clean && make` died in
# version-sync with
#
#     tests/test_esp.c:177:10: fatal error: esp_fixture.inc: No such file
#
# before a single object was compiled. A release tarball ships the .inc
# files (they are small generated text) so a build from the archive
# worked, which is how this hid: it only bit a tree that had been
# cleaned. Same shape as v0.8.4's missing isodir/ -- a build step that
# depends on a generated file without saying so is fine right up until
# the file is not there.
version-sync: include/version.h \
              tests/fat32_fixture.inc tests/esp_fixture.inc \
              tests/update_fixture.inc
	@# v0.8.4: the source archive had no isodir/, so a build from it in an
	@# empty directory died at the ISO step. This creates only what is
	@# missing and never touches an isodir that is already there.
	python3 tools/make_isodir.py isodir
	python3 tools/make_version.py --check
	python3 tools/check_efi_embed.py havendos.elf
	python3 tools/check_screen_width.py
	python3 tools/check_boot_log.py
	python3 tools/check_help.py
	python3 tools/check_bus_label.py
	python3 tools/check_machine_reset.py
	python3 tools/check_gfx_readonly.py
	python3 tools/check_diskname_sync.py
	python3 tools/check_blk_dispatch.py
	python3 tools/check_nic_dispatch.py
	python3 tools/check_statics.py
	python3 tools/check_klog_fmt.py
	python3 tools/check_usb_buffers.py
	python3 tools/check_kpoll_sites.py
	python3 tools/check_kpoll_guard.py
	python3 tools/check_console_grid.py
	python3 tools/check_tls_stages.py
	python3 tools/check_rsa_kg_loop.py
	python3 tools/check_pair_idle.py
	python3 tools/check_harnesses.py
	python3 tools/check_update_size.py havendos.elf fs/update.c
	python3 tools/make_version.py include/version.h isodir

havendos.iso: havendos.elf version-sync
	cp havendos.elf isodir/boot/havendos.elf
	@# v0.8.4: the disc carries a manifest too, so an update applied from
	@# a CD gets the same checksum verification one applied from the EFI
	@# System Partition gets. Written AFTER the kernel is copied, so it
	@# describes the kernel that is actually on the ISO.
	python3 tools/make_iso_manifest.py isodir/boot/havendos.elf include/version.h isodir/update.inf
	grub-mkrescue -o havendos.iso isodir
	@# A copy named for its version, so a downloaded ISO says what it is.
	@# havendos.iso stays as well: the QEMU scripts and the update tests all
	@# refer to it by that name, and renaming it would break them silently.
	@cp -f havendos.iso "HavenDOS-$$(sed -n 's/^#define[[:space:]]*HAVENDOS_VER_NUM[[:space:]]*"\([^"]*\)".*/\1/p' include/version.h).iso"
	@echo ""
	@echo "==================================="
	@echo "  HavenDOS v$$(sed -n 's/^#define[[:space:]]*HAVENDOS_VER_NUM[[:space:]]*\"\([^\"]*\)\".*/\1/p' include/version.h) ISO built successfully!"
	@echo "  Size: $$(du -h havendos.iso | cut -f1)"
	@echo "  File: HavenDOS-$$(sed -n 's/^#define[[:space:]]*HAVENDOS_VER_NUM[[:space:]]*"\([^"]*\)".*/\1/p' include/version.h).iso"
	@echo "==================================="
	@python3 tools/check_iso_version.py havendos.iso
	@# v0.8.5.6: and that it can actually boot. A UEFI-only ISO --
	@# what grub-mkrescue emits when grub-pc-bin is absent -- passed
	@# every check above, version checks included, and booted iPXE.
	@python3 tools/check_iso_bootable.py havendos.iso

# ── Native test suites ───────────────────────────────────────────────
# v0.7.19: `make test` runs all three from the source tree alone.  The
# v0.7.18 zip shipped two suites that could not compile — test_pmm.c
# included a path into a v0.7.17 tree that was never shipped, and
# test_chrome.c included a bars.inc that did not exist — so the claimed
# 17/17 and 42/42 were not reproducible by anyone who had the zip.
#
# bars.inc is GENERATED from shell/ui.c, so the chrome test can never
# certify a stale copy of the function it is testing.
tests/bars.inc: shell/ui.c tools/make_bars_inc.py
	python3 tools/make_bars_inc.py shell/ui.c tests/bars.inc

tests/vercmp.inc: fs/update.c tools/make_bars_inc.py
	python3 tools/make_bars_inc.py fs/update.c tests/vercmp.inc ver_parse ver_cmp

# appreg.inc is GENERATED from the app registry in shell/desktop.c for the
# same reason bars.inc is generated from shell/ui.c: the menu test must not
# certify a stale copy of the table it is testing.
tests/appreg.inc: shell/desktop.c tools/make_appreg_inc.py
	python3 tools/make_appreg_inc.py shell/desktop.c tests/appreg.inc

# The FAT32 suite runs against an image this project did NOT format, so the
# formatter is a build dependency and the image is a build product.
#
# This is the rule tests/test_fat16path.c did not have.  That suite skips
# unless BOOTDISK names an installer-produced image, no such image was ever
# in the tree, and the skip notice was swallowed by a `tail -1` below -- so
# it sat in `make test` output with no result for three releases.  A fixture
# a suite needs is generated by the build, or the suite is decoration.
#
# A missing mkfs.vfat FAILS here rather than skipping, with the package name
# in the message.  That is deliberate: a build dependency that quietly turns
# a suite off is the thing being fixed.
tests/fat32.img tests/fat32_fixture.inc: tools/make_fat32_fixture.py
	python3 tools/make_fat32_fixture.py tests

# The ESP suite runs against a FAT32 volume laid out like the EFI System
# Partition on a Windows 11 laptop -- 4.6 MB of files under /EFI/Microsoft
# that the install must leave byte-for-byte alone. Same rule as above: the
# fixture is generated by the build, and its absence is an error rather
# than a quietly skipped suite.
tests/esp.img tests/esp_full.img tests/esp_fixture.inc: tools/make_esp_fixture.py
	python3 tools/make_esp_fixture.py tests

# The update suite runs against an EFI System Partition with an update
# staged on it, the way a person stages one from Windows with `mountvol`.
# Same rule as the two above: the fixture is generated by the build, and
# the CRC in its manifest is computed by python3's zlib rather than by any
# HavenDOS code -- the suite then asserts gpt_crc32() agrees with it.
tests/exfat.img tests/exfat_fixture.inc: tools/make_exfat_fixture.py
	python3 tools/make_exfat_fixture.py tests

tests/update_esp.img tests/update_fixture.inc: tools/make_update_fixture.py
	python3 tools/make_update_fixture.py tests

TESTCC = gcc -std=gnu99

.PHONY: test
# v0.8.5.6: the .img files are prerequisites too, and that is the whole
# fix for 621 silently-absent checks.
#
# The generator rules name both the image and the .inc as targets, which
# is correct -- but this line listed only the .inc files, so make was
# never ASKED about tests/fat32.img and never noticed it was missing. A
# source archive ships the .inc (it is small, generated text) and not
# the .img (it is 100 MB of FAT volume), so on a fresh tree make saw
# every prerequisite satisfied and five suites ran against fixtures that
# were not there: test_esp, test_update, test_fat32, test_fat16fmt and
# test_installadd, reporting 0/0 or "cannot copy fixture".
#
# 621 of 1995 checks -- 31%, and the install, update and filesystem
# suites at that, the ones standing between the installer and somebody's
# Windows partition. tools/run_suite.py now refuses a suite that comes
# in under its floor, so this cannot hide again even if a rule breaks.
test: tests/bars.inc tests/vercmp.inc tests/appreg.inc \
      tests/fat32_fixture.inc tests/esp_fixture.inc tests/update_fixture.inc \
      tests/exfat_fixture.inc \
      tests/fat32.img tests/fat16.img tests/fat12.img \
      tests/esp.img tests/esp_full.img tests/update_esp.img tests/exfat.img
	@echo "── test_fixes ─────────────────────────────"
	@$(TESTCC) -o /tmp/hd_tf tests/test_fixes.c && \
	  python3 tools/run_suite.py --name test_fixes --min 10 -- /tmp/hd_tf
	@echo "── test_pci ───────────────────────────────"
	@echo "  PCI enumeration (multifunction devices, USB and audio discovery)"
	@$(TESTCC) -Wno-unused-function \
	           -o /tmp/hd_tpci tests/test_pci.c drivers/pci_bar.c \
	           && \
	  python3 tools/run_suite.py --name test_pci --min 34 -- /tmp/hd_tpci
	@echo "── test_diskname ──────────────────────────"
	@echo "  a disk is named for the bus it is really on"
	@# v0.8.6: -Iinclude, because this links fs/blkdev.c now instead of
	@# carrying a copy of the function it tests. See the file's own header.
	@$(TESTCC) -Iinclude -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_tdn tests/test_diskname.c && \
	  python3 tools/run_suite.py --name test_diskname --min 35 -- /tmp/hd_tdn
	@echo "── test_klog ──────────────────────────────"
	@echo "  the kernel log: what goes in comes out, and dmesg agrees with it"
	@$(TESTCC) -o /tmp/hd_tklog tests/test_klog.c && \
	  python3 tools/run_suite.py --name test_klog --min 31 -- /tmp/hd_tklog
	@echo "── test_uhci ──────────────────────────────"
	@echo "  the USB host controller, against a simulated UHCI and device"
	@# -no-pie keeps every static below 4 GB, so the 32-bit physical
	@# addresses the driver writes into descriptors are the real addresses
	@# of its own tables and the simulated controller can follow them.
	@$(TESTCC) -no-pie -Wno-unused-function -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_tuhci tests/test_uhci.c drivers/pci_bar.c \
	           && \
	  python3 tools/run_suite.py --name test_uhci --min 110 -- /tmp/hd_tuhci
	@echo "── test_pmm ───────────────────────────────"
	@$(TESTCC) -Wno-builtin-declaration-mismatch -Wno-int-to-pointer-cast \
	           -Wno-pointer-to-int-cast \
	           -o /tmp/hd_tp tests/test_pmm.c && \
	  python3 tools/run_suite.py --name test_pmm --min 17 -- /tmp/hd_tp
	@echo "── test_chrome ────────────────────────────"
	@$(TESTCC) -o /tmp/hd_tc tests/test_chrome.c && \
	  python3 tools/run_suite.py --name test_chrome --min 42 -- /tmp/hd_tc
	@echo "── test_chat22 ────────────────────────────"
	@$(TESTCC) -Iinclude -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_tch tests/test_chat22.c && \
	  python3 tools/run_suite.py --name test_chat22 --min 60 -- /tmp/hd_tch
	@echo "── test_edgeom ────────────────────────────"
	@$(TESTCC) -o /tmp/hd_tge tests/test_edgeom.c && \
	  python3 tools/run_suite.py --name test_edgeom --min 29 -- /tmp/hd_tge
	@echo "── test_gpt ───────────────────────────────"
	@echo "  the GUID partition table -- what keeps the installer off a Windows disk"
	@$(TESTCC) -fno-builtin -Iinclude -o /tmp/hd_tg tests/test_gpt.c && \
	  python3 tools/run_suite.py --name test_gpt --min 25 -- /tmp/hd_tg
	@echo "── test_nvme ──────────────────────────────"
	@echo "  NVMe command structures and doorbell arithmetic"
	@$(TESTCC) -o /tmp/hd_tn tests/test_nvme.c && \
	  python3 tools/run_suite.py --name test_nvme --min 95 -- /tmp/hd_tn
	@echo "── test_ahci ──────────────────────────────"
	@echo "  AHCI against a controller that executes its commands"
	@$(TESTCC) -no-pie -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_tahci tests/test_ahci.c && \
	  python3 tools/run_suite.py --name test_ahci --min 58 -- /tmp/hd_tahci
	@echo "── test_nvmerw ────────────────────────────"
	@echo "  the NVMe write path, against a controller that executes it"
	@$(TESTCC) -no-pie -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_tnrw tests/test_nvmerw.c drivers/pci_bar.c \
	           && \
	  python3 tools/run_suite.py --name test_nvmerw --min 83 -- /tmp/hd_tnrw
	@echo "── test_highfb ────────────────────────────"
	@echo "  page-directory entries for a framebuffer above 4 GB"
	@$(TESTCC) -Wno-pointer-to-int-cast -Iinclude \
	           -o /tmp/hd_thf tests/test_highfb.c && \
	  python3 tools/run_suite.py --name test_highfb --min 150 -- /tmp/hd_thf
	@echo "── test_mbinfo ────────────────────────────"
	@echo "  the Multiboot structs against the spec -- fbdiag reads its answer here"
	@$(TESTCC) -o /tmp/hd_tmb tests/test_mbinfo.c && \
	  python3 tools/run_suite.py --name test_mbinfo --min 41 -- /tmp/hd_tmb
	@echo "── test_menugeom ──────────────────────────"
	@echo "  the start menu in every text mode, and the one app registry"
	@$(TESTCC) -Itests -o /tmp/hd_tmg tests/test_menugeom.c && \
	  python3 tools/run_suite.py --name test_menugeom --min 75 -- /tmp/hd_tmg
	@echo "── test_accounts ──────────────────────────"
	@$(TESTCC) -Iinclude -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_tac tests/test_accounts.c && \
	  python3 tools/run_suite.py --name test_accounts --min 27 -- /tmp/hd_tac
	@echo "── test_tiles ─────────────────────────────"
	@$(TESTCC) -o /tmp/hd_tti tests/test_tiles.c && \
	  python3 tools/run_suite.py --name test_tiles --min 64 -- /tmp/hd_tti
	@echo "── test_vercmp ────────────────────────────"
	@$(TESTCC) -Itests -o /tmp/hd_tvc tests/test_vercmp.c && \
	  python3 tools/run_suite.py --name test_vercmp --min 30 -- /tmp/hd_tvc
	@echo "── test_fat16path ─────────────────────────"
	@$(TESTCC) -Iinclude -Wno-builtin-declaration-mismatch \
	           -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
	           -o /tmp/hd_tfp tests/test_fat16path.c && \
	  python3 tools/run_suite.py --name test_fat16path --allow-skip -- /tmp/hd_tfp
	@echo "── test_gptwrite ──────────────────────────"
	@echo "  adding a partition to a Windows disk without losing the disk"
	@$(TESTCC) -fno-builtin -Iinclude -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_tgw tests/test_gptwrite.c && \
	  python3 tools/run_suite.py --name test_gptwrite --min 102 -- /tmp/hd_tgw
	@echo "── test_esp ───────────────────────────────"
	@echo "  installing onto Windows' EFI partition without breaking Windows"
	@$(TESTCC) -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_tesp tests/test_esp.c && \
	  python3 tools/run_suite.py --name test_esp --min 114 -- /tmp/hd_tesp
	@echo "── test_fat16borrow ───────────────────────"
	@echo "  the writable borrow, and whose disk the writes land on"
	@# fs/gpt.c is LINKED, not stubbed: fs/fat16.c asks it whether a
	@# disk is GPT before falling back to the MBR, and a stub saying
	@# "no" would be a different program from the one that ships.
	@$(TESTCC) -Iinclude -Itests -Wno-unused-function \
	           -Wno-implicit-function-declaration \
	           -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_tfb tests/test_fat16borrow.c fs/gpt.c && \
	  python3 tools/run_suite.py --name test_fat16borrow --min 82 -- /tmp/hd_tfb
	@echo "── test_fat16fmt ──────────────────────────"
	@echo "  FAT16 geometry, as judged by fsck.vfat -- not by our own arithmetic"
	@$(TESTCC) -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_tf16f tests/test_fat16fmt.c && \
	  python3 tools/run_suite.py --name test_fat16fmt --min 97 -- /tmp/hd_tf16f
	@echo "── test_fat32fmt ──────────────────────────"
	@echo "  FAT32 for an ESP we make ourselves -- judged by fsck.fat and mtools"
	@$(TESTCC) -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_tf32f tests/test_fat32fmt.c && \
	  python3 tools/run_suite.py --name test_fat32fmt --min 94 -- /tmp/hd_tf32f
	@echo "── test_espmake ───────────────────────────"
	@echo "  making our own ESP -- disks laid out by sgdisk, judged by sgdisk and fsck.fat"
	@$(TESTCC) -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch -Wno-unused-function \
	           -o /tmp/hd_tespmk tests/test_espmake.c && \
	  python3 tools/run_suite.py --name test_espmake --min 50 --allow-skip -- /tmp/hd_tespmk
	@echo "── test_x509 ──────────────────────────────"
	@echo "  X.509 v3 + PKCS#1 v1.5 -- every verdict is openssl's"
	@$(TESTCC) -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch -Wno-unused-function \
	           -o /tmp/hd_tx509 tests/test_x509.c net/tls_crypto.c && \
	  python3 tools/run_suite.py --name test_x509 --min 45 --allow-skip -- /tmp/hd_tx509
	@echo "── test_wholedisk_size ────────────────────"
	@echo "  the size the whole-disk installer picks, on the disks people own"
	@$(TESTCC) -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_twds tests/test_wholedisk_size.c fs/fat16fmt.c \
	           fs/fat32fmt.c fs/instsize.c \
	           && \
	  python3 tools/run_suite.py --name test_wholedisk_size --min 124 -- /tmp/hd_twds
	@echo "── test_fat16gpt ──────────────────────────"
	@echo "  can the installed system find its own partition on a GPT disk"
	@$(TESTCC) -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_tf16g tests/test_fat16gpt.c && \
	  python3 tools/run_suite.py --name test_fat16gpt --min 56 -- /tmp/hd_tf16g
	@echo "── test_fat16nvme ─────────────────────────"
	@echo "  can an installed system on an NVMe drive read itself back"
	@$(TESTCC) -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_tf16n tests/test_fat16nvme.c && \
	  python3 tools/run_suite.py --name test_fat16nvme --min 21 -- /tmp/hd_tf16n
	@echo "── test_installadd ────────────────────────"
	@echo "  the whole install beside Windows: four sectors outside the partition"
	@$(TESTCC) -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_tia tests/test_installadd.c && \
	  python3 tools/run_suite.py --name test_installadd --min 200 -- /tmp/hd_tia
	@echo "── test_xhci ──────────────────────────────"
	@echo "  xHCI against a controller that executes its commands"
	@# -no-pie so a static ring's address still fits the uint32_t a
	@# controller register holds, and the simulated controller can
	@# follow the pointers the driver writes into it.
	@$(TESTCC) -no-pie -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch -Wno-unused-function \
	           -o /tmp/hd_txhci tests/test_xhci.c drivers/pci_bar.c \
	           && \
	  python3 tools/run_suite.py --name test_xhci --min 344 -- /tmp/hd_txhci
	@echo "── test_update ────────────────────────────"
	@echo "  where an update comes from, and whether it is what it claims to be"
	@# fs/gpt.c is linked rather than included: it declares memcpy and
	@# memset itself and cannot share a translation unit with anything
	@# that includes the kernel's <string.h>.
	@# fs/exfat.c is linked for a different reason -- v0.8.8.2 gave the
	@# USB update source an exFAT arm, and including it beside fs/fat32.c
	@# makes each redefine the other's SECT_SZ and ATTR_DIRECTORY.
	@$(TESTCC) -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_tupd tests/test_update.c fs/gpt.c fs/exfat.c \
	           && \
	  python3 tools/run_suite.py --name test_update --min 159 -- /tmp/hd_tupd
	@echo "── test_fat32 ─────────────────────────────"
	@echo "  FAT32 against an image made by mkfs.vfat -- the ESP an install needs"
	@$(TESTCC) -Iinclude -Itests -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_tf32 tests/test_fat32.c && \
	  python3 tools/run_suite.py --name test_fat32 --min 227 -- /tmp/hd_tf32
	@echo "── test_fat32sys ──────────────────────────"
	@echo "  the system volume on FAT32: parked, borrowed, found, renamed"
	@$(TESTCC) -Iinclude -Itests -Wno-builtin-declaration-mismatch \
	           -Wno-unused-function -o /tmp/hd_tf32s tests/test_fat32sys.c && \
	  python3 tools/run_suite.py --name test_fat32sys --min 112 -- /tmp/hd_tf32s
	@echo "── test_kbdinit ───────────────────────────"
	@echo "  the 8042 is given time to answer, during init as well as at runtime"
	@# drivers/keyboard.c is COMPILED here through io.h's IO_H seam. What
	@# is asserted is the ACCESS PATTERN -- no status read without a delay
	@# before it -- which is the rule v0.8.5.3 established for the runtime
	@# path and never applied to keyboard_init().
	@$(TESTCC) -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch -Wno-unused-function \
	           -o /tmp/hd_tkbd tests/test_kbdinit.c && \
	  python3 tools/run_suite.py --name test_kbdinit --min 42 -- /tmp/hd_tkbd
	@echo "── test_theme ─────────────────────────────"
	@echo "  previewing a theme is not choosing one"
	@# shell/ui.c is COMPILED here, not copied: the stubs are only the
	@# VGA/keyboard/clock/vfs edges, and vfs_write is an in-memory file
	@# that COUNTS writes -- which is how "a preview writes nothing" is
	@# checked rather than asserted.
	@$(TESTCC) -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch -Wno-unused-function \
	           -o /tmp/hd_tth tests/test_theme.c && \
	  python3 tools/run_suite.py --name test_theme --min 41 -- /tmp/hd_tth
	@echo "── test_blkdev ────────────────────────────"
	@echo "  one block layer, and the two orders it holds"
	@$(TESTCC) -Iinclude -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_tblk tests/test_blkdev.c && \
	  python3 tools/run_suite.py --name test_blkdev --min 81 -- /tmp/hd_tblk
	@echo "── test_exfat ─────────────────────────────"
	@echo "  exFAT, against a volume mkfs.exfat wrote and fsck.exfat passed"
	@# fs/exfat.c is LINKED, and the suite carries its own CRC-32
	@# rather than -lz: tools/inject.py builds with the compiler
	@# line followed by the test file, so a library named here would
	@# come before the sources that need it.
	@$(TESTCC) -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch -Wno-unused-function \
	           fs/exfat.c -o /tmp/hd_tex tests/test_exfat.c && \
	  python3 tools/run_suite.py --name test_exfat --min 107 -- /tmp/hd_tex
	@echo "── test_bignum ────────────────────────────"
	@echo "  limb arithmetic and RSA-2048, against CPython and openssl"
	@# The expected values in tests/bignum_vectors.inc come from
	@# CPython's integers and from `openssl genrsa`, NOT from
	@# net/bignum.c -- a bignum suite written from the same
	@# understanding as the bignum is two copies of one mistake.
	@# Regenerate with: python3 tools/make_bignum_vectors.py tests
	@$(TESTCC) -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch -Wno-unused-function \
	           net/bignum.c -o /tmp/hd_tbn tests/test_bignum.c && \
	  python3 tools/run_suite.py --name test_bignum --min 299 -- /tmp/hd_tbn
	@echo "── test_i2c ───────────────────────────────"
	@echo "  the DesignWare I2C core, against a simulated one"
	@# drivers/i2c.c is INCLUDED, not linked: the suite replaces its
	@# MMIO seam (I2C_RD / I2C_WR) with a simulated controller, and a
	@# seam only exists inside the translation unit.
	@$(TESTCC) -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch -Wno-unused-function \
	           -o /tmp/hd_ti2c tests/test_i2c.c && \
	  python3 tools/run_suite.py --name test_i2c --min 49 -- /tmp/hd_ti2c
	@echo "── test_gfx ───────────────────────────────"
	@echo "  the Iris Xe inspector, against a model display engine"
	@# drivers/xedisp.c is INCLUDED: the suite replaces its read seam
	@# (GFX_RD) and its physical-memory seam (GFX_PHYS). There is no
	@# write seam to replace, and no pci_cfg_write to link against --
	@# a write added to the driver stops this suite building.
	@$(TESTCC) -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch -Wno-unused-function \
	           -o /tmp/hd_tgfx tests/test_gfx.c && \
	  python3 tools/run_suite.py --name test_gfx --min 203 -- /tmp/hd_tgfx
	@echo "── test_edid ──────────────────────────────"
	@echo "  EDID over GMBUS, against a model controller and monitor"
	@# drivers/xegmbus.c is the first display code that WRITES; the
	@# suite replaces both seams (GFX_RD, GFX_WR) with a model that sees
	@# every write and fails any outside the six GMBUS registers.
	@$(TESTCC) -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch -Wno-unused-function \
	           -o /tmp/hd_tedid tests/test_edid.c && \
	  python3 tools/run_suite.py --name test_edid --min 106 -- /tmp/hd_tedid
	@echo "── test_xemode ────────────────────────────"
	@echo "  lighting pipe B on the HDMI socket, against a model engine"
	@# drivers/xemode.c is the first code that changes what a screen
	@# shows. The model starts from the HP's own registers, moves status
	@# bits the way the hardware does, fans PHY group writes out to the
	@# lanes, and counts every write that lands on pipe A's side.
	@$(TESTCC) -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch -Wno-unused-function \
	           -o /tmp/hd_txm tests/test_xemode.c && \
	  python3 tools/run_suite.py --name test_xemode --min 209 -- /tmp/hd_txm
	@echo "── test_rsa ───────────────────────────────"
	@echo "  Miller-Rabin and RSA keygen, against CPython and openssl"
	@# net/rsa.c arrived with a suite and a harness and was never added
	@# here, so `make test` did not run it -- which is the v0.8.5.6
	@# disease: 31 suites behind a `| tail -1` that hid a failing one.
	@# Regenerate the vectors with:
	@#   python3 tools/make_rsa_vectors.py tests
	@$(TESTCC) -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch -Wno-unused-function \
	           net/bignum.c net/rsa.c -o /tmp/hd_trsa tests/test_rsa.c && \
	  python3 tools/run_suite.py --name test_rsa --min 371 -- /tmp/hd_trsa
	@echo "── test_console ───────────────────────────"
	@echo "  the console grid, on both consoles, at sizes no VGA card has"
	@# -no-pie so the simulated panel's address still fits the uint32_t
	@# vga_fb_init() takes. VGA_MEM_PTR is v0.8.8's seam: it is what
	@# lets the TEXT console be compiled here at all -- without it a
	@# successful 80x25 mode change writes to 0xB8000 and dies.
	@$(TESTCC) -no-pie -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch -Wno-unused-function \
	           -Wno-int-to-pointer-cast \
	           -o /tmp/hd_tcon tests/test_console.c && \
	  python3 tools/run_suite.py --name test_console --min 217 -- /tmp/hd_tcon
	@echo "── test_der ───────────────────────────────"
	@echo "  writing DER, checked against the bytes -- stage 3"
	@# -fno-builtin and the kernel's own string.c: net/der.c net/x509.c's
	@# memmove is tested against the memcpy that SHIPS, not glibc's.
	@# See the suite's header -- the injection harness found that.
	@$(TESTCC) -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_tder tests/test_der.c && \
	  python3 tools/run_suite.py --name test_der --min 57 -- /tmp/hd_tder
	@echo "── test_pair ──────────────────────────────"
	@echo "  the pair request -- openssl VERIFIES the certificate chain"
	@# The judge that matters is `openssl verify -CAfile root.pem
	@# host.pem`: real signature verification by an independent
	@# implementation, which is the same check the device makes on the
	@# record it is asked to remember. Plus a negative control with an
	@# unrelated CA, so "verify succeeded" cannot mean openssl was not
	@# checking. 27 injected defects, all 27 caught. Under ASAN.
	@$(TESTCC) -g -fsanitize=address,undefined -Iinclude -Itests \
	           -o /tmp/hd_tpair tests/test_pair.c net/tls_crypto.c && \
	  python3 tools/run_suite.py --name test_pair --min 149 --allow-skip -- /tmp/hd_tpair
	@echo "── test_i2c_hid ───────────────────────────"
	@echo "  the trackpad, against a mock that follows the spec"
	@# drivers/i2c_hid.c finds a HID device on an I2C bus with no ACPI
	@# interpreter, by trying the registers a descriptor is ever at and
	@# accepting only one whose first four bytes ARE a descriptor. 29
	@# injected defects, 26 caught; the two survivors are redundancy
	@# and are named at the end of drivers/i2c_hid.c. Under ASAN.
	@$(TESTCC) -g -fsanitize=address,undefined -Iinclude -Itests \
	           -o /tmp/hd_tih tests/test_i2c_hid.c && \
	  python3 tools/run_suite.py --name test_i2c_hid --min 84 -- /tmp/hd_tih
	@echo "── test_hid_rd ────────────────────────────"
	@echo "  HID report descriptors, judged by hid_parser -- under ASAN"
	@# The parser that makes a trackpad possible: I2C-HID has no boot
	@# protocol, so the descriptor is the only way to know which bits
	@# are X. 28 injected defects, all 28 caught -- four of them by
	@# ASAN. Needs `pip install --break-system-packages hid-parser`;
	@# without it the suite SKIPS rather than passing on no judge.
	@$(TESTCC) -g -fsanitize=address,undefined -Iinclude -Itests \
	           -o /tmp/hd_thrd tests/test_hid_rd.c && \
	  python3 tools/run_suite.py --name test_hid_rd --min 87 --allow-skip -- /tmp/hd_thrd
	@echo "── test_der_read ──────────────────────────"
	@echo "  DER public keys, judged by openssl -- under ASAN"
	@# Built with -fsanitize=address,undefined, which is not decoration:
	@# the 294-prefix truncation sweep and the exact-size fixtures only
	@# mean something if a read one byte past the data is a fault. 22
	@# injected defects, 19 caught; the three survivors are redundancy
	@# and are named at the end of net/der_read.c.
	@$(TESTCC) -g -fsanitize=address,undefined -Iinclude -Itests \
	           -o /tmp/hd_tdr tests/test_der_read.c && \
	  python3 tools/run_suite.py --name test_der_read --min 83 --allow-skip -- /tmp/hd_tdr
	@echo "── test_b64 ───────────────────────────────"
	@echo "  base64, judged by Python -- 2010 cases plus the edges"
	@# v0.8.8.11 replaced FOUR base64 implementations with one, two of
	@# which took no output capacity at all. 19 injected defects, all
	@# 19 caught -- the last two only after the suite learned to feed
	@# it a byte with the top bit set and four pads on a quad boundary.
	@$(TESTCC) -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_tb64 tests/test_b64.c && \
	  python3 tools/run_suite.py --name test_b64 --min 57 --allow-skip -- /tmp/hd_tb64
	@echo "── test_plist ─────────────────────────────"
	@echo "  property lists, judged by Python's plistlib -- stage 3"
	@# There is no plist READER in HavenDOS, so this writer cannot be
	@# checked against one of ours even in principle. plistlib is the
	@# judge. --allow-skip because a Python without it should skip the
	@# suite, not silently pass it. 23 injected defects, 19 caught;
	@# the four survivors are written down at the end of net/plist.c.
	@$(TESTCC) -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_tplist tests/test_plist.c && \
	  python3 tools/run_suite.py --name test_plist --min 93 --allow-skip -- /tmp/hd_tplist
	@echo "── test_heap ──────────────────────────────"
	@echo "  the kernel allocator, against the pool"
	@# The oldest untested file in the tree until v0.8.8.5: seven bug
	@# fixes in its own header, every one found by reading.
	@$(TESTCC) -Iinclude -Itests -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_theap tests/test_heap.c && \
	  python3 tools/run_suite.py --name test_heap --min 62 -- /tmp/hd_theap
	@echo "── test_mines ─────────────────────────────"
	@echo "  the minefield's rules, against the board -- no seams"
	@# No stubs and no simulated hardware: shell/mines_model.c calls
	@# nothing, so it is linked straight in. include/mines.h says why
	@# the rules live in a file with no screen in it.
	@$(TESTCC) -Iinclude -Itests -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_tmines tests/test_mines.c && \
	  python3 tools/run_suite.py --name test_mines --min 79 -- /tmp/hd_tmines
	@echo "── test_kpoll ─────────────────────────────"
	@echo "  the tick sets flags, kernel_poll does the work, and the refusals"
	@# kernel/kpoll.c kernel/appguard.c is COMPILED here through include/kpoll_asm.h's
	@# seam, so the real read-and-clear runs -- and the suite can drive
	@# kpoll_tick() by hand, which produces the exact interleaving the
	@# cli is there to protect against.
	@$(TESTCC) -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch -Wno-unused-function \
	           -o /tmp/hd_tkp tests/test_kpoll.c && \
	  python3 tools/run_suite.py --name test_kpoll --min 156 -- /tmp/hd_tkp
	@echo "── test_netdev ────────────────────────────"
	@echo "  one NIC layer, its two orders, and what it refuses"
	@# NET_NO_HW keeps the PCI drivers out: this suite links net/netdev.c
	@# for the table, the dispatch and the words and has no hardware to
	@# call -- the same thing BLK_NO_HW does for fs/blkdev.c.
	@$(TESTCC) -no-pie -DNET_NO_HW -Iinclude \
	           -Wno-builtin-declaration-mismatch -Wno-unused-function \
	           -o /tmp/hd_tnd tests/test_netdev.c && \
	  python3 tools/run_suite.py --name test_netdev --min 121 -- /tmp/hd_tnd
	@echo "── test_usbnet ────────────────────────────"
	@echo "  Apple USB Ethernet, against a phone that answers"
	@# This one links the REAL usb/core.c and registers a simulated host
	@# controller through usb_register_hc(), so usb_select_config(),
	@# usb_set_interface() and the descriptor walk are the kernel's own.
	@$(TESTCC) -no-pie -DNET_NO_HW -Iinclude \
	           -Wno-builtin-declaration-mismatch -Wno-unused-function \
	           -o /tmp/hd_tun tests/test_usbnet.c && \
	  python3 tools/run_suite.py --name test_usbnet --min 125 -- /tmp/hd_tun
	@echo "── test_usbhid ────────────────────────────"
	@echo "  USB keyboards and mice, on every interface of a device"
	@# v0.9.2.1: a wireless receiver's mouse is on interface 1. Links
	@# the REAL usb/core.c (survey, usb_claim_interface) and usb/hid.c.
	@$(TESTCC) -no-pie -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch -Wno-unused-function \
	           -o /tmp/hd_tuh tests/test_usbhid.c && \
	  python3 tools/run_suite.py --name test_usbhid --min 30 -- /tmp/hd_tuh
	@echo "── test_usbmux ────────────────────────────"
	@echo "  the Apple Mobile Device channel, against a device that speaks it"
	@$(TESTCC) -no-pie -Iinclude \
	           -Wno-builtin-declaration-mismatch -Wno-unused-function \
	           -o /tmp/hd_tmux tests/test_usbmux.c && \
	  python3 tools/run_suite.py --name test_usbmux --min 378 -- /tmp/hd_tmux
	@echo "── test_lockdown ──────────────────────────"
	@echo "  lockdownd stage 1: QueryType over the real mux and its TCP"
	@$(TESTCC) -no-pie -Iinclude \
	           -Wno-builtin-declaration-mismatch -Wno-unused-function \
	           -o /tmp/hd_tldn tests/test_lockdown.c && \
	  python3 tools/run_suite.py --name test_lockdown --min 365 -- /tmp/hd_tldn
	@echo "── test_dhcp ──────────────────────────────"
	@echo "  DHCP through the real IP and UDP layers, against a simulated server"
	@# v0.8.9.8. DHCP had never worked: the broadcast was ARPed for, UDP
	@# refused 548 bytes, and the parser wanted replies no server sends.
	@# The fallback address is QEMU's own, so nothing showed it until an
	@# iPhone handed out 172.20.10.x.
	@$(TESTCC) -g -fsanitize=address,undefined -fno-builtin -Iinclude \
	           -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_tdhcp tests/test_dhcp.c && \
	  python3 tools/run_suite.py --name test_dhcp --min 23 -- /tmp/hd_tdhcp
	@echo "── test_tlscc ─────────────────────────────"
	@echo "  TLS client certificates -- openssl s_server is the judge"
	@# Pairing stage 3's second half. lockdownd will not open a session
	@# in the clear: it asks for TLS with a client certificate, and the
	@# thing that decides whether ours works is a real TLS 1.2
	@# handshake with `openssl s_server -Verify 1 -CAfile <our root>
	@# -verify_return_error`, which makes the same decision the phone
	@# makes. net/tls.c's tcp_send/tcp_recv are implemented on a POSIX
	@# socket in the suite; there is no mock server anywhere in it.
	@#
	@# -verify_return_error is load-bearing and was found by the
	@# suite's own negative control: without it s_server prints
	@# "verify error" and completes the handshake anyway. 26 injected
	@# defects, all 26 caught. Under ASAN.
	@$(TESTCC) -g -fsanitize=address,undefined -fno-builtin \
	           -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch -Wno-unused-function \
	           -o /tmp/hd_tcc tests/test_tlscc.c net/tls_crypto.c && \
	  python3 tools/run_suite.py --name test_tlscc --min 64 --allow-skip -- /tmp/hd_tcc
	@echo "── test_msc ───────────────────────────────"
	@echo "  USB storage against a stick that executes its commands"
	@$(TESTCC) -Iinclude -Itests -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_tmsc tests/test_msc.c && \
	  python3 tools/run_suite.py --name test_msc --min 114 -- /tmp/hd_tmsc
	@echo "── test_hub ───────────────────────────────"
	@echo "  a device behind a hub, and the route string that addresses it"
	@$(TESTCC) -Iinclude -Itests -Wno-unused-function \
	           -Wno-builtin-declaration-mismatch \
	           -o /tmp/hd_thub tests/test_hub.c && \
	  python3 tools/run_suite.py --name test_hub --min 99 -- /tmp/hd_thub
	@echo "── test_machine ───────────────────────────"
	@echo "  reset and power off, against a machine that refuses to die"
	@# kernel/machine.c is INCLUDED by the suite, not linked: the two
	@# instructions a native test cannot run -- the triple fault and the
	@# park -- are replaced through include/machine_asm.h's MACHINE_ASM_H
	@# seam, the same trick io.h and mmio.h use. So the real escalation
	@# runs here, and because nothing actually resets, every fallback
	@# stage is reached in one run.
	@$(TESTCC) -fno-builtin -Iinclude -Itests \
	           -Wno-builtin-declaration-mismatch -Wno-unused-function \
	           -o /tmp/hd_tmach tests/test_machine.c && \
	  python3 tools/run_suite.py --name test_machine --min 46 -- /tmp/hd_tmach

clean:
	rm -f tests/appreg.inc $(OBJS) $(OBJS_PASS1) $(OBJS:.o=.d) $(OBJS_PASS1:.o=.d) \
	      havendos.elf havendos_pass1.elf havendos.iso \
	      HavenDOS-*.iso \
	      isodir/boot/havendos.elf include/kernel_elf.h include/grub_disk_embed.h \
	      tests/bars.inc tests/vercmp.inc \
	      tests/fat32.img tests/fat16.img tests/fat12.img tests/fat32_fixture.inc \
	      tests/fat32.img.scratch tests/fat32.img.part tests/fat32.img.fsck \
	      tests/fat32.img.scratch.part tests/fat32.img.scratch.fsck \
	      tests/exfat.img tests/exfat_fixture.inc \
	      tests/esp.img tests/esp_full.img tests/esp_fixture.inc \
	      tests/esp.img.scratch tests/esp.img.scratch.fsck \
	      tests/esp_full.img.scratch tests/esp_full.img.scratch.fsck \
	      include/grub_efi_embed.h tests/f16fmt.img tests/f16fmt.img.out

.PHONY: all clean test

# The per-object header dependencies gcc wrote with -MMD.  "-include" so a
# clean tree, where none of them exist yet, is not an error.
-include $(OBJS:.o=.d) $(OBJS_PASS1:.o=.d)
