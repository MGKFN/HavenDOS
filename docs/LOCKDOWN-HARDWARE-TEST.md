# LOCKDOWND STAGES 1 AND 2 — the five minutes that settle it

**v0.8.7.7. Read this, not HANDOFF.md, when you sit down at the laptop.**

Every bug in 0.8.7.1 through 0.8.7.5 was found by this laptop and none
by QEMU. Four for four, with the suites green the whole time. This
release adds 375 checks and two injection harnesses and **none of them
can tell you whether lockdownd answers**, because QEMU has no iPhone.

One boot answers it.

---

## Before you start

- iPhone **unlocked**.
- Plugged in **before** you boot. Nothing in this kernel runs on a
  timer, so a device plugged in afterwards needs `usb rescan`.
- Boot **installed**, not live — either works for this test, but a live
  shell cannot see the HavenDOS partition and you may want `update`
  afterwards.
- **Personal Hotspot does not need to be on.** That is stage 4. If the
  phone has never trusted this machine it will not let you turn it on
  anyway, which is the whole reason this release exists.

If the phone shows a **"Trust this computer?"** prompt, note whether it
appears and whether tapping Trust makes any difference. It should not,
yet — HavenDOS has no pairing record to offer, which is stage 3. The
fact of the prompt is data.

---

## The test, in order

### 1. Is the channel still there

```
usb mux
```

This is stage 0 and it worked on 26 October. Expected, unchanged:

```
usbmux: 05AC:12A8 cfg 3 if 1 is Apple Mobile Device, with Ethernet
usbmux: open, ep 85 in / 04 out, 512/512 bytes
usbmux: THE DEVICE ANSWERED -- version 2
usbmux: SETUP sent
```

**NEW IN THIS RELEASE, and worth reading:** the dump now prints the mux
sequence pair.

```
mux seq: tx 1  rx FFFF
```

**Those two numbers are the thing 0.8.7.6 had wrong.** It sent `tx 1
rx 1`. usbmuxd resets the pair on the SETUP packet — tx to 0, rx to
0xFFFF — and 0xFFFF means "nothing received from you yet", not 65535.
After a SETUP and nothing else, `tx 1 rx FFFF` is correct. If you see
`tx 1 rx 1` you are running an old kernel.

**If `usb mux` does not get this far, stop.** Nothing below it can
work, and the fault is in stage 0, which the previous release proved.

### 2. THE TEST

```
usb lockdown
```

**STAGE 1 CLOSED ON THE HP, 31 OCTOBER, v0.8.7.8.**

```
ok  lockdown: LOCKDOWND ANSWERED -- com.apple.mobile.lockdown
    lockdown  18516->62078  seq 286 ack 302  their window 131072
            requests 1  replies 1
            reply: 301 bytes in 1 packets
            length prefix: 00 00 01 29 = 297
```

So stage 1 is not what this command is testing any more. **v0.8.7.9
runs stage 2 straight after it, on the same connection**: GetValue for
DevicePublicKey. Still no cryptography.

One command now prints both.

### 3. What the screen means

There are five outcomes and they are **not the same fact**. This is the
whole reason the command prints what it prints.

---

#### (a) `LOCKDOWND ANSWERED.` then `GOT THE DEVICE KEY.`

Both stages. Stage 1 is settled, so the second half is the new part:

```
ok  lockdown: GOT DevicePublicKey -- 426 bytes
    lockdown  18516->62078  seq 571 ack 1203  their window 131072
            requests 2  replies 2
            reply: 902 bytes in 1 packets
            length prefix: 00 00 03 7A = 890
ok          value: 426 bytes
ok          -----BEGIN RSA PUBLIC KEY-----
```

**The first line of the value is the test.** It should read
`-----BEGIN RSA PUBLIC KEY-----`. The byte count beside it matters as
much: a key around four hundred bytes is right, and **anything near
sixty is base64 that stopped at the end of the first line** — which is
the specific way this can fail, because a `<data>` element is wrapped
across lines with tabs inside it.

If that line is a key, **stage 2 is closed and the road is clear to
stage 3**, which is the pair request and where RSA and X.509 arrive.

An `Error` here naming pairing or trust is the one answer that would
mean stage 2 needs stage 3 — i.e. the phone will not hand over its
public key to a host it has no record of. Write down the exact string.

#### (a2) stage 1 answers and stage 2 does not

```
lockdown: connected to port 62078
lockdown: QueryType sent, 254 bytes
lockdown: LOCKDOWND ANSWERED -- com.apple.mobile.lockdown
lockdown  17492->62078  seq 258 ack 251  their window 131072
        requests 1  replies 1
        reply: 247 bytes in 2 packets
        length prefix: 00 00 00 F3 = 243
        <?xml version="1.0" encoding="UTF-8"?> <!DOCTYPE plist PUBLIC
        Type:  com.apple.mobile.lockdown
```

Stage 1 answered and stage 2 did not. Same connection, same framing,
so **the reply is the difference**: GetValue returns `<data>`, not
`<string>`, and it is base64 wrapped across lines.

`dmesg | grep lockdown` distinguishes the three:

| line | what it means |
|---|---|
| `the DevicePublicKey value is N bytes of base64 this could not decode` | the value region is not what the driver thinks it is |
| `a reply to GetValue ... with no Value in it` | the reply parsed and had no Value |
| `GetValue DevicePublicKey refused -- <Error>` | lockdownd said no, and the string says why |

`reply: N bytes in 2 packets` is the interesting line: it confirms the
reply really does arrive as a length and then a body, which is what
libimobiledevice does and what the accumulation in `usb/lockdown.c`
exists for. **If it says `1 packet`, say so** — it means the device
sends it whole and a reader written for one packet would have worked.
That is worth knowing and changes nothing.

