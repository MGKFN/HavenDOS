#pragma once
#include "types.h"

/* ════════════════════════════════════════════════════════════════════
   base64, once, in one place

   This file exists because the tree had FOUR implementations and two
   of its own comments complained about it:

     usb/lockdown.c   ld_b64(), a bounded decoder, whose comment says
                      "two base64 decoders in one tree is a smell"
     net/plist.c      an encoder with line wrapping, whose comment
                      says it is the second encoder and why
     shell/shell_v073.c
                      b64_encode_buf(), `static` in three thousand
                      lines of VGA drawing, so unreachable
     net/tls.c        (none -- it never needed one)

   Stage 3 needs the DECODER from net/, which is what finally forced
   the issue: the device's public key arrives as base64 inside a plist
   value, and inside that is PEM, which is base64 again. Copying the
   decoder a third time was the alternative to this file.

   ── WHAT THE DECODER ACCEPTS, AND WHY THAT IS NOT LAXNESS ───────────

   It skips whitespace, because every real base64 in the wild is
   wrapped. It REFUSES a character outside the alphabet, anything
   after the padding, more than two padding characters, and a value
   too large for the buffer -- refused, never truncated, because a
   decoder that silently shortens a key turns a corrupt key into a
   short one and a short key is a pairing that fails with no reason
   given.

   It ACCEPTS an unpadded value, and a trailing group of two or three
   characters. That is deliberate and it is usb/lockdown.c's rule
   carried over rather than a new one: two characters are twelve bits,
   which is one byte with four left over -- exactly what an unpadded
   single byte looks like. ONE character is the impossible case and is
   refused.

   The first draft of this file was stricter on both counts and broke
   four checks in tests/test_lockdown.c. Those checks exist because an
   earlier version of that suite had made the same mistake in the same
   direction; the comment there records it. Being strict about a thing
   base64 actually permits is not rigour.
   ════════════════════════════════════════════════════════════════════ */

/* Decode. Returns the number of bytes written, or -1 on any refusal.
   `out` is never written past `outcap`. A NULL `out` with outcap 0
   measures: it returns the length the decode WOULD produce, having
   validated the input, which is how a caller sizes a buffer without
   decoding twice. */
int b64_decode(const uint8_t *in, int inlen, uint8_t *out, int outcap);

/* Encode. Returns the number of characters written (never NUL
   terminated; the caller knows the length), or -1 if it will not fit.
   `wrap` is the column to break lines at, in characters, rounded down
   to a multiple of four so a quad is never split -- or 0 for one
   unbroken line. A trailing newline is NOT added.

   The exact space needed is b64_encoded_len(). */
int b64_encode(const uint8_t *in, int inlen, char *out, int outcap,
               int wrap);

/* How many characters b64_encode() will produce for `inlen` bytes at
   this wrap column, newlines included. */
int b64_encoded_len(int inlen, int wrap);
