# HavenDOS v0.8.7.5

**Channel:** Beta.  **Codename:** Iris.  **Date:** 26 October 2026.

v0.8.7.4 was built to name the completion code behind `usb cfg`'s
`(3)`. It never had to: the photographs of its own boot were enough.

**The event ring had two readers and no owner.** One timed-out transfer
poisoned every control transfer on every device on the controller.

---

## What `(3)` actually was

`evt_wait()` returned the next event of the right **type** and never
checked which slot or endpoint the event belonged to:

```c
    if (want_type && type != want_type) continue;
    if (status_out) *status_out = st;
    return (int)((st >> 24) & 0xFFu);        /* whoever's event this was */
```

A controller has one event ring and many things waiting on it — every
endpoint of every device, plus the command ring. The comment that
justified this argued that `cmd_run()` and `x_data()` "run to completion
before any poll happens."

They do not. **A transfer that times out returns with its TRB still live
in the ring** — which the interrupt-poll comment forty lines further
down says in so many words, about this same driver:

> *"So x_data() waits out its full 1000 ms on every poll where no key was
> pressed, and then returns with the TRB still live in a ring of
> sixteen."*

It was diagnosed for the keyboard in 0.8.5.1 and never carried back to
the synchronous reader.

So, on the HP, in order:

1. `usbnet_probe()` runs and succeeds — it happens before any
   networking, which is why `NIC: Apple USB Ethernet` was green.
2. DHCP writes to an untrusted iPhone's bulk OUT. Nothing answers.
   `x_data()` times out and abandons the TRB, several times over as
   DHCP retries across 2.2 seconds.
3. Those TRBs finish. Several transaction-error events land on the ring
   with no waiter.
4. `usb cfg` runs six descriptor reads. Each takes one of those
   abandoned transfers' completion codes as its own result.

**Six reads, six `(3)`, on three devices, two of which were working
perfectly.** `USB_EXFER` is `-3`; that was the number.

It also accounts for every observation that did not fit:

| what was seen | why |
|---|---|
| the 26 Oct 10:18 boot with no phone was healthy | no NIC, no DHCP, no timeout, nothing abandoned |
| the probe succeeded while `usb cfg` failed on the same boot | opposite sides of the DHCP attempt |
| rescans "made it worse" | each one runs DHCP again |
| `usb mux` found no Apple Mobile Device interface | it was reading other transfers' completion codes |

"Degrades over rescans" was never real. **One timeout was enough.**

---

## One owner for the event ring

`evt_wait()` and `evt_drain()` are gone. `evt_pump()` replaces both: it
drains the whole ring and files each event where its owner will look for
it. Two rules it never breaks.

**Nothing is discarded for being the wrong type.** A command completion
met by a transfer's wait is filed, not thrown away. `evt_wait_cmd()` and
`evt_wait_ep()` read from where it was filed.

**Nothing stops the walk.** An event is consumed — cursor advanced, ERDP
written back — *before* anything decides what it is. An event for a
torn-down endpoint, a port-change event, an event nobody owns: each is
consumed and dropped. The old `evt_drain()` **returned** on anything it
could not attribute, leaving it at the head of the ring where it blocked
every other consumer on the controller permanently. Unplugging anything
would have done it.

Ownership is stated, not guessed:

- `ep_busy` — an asynchronous transfer is outstanding.
- `ep_sync` — a caller is in `evt_wait_ep()` for this endpoint, set
  before the doorbell so an event cannot arrive unowned.

Neither set means the event is nobody's, and it goes in the bin. That is
what makes a timeout survivable.

## A timeout now cleans up after itself

`evt_wait_ep()` no longer walks away from a live TRB. On timeout it
clears `ep_sync` — disowning the event still to come — and calls
`ep_stop()`: **Stop Endpoint**, so the controller stops retrying a
transfer the caller has already been told failed, then **Set TR Dequeue**
so the endpoint resumes where we will write next instead of staying
Stopped on a TRB nobody will collect.

## Enable Slot reads its reply from the reply

The slot id was read off the event ring entry *behind* the cursor. That
was only ever right because the old `evt_wait()` stopped at the first
event it took; `evt_pump()` drains the ring, so anything landing behind
the completion ends up under that cursor. A polled keyboard finishing
while a command is in flight does exactly that. The completion's control
word is kept in `cmd_control` and read from there.

## Boot asks the link before it asks the network

A USB Ethernet adapter that is really a phone reports no carrier until
the phone is actually sharing its connection. Boot now asks:

```
    warn NET: Apple USB Ethernet reports no carrier; DHCP skipped
    warn NET: eth0 on the fallback 10.0.2.15, and nothing is on the
         other end of the cable
```

"No carrier" names the cause; "no DHCP answer" only described a symptom
of it. This check is itself a control transfer, so **until the event ring
had one owner its answer could not be trusted either** — which is why
0.8.7.4 could not have used it. It also returns the 2.2 seconds that
boot spent asking a network that was not there.

## Version macros that had drifted

`HAVENDOS_VER_PATCH` and `VER_BUILD` still read 0.8.6.7 through the whole
of 0.8.7, disagreeing with `VER_NUM`. Nothing in the tree reads them,
which is why nothing complained. A stale number nobody uses is still a
number somebody will one day believe. Corrected, and they now agree.

---

## Tests

`tests/test_xhci.c` — **310**, from 294.

The simulator gained three things it could not previously represent, all
of which the laptop does:

- `g_ep_dead` — an endpoint that answers nothing at all, so no event is
  ever posted and the TRB stays live. `g_nak` modelled this for a polled
  interrupt endpoint; the synchronous paths had nothing.
- real **Stop Endpoint** and **Set TR Dequeue**, so the recovery is
  executed rather than assumed — including the Stopped event arriving
  with nobody waiting for it.
- `g_trail_evt` — a transfer event landing *behind* a command
  completion, which is the only arrangement that tells apart reading a
  command's reply from the ring cursor and reading it from the reply.

The two tests that proved `evt_drain()` left a foreign event alone both
ended by consuming it with an explicit `evt_wait()`, "so the tests after
this one start clean". **That hand cleanup was the thing the real system
never did**, and it hid the defect for three releases. Both now assert
the new contract instead.

`tools/check_xhci_suite.py` — **82 injections, all caught**, from 76. Two
of the four that no longer matched the rewritten source were retargeted;
the two that described `evt_drain()` were replaced by eight for the new
design. The harness found **two holes in the new tests on its first run**
— one a missing test, one an injection whose stated consequence did not
follow, because consuming the event before judging it makes an early
return harmless. Both fixed.

---

## What this does not fix

**The iPhone still will not tether.** iOS does not activate the
tethering interface for a host with no pairing record, and that
diagnosis stands. What changes is that the stack around it is no longer
lying: `usb cfg` should be healthy on a boot with the phone attached, and
`usb mux` should now find the Apple Mobile Device interface it has been
unable to see.

Worth saying plainly: this was never really a networking bug. Any
timed-out transfer on any device silently corrupted the next one's
result — storage, keyboard, anything. Tethering is simply what made it
happen every boot.
