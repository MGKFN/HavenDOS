# HavenDOS v0.8.7 "Iris"

**Channel:** Beta.  **Date:** 25 October 2026.

iPhone tethering: one NIC layer, alternate settings, and a driver for
Apple USB Ethernet. **None of it has been on hardware** —
`docs/USB-ETHERNET-HARDWARE-TEST.md` is the five minutes that settle it.

---

## The grep 0.8.6 asked for

0.8.6 "Argo" closed by instructing its successor to grep `net/` for the
bus-dispatch shape before adding a NIC. **`net/ethernet.c` was clean** —
it calls `nic_send()` / `nic_recv()` and names no driver, and always has.

The shape was one layer below, in **`net/net.c`**: three switches on one
`static int nic_type` (`net_get_mac()`, `nic_send()`, `nic_recv()`), a
fourth place deciding the probe order inside `net_init()`, and the
numbering — `0=none 1=rtl8139 2=e1000` — recorded only as a comment
above the variable. Three of the four copies `fs/blkdev.c` was created
to delete.

Adding a USB NIC to that cost four edits, and forgetting the one in
`net_get_mac()` produces a machine that sends ARP replies claiming
`00:00:00:00:00:00` and cannot be told why: the frames go out, the
counters rise, nothing comes back.

**So the table was written before the driver.**

- **`net/netdev.c`** + `include/netdev.h` own the table, the dispatch,
  the per-NIC refusal, the bus word, both orders and the cached MAC.
- **`tools/check_nic_dispatch.py`** refuses a driver call outside it.
- **Registration order is identity; preference is a separate question**
  with a separate function — the thing the block layer had to be taught
  twice. `netdev_preferred()` is e1000, RTL8139, then USB;
  `netdev_set_active()` overrides it.
- **The MAC is read once, at registration.** On a PCI NIC that is a
  register read. On a USB NIC it is a **control transfer**, and
  `net/arp.c` asks twice per reply — so the failure mode is not
  slowness but an address arriving mid-transfer and an ARP reply that
  lies about who we are.
- **`net_ready()` is a question about the table now**, not a flag
  latched at boot, because a USB adapter can be unplugged.
- `netdev_unregister()` **compacts** the table, so a pinned index can
  come to mean a different NIC. That is the one silent failure in the
  file and the harness plants it twice.

`net/net.c` keeps the IP configuration and three one-line accessors
meaning "the active NIC".

## Alternate settings

The phone's Ethernet interface has **no endpoints on alt 0**; the two
bulk endpoints are on alt 1 and alt 2. The descriptor walk had refused
every alternate but 0 since it was written, for a reason that is still
right — *"adopting one by accident is how a device ends up with
endpoints it was never told to use"*.

**The refusal became a choice, not a removal.** `usb_walk_config_alt()`
is the walker; `usb_walk_config()`, `usb_parse_config()` and
`usb_parse_config_alt()` are wrappers, all defaulting to alt 0, so
every caller predating this release is unchanged.

- **`usb_select_config(d, idx, want_if)`** — `idx` is a descriptor
  **index**, not a `bConfigurationValue`. New field `usb_dev_t::cfgidx`
  remembers it: a device is free to number its configurations 1, 3, 4,
  7 while they sit at indices 0, 1, 2, 3, and deriving one from the
  other would be guessing at a mapping the device never promised.
- **`usb_set_interface(d, ifnum, alt)`** walks into a **scratch copy**
  and adopts it only once the device and the controller have both
  agreed. The walk must run before the request is sent, because the new
  setting's endpoints are in the descriptor — and a device left on its
  old setting facing a kernel describing the new one sends bulk
  transfers to endpoints that do not exist. Neither controller reports
  that as "the interface did not switch".
- **`fetch_config()`** — the nine-bytes-then-the-rest descriptor fetch
  existed **twice** and this release needed it twice more. One copy now.

## xHCI: Configure Endpoint is called twice now

Nothing before this release configured a slot twice, so four things
were true by construction and stopped being true:

1. **No DROP flags, ever.** Dword 0 of the input control context was
   left at zero for four releases. Switching settings left the previous
   one's endpoint contexts live, pointing at rings re-laid underneath
   them.
2. **`if (add == 1u) return USB_OK;`** — a switch to a setting with no
   endpoints issued **no command at all**, so the old endpoints stayed
   configured *and* the kernel reported success. That is exactly alt 0
   of the Apple Ethernet interface.
3. **The rings leaked.** `XHCI_EP_RINGS` is five; alt 1 and alt 2 use
   four different endpoint addresses between them, so two switches
   exhausted them. The v0.8.6.1 slot leak one level down.