---

#### (b) `No Apple Mobile Device interface on the bus.`

The phone is not enumerated at all. `usb -vv` and then `usb cfg`. This
is not a stage-1 result.

---

#### (c) `The device REFUSED the port.`

```
lockdown: port 62078 is CLOSED on the device -- the channel works,
          lockdownd is not listening
```

The mux works and something answered with an RST. **An iPhone always
listens on 62078**, so this says the connection reached something that
is not lockdownd — most likely the port number or the TCP header is
wrong, not the transport.

This is a *good* failure: it means SYN went out and a reply came back,
so the framing of the mux TCP header is close enough for the device to
parse it and object.

---

#### (d) `The exchange failed.` — the connection opened

The transport and the three-way handshake both worked and the plist
framing is what is left. **`dmesg | grep lockdown` names which part**,
and these are the four it can be:

| line | what it means |
|---|---|
| `no reply at all` | the request went out and nothing came back |
| `N bytes arrived, fewer than the four-byte length prefix` | something came back, but not even a length |
| `N of M bytes arrived -- the reply stopped part way` | it started and stopped |
| `the reply says N bytes, which is more than this can hold` | the reply is over 4092 bytes |

**`no reply at all` is the one to expect if pairing is required to get
even this far**, and it is the result that would mean stage 1 cannot be
done without stage 3 — which would be bad news and worth knowing
immediately.

Before concluding that, check the dump's raw line. If the length prefix
is there but the body never came, that is a different fault from
silence.

---

#### (e) `It answered, but not with the lockdown type.`

Every byte of framing worked. Three sub-cases, and the screen says
which:

- **the reply carried an `Error`** — lockdownd refused the request
  itself. The Error string is printed. `InvalidRequest` means the plist
  is wrong; anything mentioning pairing or trust means **stage 1 needs
  stage 3** and that is the answer this boot was for.
- **a `Type` this kernel did not expect** — print it and write it down.
- **no `Type` in it** — the raw line is what arrived. If it is not XML,
  the length prefix is the suspect.

---

## 4. Then, whatever happened

```
dmesg | grep -e usbmux -e lockdown
```

Photograph it. Every refusal in both files is logged by name on purpose:
each one has a different cause and a different fix, and the single most
expensive thing this project has done repeatedly is read one failure as
another.

---

## The one thing most likely to be wrong now

Both of the old suspects are settled. The length prefix does **not**
count itself — the HP printed `00 00 01 29 = 297` against a 301-byte
reply — and the device sends the length and the body **together**, in
one packet, not as the two writes libimobiledevice uses. (The reader
accumulates either way; that case is tested as the deliberate
contrast.)

What is left is **the base64 in a `<data>` element**, and it has one
failure mode worth naming in advance:

**A `<data>` element is wrapped across lines with a tab on each.** A
decoder that stops at the first character outside the alphabet returns
the first line and nothing else — about a sixth of the key, still
beginning `-----BEGIN RSA PUBLIC KEY-----`, and therefore still looking
like a key on screen. **That is why the byte count is printed next to
the first line.** Four hundred-odd bytes is right. Sixty is that bug.

(There is a second base64 decoder in this tree, in
`shell/shell_v073.c`, and it is exactly the one that would do this. It
is `static` and NUL-terminated and could not be used here. The two
should be merged when something else needs one; it is in HANDOFF.md.)

## Already confirmed on the HP, 30–31 October

Do not re-derive these; they are settled.

- **The mux sequence rules.** `mux seq: tx 2 rx FFFF` after a SYN is
  correct — the SETUP reset the pair and sent `tx 0`, the SYN carried
  `tx 1`. 0.8.7.6 would have shown `tx 1 rx 1`.
- **The source port.** `SYN -> port 62078, from 18516`; 18516 is
  0x4854, big-endian, correct.
- **The TCP header.** The phone answered it, so its offset nibble,
  flags, window and port pair were all acceptable to iOS.
- **The mirroring.** The device's `rx_seq` came back as 1, which is our
  SYN's `tx_seq`.
- **`errs 0`.** The event ring, the buffer sizes and the configuration
  choice are all holding.
- **The whole of stage 1**, 31 October: the completing ACK, the plist
  request, the reply, and `com.apple.mobile.lockdown` coming back.
  `seq 286 ack 302` is 1+285 and 1+301 — the ack arithmetic and the
  ISN+1 choice are both right. `their window 131072` means the `<<8`
  shift is right too.

## What this release also fixed, which you cannot see on screen

Three things in the mux framing that 0.8.7.6 had wrong and that nothing
had exercised, because the only sequenced packet it ever sent was one
SETUP on which the wrong answers happened to be equal:

- the SETUP packet resets the sequence pair; it does not continue it
- `tx_seq` advances only on a packet that carries the field
- `rx_seq` is the device's own value mirrored back, not a count of
  arrivals

And one real bug the injection harness found while establishing why a
fourth could not be caught: **a SETUP that failed to send was never
sent again**, so the channel sat claiming a version the device had
never agreed to. `usb mux` twice in a row is the path that hit it.

---

## If it works

Both stages are closed and the road is clear to **stage 3: the pair
request**, which is where RSA key generation and X.509 certificates
arrive and which is much the hardest of the four.

What stage 3 needs that does not exist yet:

- a **plist writer** — the pair request carries certificates and is
  several kilobytes, which is more than one mux packet and more than
  `LD_BUF`. `ld_send_plist()` refuses it by name today rather than
  half-sending it.
- **RSA key generation**, a **root certificate**, a **host
  certificate**, and a **device certificate** signed by the root.
- a second base64 decode, to get the DER out of the PEM this stage
  returns.

So stage 3 is not one more request. Say so before starting it.
