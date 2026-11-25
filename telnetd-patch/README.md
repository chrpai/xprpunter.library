# telnetd.device v1.91 TCP_NODELAY and single-send patch

This folder holds the patch script and its tests only. The `telnetd.device` binaries are not included,
because telnetd.device is closed-source shareware. Run the script on your own copy of v1.91.

`telnetd.device` here is AmiTCP-68020 telnetd.device v1.91 (23.11.94), patched so every
accepted connection gets `setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &1, 4)`, and telnet-mode
writes are sent with one `send()` per write instead of one per 0xFF byte.
`telnetd-nodelay-only.device` is the first version (TCP_NODELAY only, 30,068 bytes).

- Original: 30,044 bytes, sha256 `fa033572d3976b85116f2bceb2e0c0fcfbc6cb72872ddb11682daf892285872f`
- Patched: 30,168 bytes, sha256 `0cf9879c03dd8d002f61cf8d0d34569dbc2d96b5fe21cd8bbf9bada78b6d0327`
- Rebuild: `python3 patch_telnetd_nodelay.py telnetd.device.orig telnetd.device`
  (the script checks the size and the exact bytes it changes, and refuses anything else)

## What changed

Offsets are relative to hunk 2 (main code, file offset 0x314).

1. After `accept()` (`jsr -48(a6)` at 0x1dce, socket in d7), the original code at
   0x1f76 sets `SO_KEEPALIVE` on the new socket with `jsr -90(a6)` (setsockopt) at 0x1f8a.
   That one call is changed to `jsr cave(pc)`.
2. A 24-byte cave is appended to the end of hunk 2 (0x7108). It makes the original
   SO_KEEPALIVE call, then `setsockopt(d7, 6, 1, &one, 4)` using the same bsdsocket base
   in a6, and puts d2 back to 8. d3 stays 4, which matters: the code after it reuses d3 as
   the length argument to `gethostbyaddr()`. The `&one` is the existing constant 1 at
   0x0c66 that the original already uses for its setsockopt calls.
3. **Single send.** In telnet mode the device's send hook (installed at 0x1486, original
   at 0x3060) doubles 0xFF for telnet by calling `send()` up to and including each 0xFF,
   then starting the next `send()` at that same 0xFF. A write with k 0xFF bytes becomes
   k+1 sends. Every Punter block has 0xFF as its size byte at offset 4, plus about 1.2
   more in compressed data, so a 255-byte block went out as about 3 to 4 sends, which
   with TCP_NODELAY means 3 to 4 TCP segments (the first only 5 bytes). The hook now points
   to a 100-byte replacement at 0x7120 with the same register and stack contract. It copies
   the data into a 512-byte stack buffer, doubling 0xFF, and calls `send()` once per
   buffer, so writes up to about 500 bytes go out in one send(). The byte counter it
   updates gets the same total. The rlogin modes use a different hook and are unchanged.
4. Hunk 2's length goes from 0x1c42 to 0x1c61 longwords in the header table and in the
   HUNK_CODE header. The cave is position-independent, so the reloc table is unchanged.

Only connections that pass the access/blacklist checks reach this code; rejected ones are
closed without it, as before. The listening socket is not touched.

## PACERATE

`PACERATE` ends up at offset $32 in the device base (default -1 when not given).
When a unit opens: -1 = no pacing (0x7fffffff chars/s), 0 = pace at the baud rate the
client sets with SDCMD_SETPARAMS (min 300, default 19200) / 10, any other value = that
value / 10 chars/s. PACERATE is a `/K` keyword in the template
`PORTNUMBER/N/A,NUMUNITS/N/A,ACCESS/M/A,PACERATE/K/N,...`, so in the conf
`6800 10 19200 255.255.255.0` the 19200 is taken as an ACCESS entry, not a pace rate, and
pacing is off. To set it you would need `PACERATE=19200` (or `PACERATE 19200`).

## Write path, for reference

CMD_WRITE (around 0x8c2) sends the whole request in one call to the send hook when
connected. It only cuts it smaller when pacing is on (PACERATE set): then each piece is
limited to the per-tick budget. Nothing else in telnetd buffers or chunks writes, so any
remaining split before the 0xFF fix comes from the caller (C-Net's swrite), not telnetd.

## Testing

- Done: disassembled before and after (capstone), checked the patched file parses as a
  valid 3-hunk LoadSeg executable (amitools `hunktool`), and ran the real relocated code
  from 0x1f76 to the `gethostbyaddr()` call on a 68020 emulator core (machine68k/Musashi)
  with a stub bsdsocket base. `emutest.py` shows the original makes one setsockopt call;
  the patched one makes the same call plus `setsockopt(5, 6, 1, &1, 4)`, and d2, d3, a6
  and the stack are identical afterwards.
- `emutest_iac.py` runs the original and the new send hook on the same core for 9 cases
  (empty, single 0xFF, all-0xFF 300 bytes, a Punter-like block, 1,100 random bytes,
  buffer-boundary case). The byte stream and counter match the original exactly, saved
  registers and the stack are intact, and a 255-byte block with 3 0xFF goes from 4 sends to 1.
- On the real BBS (C-Net under WinUAE), the patched device plus ccgmsterm PR #8 took a 254 KB
  Punter download at 64 MHz from 1,895 to about 13,500 CPS. See the main README's Speed section.