4. **A reused ring was not re-laid.** `ring_alloc()` returns the
   existing ring for a DCI the slot holds and `x_configure()` writes the
   dequeue pointer back to the ring **base**, so the controller
   dequeued a TRB the driver wrote on the previous setting.
   `x_configure()`'s own comment has claimed since v0.8.5.1 that a
   configure re-lays the rings; clearing `ep_busy`/`ep_done` is not
   re-laying them.

Also found: **changing configuration needs the drop too.** The first
version of the test expected no command when selecting a configuration
whose interface has no endpoints. It failed, and the failure was right —
the boot configuration's interrupt endpoint has to be taken away.

`tests/test_xhci.c`'s simulated controller **ignored the drop flags**,
which is the Disable Slot situation of v0.8.6.1 verbatim: it could not
see that the driver never sent one. It honours and counts them now.

## Apple USB Ethernet

**`usb/ethernet.c`** + `include/usbnet.h`. Registers through
`netdev_register_usb()`, so nothing above the NIC layer knows a frame is
leaving over USB.

**Confirmed, from `usb cfg` on the HP:** class `FF/FD/01` on interface 2
of configuration index 3, endpoints on alts 1 and 2 and none on alt 0.

**Not confirmed:** the framing. The two bytes prepended on receive, the
fixed 1514-byte zero-padded transmit, and the vendor requests `0x00`
(MAC) and `0x45` (carrier) at wIndex 2 are what the published Linux
`ipheth` driver does. No Apple document says so and this kernel has
never exchanged a byte with the interface.

So the driver ships with the evidence built in: it keeps the **first
transfer exactly as it arrived, before stripping anything**, and
**`usb eth`** prints it raw. On a freshly tethered link the broadcast
traffic settles it — two bytes and then `FF FF FF FF FF FF`, or not.

Every refusal is distinct and logged by name — no `FF/FD/01` anywhere,
`SET_CONFIGURATION` refused, `SET_INTERFACE` refused, no bulk pair,
`GET_MACADDR` refused — because each has a different fix. That is
v0.8.6.5's lesson about `update` naming which of four reasons a stick
was not used.

**A short MAC answer refuses the whole NIC.** Three bytes of address and
three of whatever was in the buffer would be cached by `net/netdev.c`
and put in an ARP reply by `net/arp.c`, poisoning every cache on the
network.

### `usb_bulk_poll()`

Nothing in this kernel runs on a timer and `net_poll()` runs from the
shell's idle loop, so a blocking bulk IN per poll is a shell that stops
answering — v0.8.5's keyboard at one character per second, one endpoint
type along. `intr_poll` turned out to be entirely endpoint-type
agnostic, so one op serves both and the only difference is which
blocking call it falls back to on UHCI.

---

## Found in already-shipped code

### A bare opt-out marker silenced three build checks

All three tested the reason with `\s*\S`, and in C the character after a
bare marker is the `*` of the `*/` that closes the comment — so

```c
/* blk-dispatch-ok: */
/* bus-label-ok: */
```

silenced them with no reason at all. All three documented the rule in
the same words — *"a bare marker is how an exemption becomes a habit"* —
and none enforced it. `check_blk_dispatch.py` and `check_bus_label.py`
have shipped that way since 0.8.6.

Fixed in **`tools/csource.py::has_reason()`**, beside the
comment-stripping, with ten cases. Found by the third of the three cases
a new checker has to pass — the one that is easy not to write. The first
two both passed.

### Two modules' private state merged, silently

`tests/test_usbnet.c` includes the **real** `usb/core.c`, because the
driver's job is three requests through the core against a simulated
phone and a stubbed core would let the driver pass while the core's half
of alternate settings was wrong.

It segfaulted. `net/netdev.c` and `usb/core.c` both had
`static int g_ndev;` — and an uninitialised `static int` is a
**tentative definition**, which C permits any number of in one
translation unit. The two **merged into one variable**:
`usb_enumerate()` set it to the USB device count and `netdev_count()`
read it as the NIC count, so the NIC layer dispatched through a slot
whose ops pointer was null.

No error, no warning, at any `-W` level — `gcc -Wall -Wextra` builds it
clean. The arrays clashed loudly and took a minute; the counter took a
sanitiser.

**`tools/check_statics.py`** refuses it now, scoped to the files each
suite actually includes together — two unrelated drivers sharing a
counter name is fine and always will be.

### A backgrounded harness left an injected defect in the tree

