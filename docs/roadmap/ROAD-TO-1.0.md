# HavenDOS — road to 1.0: the pixel desktop (decided 24 Nov 2026, updated 25 Nov)

Kyan, 24 Nov, after 0.9.7.10: users and friends say HavenDOS "feels old". Goal: a modern OS feel.
"Modern DOS" can't be used as a term. The **name stays HavenDOS** (established in the community and in
search). The retro side lives on in Terminal/Shell Mode, Retro PC and the Legacy games.

## Decisions
- **Back to Beta.** Numbers go 0.9.8, 0.9.9, 0.9.10 … as high as needed until **HavenDOS 1.0 Stable**.
  The new version scheme (27100.1000-style) is **not** used.
- **HavenDOS 1.0 moves from January to spring** (roughly March–April).
- **Step by step, not a rewrite** (option 1): the desktop goes pixel first, then pixel building blocks,
  then apps one by one; nothing breaks along the way.
- **80x25 is dropped for everything.** The Shell is renamed **Terminal**, a resizable window (still a
  character grid inside, like any terminal). The framebuffer stays (it IS the pixel display); the
  character-grid UI style goes.
- **UI font: IBM Plex Sans** for the desktop (taskbar, Start menu, titles, buttons). Plex Mono stays in
  the Terminal and for code.
- **Games:** the current ones are kept as **Legacy** (old DOS style), and new pixel versions are made.
- **Installer languages for 1.0:** English, Dutch, German, French, Spanish.
- **25 Nov: the goal is the Windows/Linux model.** Kyan: "if we transitioned to the pixels we have kinda
  our same system like windows or linux?? bc i want smth like that". Yes: after 0.9.9–0.9.14 every window
  is its own pixel surface, apps draw with building blocks, the window manager composites. Still
  different after 1.0: drawing on the CPU (not the GPU), apps built in and not memory-protected, no
  third-party apps yet. **Next release after 0.9.8.2 is 0.9.9** (Kyan: "sure").

## Open question (25 Nov) — editions, 32/64-bit, "lightweight"
Kyan: *"why keep it light weight thats why the 32 bit version will get the old renderer legacy and 64 bit
our new system?? idk if possible?? bc i want to transition to more usable keeping everything lightweight
makes it feel sluggish i aim for older PCs but that would be then with smth like editions idk?? whats
smart??"*

**Recommendation (waiting on his yes):**
- **Don't tie the new desktop to 64-bit.** HavenDOS has no 64-bit kernel yet. The port is 1–2 releases of
  its own (see `claude/havendos-xe-graphics-and-64bit.md`). Tying the desktop to it puts every 0.9.9+
  step behind the port.
- **The pixel desktop is not what makes it heavy or slow.** It runs on any PC with a framebuffer
  (VESA/GOP, so most PCs since ~2000), 32-bit included. "Sluggish" comes from redrawing too many pixels
  per change, which is fixable (0.9.10 building blocks, dirty rectangles, faster blits). Usable and
  lightweight are not opposites.
- **One HavenDOS, one ISO, with a Classic mode:**
  - *HavenDOS* (default): the pixel desktop, on 32-bit and, later, 64-bit.
  - *Classic* (a boot-menu entry, chosen automatically where there is no framebuffer or under ~64 MB
    RAM): today's character-grid desktop and apps, frozen as Legacy, bug fixes only, no new features.
    It costs almost nothing, because that code exists already.
- **64-bit after 1.0** (or alongside the RCs if there is time): the same desktop, the ISO carries both
  kernels and GRUB picks. It's needed for >4 GB RAM and 64-bit-only UEFI machines, not for the look.
- **Editions** (Home/Lite/…) only later, if there is a real reason (different app sets). They are a
  packaging choice, not a renderer split.
- Alternatives if he prefers: (B) the 32-bit = Legacy / 64-bit = new split as he described (blocks the
  desktop on the port, doubles upkeep); (C) separate editions now.

## Roadmap (about a week per release; dates are estimates)
| Release | Scope |
|---|---|
| 0.9.8 Beta — the desktop | Pixel wallpaper, taskbar, Start menu with search, ~65 pixel icons, Plex Sans, pixel window buttons, draggable stacking windows. Apps inside unchanged. **Shipped 24 Nov.** |
| 0.9.8.1 | Pointer in pixels with acceleration. **Shipped 25 Nov.** |
| 0.9.8.2 | Shell scrolls inside its window; maximise keeps title bar + taskbar; power and notifications flyouts; icons with names under them, bigger. **Shipped 25 Nov, not yet tested by Kyan.** |
| 0.9.9 — resizable windows | **Each window its own pixel surface.** Resize with cursors; apps fill their window; no 80x25 anywhere; several windows side by side. Shell → Terminal. (+ Classic mode boot entry, if Kyan says yes.) |
| 0.9.10 — building blocks | Pixel buttons, toggles, checkboxes, lists, scrollbars, text fields, dialogs, menus, tooltips, progress bars; theme colours; High Contrast. Redraw only what changed (speed). |
| 0.9.11 | Settings and Files rebuilt. Files on the desktop. |
| 0.9.12 | Text Editor, Notes, Calculator, Calendar, Notifications, Package and User Manager. |
| 0.9.13 | Installer, first setup, sign-in/lock, Terms/Privacy, shutdown, boot screens (can be pulled forward). |
| 0.9.14 | HavenCode, Hex Viewer, Paint, remaining tools. No grid left outside the Terminal. |
| 0.9.15 | New pixel games; the old ones kept as Legacy. |
| 0.9.16 | HTTPS: TLS 1.2, X25519 + AES-GCM, `update online`. |
| 0.9.17 | Languages: EN, NL, DE, FR, ES. |
| 0.9.18+ | Release candidates: bug sweeps, testing on the HP, freeze. |
| 1.0 Stable | Spring 2027. |
| after 1.0 | 64-bit kernel (same desktop, both kernels on the ISO); then real app separation; GPU drawing much later. |

Supersedes the January 1.0 date and "0.9.8 = HTTPS" in THS-RM-1005 REV E; RM-1005 needs a REV F.

## Retro PC (own repo, MGKFN/RetroPC)
- At **v0.1.1** (23 Nov, pre-release). Its own roadmap (`ROADMAP.md` in that repo): 0.2 one PC (386 starts
  in real mode with the BIOS) → 0.3 disks + CD boot → 0.4 graphics → 0.5 486 → 0.6 Pentium → 1.0 installs
  Windows XP from a CD → 1.x 64-bit → 2.0 full speed via the host CPU (WHPX/KVM) for Windows 10/11.
- Kyan, 25 Nov: "in RetroPC it's laggy". Cause: every instruction is interpreted in software (~70 M/s).
  The GPU wouldn't help with that. Real speed is RetroPC 2.0 (host virtualization); before that, a faster
  interpreter (instruction cache / decoded-block cache) is the realistic win.
- Next for Retro PC: 0.2. HavenDOS copies `emu/` from that repo.
