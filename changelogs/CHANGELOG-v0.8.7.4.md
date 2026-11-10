# HavenDOS v0.8.7.4

**Channel:** Beta.  **Codename:** Iris.  **Date:** 26 October 2026.

A diagnostic release. **It fixes nothing and explains everything**, which
is the right trade when the alternative is a fifth guess.

---

## "(3)" was six faults wearing one number

On 26 October `usb cfg` on the HP reported, for every configuration of
every device on the bus — the phone, the webcam, and the MediaTek
Bluetooth that no driver has ever touched:

```
    the device would not describe it (3)
```

3 is `USB_EXFER`. `x_control()` returns it for **every xHCI completion
code that is not success, short-packet or stall**, and those are at
least six different faults with six different causes:

| completion code | what it actually means |
|---|---|
| 4 USB Transaction Error | the device did not answer |
| 19 Context State Error | an endpoint left in the wrong state |
| 11 Slot Not Enabled | the controller has forgotten the device |
| 5 TRB Error | a ring the controller cannot read |
| 2 Data Buffer Error | a transfer longer than the buffer |
| 21 Event Ring Full | events arriving faster than they are consumed |

Each sends you to a different place, and `(3)` sends you to the wrong
one. That is v0.8.6.5's lesson — `update` naming **which** of four
reasons the stick was not used — never applied to the layer underneath
everything else.

**The completion code is named now.** `cc_name()` covers all 36 the
specification defines, and both paths report it:

- `x_control()` logs every failure. Control transfers are rare — a
  descriptor fetch, a `SET_CONFIGURATION` — so there is nothing to
  rate-limit, and this is the line that answers the question.
- `x_data()` logs **once per endpoint**, and clears the latch on the
  next success. A bulk IN polled from the idle loop fails thousands of
  times a minute when a device goes quiet; v0.8.7 counted 19,440. One
  line is informative, 19,440 is a screen nobody can read.

`usb cfg` now counts the failures and says where the real answer is.

**The return values are unchanged.** Only what the log says.

## What this rules out

The addresses on that screen were **4, 5 and 6**, which looked like the
v0.8.6.1 slot leak returning. It is not. `x_attach()` reads
`out->addr` from slot context dword 3 — the address the **controller**
assigned — and an xHCI does not reclaim addresses until it is reset. A
rescan handing out 4, 5, 6 after a boot used 1, 2, 3 is expected.

## What is still open

**Why the control path degrades.** On a fresh boot with no phone
attached, `usb cfg` describes every device perfectly — confirmed at
10:18 the same morning. After the phone has been attached and the bus
rescanned, every descriptor read fails, including on devices no driver
has touched.

One boot now answers it:

```
usb cfg
dmesg | grep xHCI
```

If it is **USB Transaction Error**, the devices genuinely stopped
answering and the question is why a phone can do that to a webcam. If
it is **Context State Error**, an endpoint is being left in the wrong
state and the suspect is v0.8.7's Configure Endpoint drop path. If it
is **Event Ring Full**, events are arriving faster than they are
consumed and that is both dramatic and very fixable.

They are three different investigations and the log now says which.

---

## Testing

**40 suites, 2,861 checks**, unchanged — this release adds no behaviour
for a suite to cover, which is itself the point. 16 build checks pass.