Not a code defect, a process one, and worth recording because it nearly
shipped. The injection harnesses damage the source on purpose and restore
it in a `finally`. The v0.8.7 sweep was run in the background, the process
was killed during `check_nvmerw_suite.py`, and `drivers/nvme.c` was left
with the NVMe WRITE opcode changed from `0x01` to `0x02` — which is READ.

`make` built it. The ISO built. `make test` caught it at 66 of 83, with
`nvme_write()` returning **success** while the simulated controller logged
reads — so the only thing that found it was the suite checking the wire
instead of the return value, which is this release's recurring lesson
arriving from an unexpected direction.

`HANDOFF.md` now says to run the harnesses in the foreground.

### `include/usb.h` names a file that does not exist

It refers to `tests/test_usb.c`; the descriptor walk is tested in
`tests/test_xhci.c`. Same class as "a header that had never matched its
function".

### Eleven harnesses still carry their own loop

`tools/inject.py` states that *"every harness declares a HARMLESS
change, it is run last, and the harness FAILS if it is reported as
caught"*. It was extracted in 0.8.6 and only six harnesses were moved
onto it; eleven still have their own copy of the loop and no harmless
self-check, so they cannot show that `MISSED` is reachable at all or
that their damaging injections were fully restored. That is the specific
bug `check_xhci_suite.py`'s first version had.

**Open.** Not fixed in this release.

---

## Testing

| suite | first run | after the harness |
|---|---|---|
| `test_netdev` | 97/97 | **121/121**, 30 of 30 caught |
| `test_xhci` | 218 → 287 | **294/294**, 76 of 76 caught |
| `test_usbnet` | 95/95 | **110/110**, 28 of 28 caught |

**All three passed first time, and all three were wrong.**

`test_netdev`'s seven holes, by shape:

- **Three were fixtures that could not distinguish anything.** NICs
  registered with unit equal to index, so `ops->send(unit)` passed for
  `ops->send(i)`; two NICs sharing one bus word, so `g_nic[0].ops->name`
  passed for `g_nic[i]` — the v0.8.3.1 bug verbatim.
- **Four were properties masked by a guard elsewhere**, failing only in
  pairs — v0.8.6.7's two bugs again. The suite reads `g_nic`,
  `g_active` and `g_pinned` directly for those four, and says so.

`test_usbnet`'s seven, the same two shapes:

- A static transmit buffer is already zero on a fresh probe, so a
  missing `memset` is invisible until a real frame dirties it first.
- The runt check read the **return value** where it had to read the
  **counter**: a two-byte transfer yields a zero-length frame either
  way, so `netdev_recv() == 0` cannot tell a refused runt from a counted
  phantom.

`test_xhci`'s two:

- The ring-relay check switched between settings whose addresses all
  differ, so `ring_free()` zeroed the cursor and covered for the missing
  `ring_relay()`. The fixture gained **alt 3, which shares endpoint
  0x83 with alt 1** — the only case where re-laying is observable.
- "An unconfigured device cannot switch interface" used a zeroed device,
  which fails the transfer anyway, so `USB_ENODEV` came back either way.

**Two injections proved not to be defects** and are recorded in their
harnesses rather than deleted, following v0.8.6.3's precedent:

- Context Entries raised above the highest DCI in use — the controller
  considers endpoint contexts whose state is Disabled, and ignores them.
- `usbnet_release()`'s ordering — `netdev_unregister()` does not call
  into the driver's ops and `unit_of()` refuses a slot whose `used` is
  clear, so either guard alone is enough.

**37 suites running, 2774 checks**, up from 36 and 2543.
**18 injection harnesses.**

### Build checks

`make` runs thirteen now. Two are new:

| check | what it refuses |
|---|---|
| `check_nic_dispatch.py` | a NIC driver call outside `net/netdev.c` |
| `check_statics.py` | two modules one suite includes sharing a static object |

---

## Not done

- **Everything above, on hardware.** See
  `docs/USB-ETHERNET-HARDWARE-TEST.md`.
- **Whether tethering needs pairing.** Interface 1 is `FF/FE/02` —
  usbmux, the lockdown channel. Whether the Ethernet interface is merely
  *offered* without a trust handshake or also *usable* is not written
  down anywhere, and only the HP can say.
- **Alt 2 is not used.** On a recent iOS it is NCM framing, which is a
  different driver.
- **Interrupt endpoints on a hub-attached device** have still never been
  exercised; `link_up` over an interrupt IN endpoint will be the first
  `0` this tree's NIC layer can produce.
- **The hub walk** is still QEMU-only.
- The eleven harnesses above.
