# xprpunter.library

Punter C1 file transfer for Amiga BBS software that loads **XPR 2.0** external protocol libraries.
Written for **C-Net 5 Professional**, running under WinUAE and reached over telnet, with Commodore 64
callers using **CCGMS** and **Dialtone** on a C64 Ultimate. No C-Net source was available, so everything
here was worked out from the XPR 2.0 specification, the CCGMS Term source, and byte-level logs of real
transfers.

Every push is built and tested by GitHub Actions. Each push to `main` that passes is published as a
release containing `xprpunter.library` and a zip with the same file.

## Status

| Feature | CCGMS | Dialtone |
|---|---|---|
| Download from the BBS, single file | works | works |
| Upload to the BBS, single file | works | works |
| Download, multiple files (`M`) | works with the [chrpai/ccgmsterm](https://github.com/chrpai/ccgmsterm) `dev` branch; **fails** in older CCGMS (see below) | works |
| Upload, multiple files | not implemented | not implemented |

Verified runs: a 12 KB file and a 254 KB file (2,734 blocks, zero errors) downloaded to Dialtone in one
batch; single files both ways with CCGMS at 1 to 2 MHz and 9600 baud. Stock CCGMS retries blocks at 4 MHz
and above; the chrpai/ccgmsterm `dev` branch fixes that (see [Speed](#speed)).

## Speed

Measured with a 254 KB file at 64 MHz on a C64 Ultimate, using the chrpai/ccgmsterm `dev` branch over its
Ultimate Net modem (TCP straight to the BBS, no relay), with a block size of 255:

| Setup | CPS |
|---|---|
| Starting point | 1,895 |
| ccgmsterm PR #8 plus the patched `telnetd.device` | about 13,500 |
| Also ccgmsterm PR #9 | about 16,800 |
| For comparison: the `punterserv.py` PC server, same C64 | 10,046 at 16 MHz, 12,961 at 64 MHz |

What made the difference, on each side:

* **Block size 255** (`B255`, the default). The protocol maximum. A 12 KB file goes from 132 blocks at
  `B100` to about 50, so every per-block cost (handshakes, round trips, reads) is paid far fewer times.
  The library already defaulted to 255; the old `B100` in the setup steps was what held it back.
* **The patched `telnetd.device`** ([`telnetd-patch/`](telnetd-patch/)). It sets `TCP_NODELAY` on each
  accepted connection, and it replaces the telnet-mode send routine. The original split every write at
  each `0xFF` so it could double it, and every Punter block contains at least one `0xFF` (the next-block
  size byte), so a block went out as 3 to 4 TCP segments. The new routine doubles into a 512-byte buffer
  and calls `send()` once. An earlier build with only the `TCP_NODELAY` change
  (`telnetd-nodelay-only.device`) is the fallback if the full patch misbehaves.
* **ccgmsterm, on the `dev` branch of [chrpai/ccgmsterm](https://github.com/chrpai/ccgmsterm):**
  * PR #6: Punter timeouts count jiffies (1/60 s) instead of loop iterations, so they no longer get
    shorter as the CPU gets faster. This is what fixed the retries above 4 MHz.
  * PR #8: on Ultimate Net, no handshake pauses, no parked UCI reads, and the disk write overlaps the
    handshake.
  * PR #9: a whole 255-byte block fits in one UCI read.
  * The Multi-Punter header fix (below).

This library sends each block with one `xpr_swrite` call, and each 3-byte token with one more. It never
splits a write unless pacing (`D`) is on.

## Installing it in C-Net

These steps are what worked on a C-Net 5 Professional system. Menu wording may differ slightly between
C-Net versions, so treat the field names as a guide.

1. **Copy the library.** Put `xprpunter.library` in `LIBS:` on the Amiga (replace any older copy).
2. **Make the system drop the old copy.** Run `Avail FLUSH` in a Shell, or restart the node or emulator.
   An old copy that is still in memory is the most common reason a new build seems to change nothing.
3. **Add a protocol.** In C-Net's configuration, open **Config, Protocols** and add an entry like this:

   | Field | Value |
   |---|---|
   | Name | `Punter` |
   | Unique ID | `P` (any unused single letter) |
   | Library Name | `xprpunter.library` |
   | Environment | `TP,B255,W60` |
   | Batch | OFF |
   | Resumable | OFF |

   Save and use the configuration.
4. **Optional: a second entry for batch downloads** with the environment `TP,B255,W60,M` and Batch ON,
   under a different name and unique ID (for example `Multi-Punter`, `M`). Use it only for multi-file
   downloads to terminals that support them (Dialtone). Do not put `M` on the entry you use for
   single-file transfers. I did not verify how C-Net treats the Batch flag for this entry, so check that
   setting on your own system.
5. **Test with logging first.** Add `L` to the environment string (`TP,B255,W60,L`) for the first
   transfers and look at the log (below).

### Environment string

A comma-separated list, case-insensitive. Everything is optional.

| Option | Meaning |
|---|---|
| `TP` / `TS` / `TA` | File type announced to the terminal: PRG, SEQ, or automatic (SEQ for text-like extensions, otherwise PRG) |
| `Bnnn` | Punter block size in bytes, including the 7-byte header, 16 to 255. Default 255 (the protocol maximum). Smaller blocks only help on noisy links |
| `Wnn` | Seconds to wait for the receiver to start, 5 to 600. Default 60 |
| `L` | Write a diagnostic log (see below) |
| `M` | Multi-Punter batch framing on send |
| `M2` | Like `M` plus four NUL bytes after the tabs. Experimental; it did not fix the CCGMS problem |
| `Dn`, `Kn` | Output pacing: pause `n` ticks (1/50 s) after every `K` bytes. Only for marginal links; it hurts at turbo speeds |

A good starting string is `TP,B255,W60`.

### Diagnostic log

With `L`, each transfer writes `CNet:punterN.log` (or `RAM:punterN.log` if there is no `CNet:` assign).
`N` cycles 1 to 9 so the last nine transfers are kept. The log shows every byte sent and received with
timestamps in 1/50 s, the protocol tokens it saw, retries, and a final `RESULT` line. It also records what
C-Net gave the library: `CALL Send: xpr_ffirst=yes xpr_fnext=yes` and the file names.

## Terminals and the telnet problem

C-Net talks to callers through `telnetd.device`, which speaks **telnet**: it negotiates options at
connect, and it doubles every `0xFF` byte it sends (and expects the other side to double theirs). Punter
blocks contain arbitrary bytes, so any `0xFF` in a transfer is corrupted by a client that is not
telnet-aware.

* **Dialtone** is telnet-aware, so it connects directly.
* **The chrpai/ccgmsterm `dev` branch** has an **IAC Escape** option (F7 screen, key `I`) that does the
  telnet doubling itself, so with its Ultimate Net modem it dials the BBS directly with no relay.
* **Stock CCGMS through the C64 Ultimate's modem emulation** is not. The C64 Ultimate's modem layer passes
  raw bytes and its documentation does not mention telnet handling. For CCGMS, run the relay in
  [`relay/tnproxy.py`](relay/tnproxy.py) on the PC that hosts WinUAE. It answers the telnet negotiation,
  removes the doubling in both directions, and gives the C64 a clean 8-bit stream:

      python relay/tnproxy.py --listen 6801 --target 127.0.0.1:6800

  Then dial the relay's address and port from the C64. `-v` logs every chunk in hex.

`telnetd.device` is closed source (shareware; the author keeps the copyright), and as far as I could
tell it has no raw mode, which is why the relay exists. The binary patch in [`telnetd-patch/`](telnetd-patch/)
speeds it up (see [Speed](#speed)) but does not add a raw mode. Another way to get a raw line would be to use
WinUAE's serial-over-TCP port instead of `telnetd.device`; that was not tried.

## Multi-Punter (batch download)

`xpr_ffirst` and `xpr_fnext` are non-null on every call from C-Net, but C-Net calls the library's `Send`
**once**, with the first file only, and then considers the job finished. So a library has to walk the file
list itself. With `M`, the library enumerates the selected files with `xpr_ffirst` / `xpr_fnext` and, for
each file:

1. sends the CCGMS-style header: 16 TAB bytes, then `NAME,P` or `NAME,S`, then CR (names are upper-cased
   for PETSCII and cut to 16 characters);
2. waits for the terminal to answer, repeating the header every 5 seconds (the terminal may not be
   listening yet when the first header goes out);
3. runs a normal single-file Punter transfer;
4. pauses about two seconds.

After the last file it sends the end marker: 16 TAB, 16 Ctrl-D, CR.

This framing is the same one Dialtone accepted in testing. Multi-Punter variants differ between terminals,
so another program's batch mode may not be compatible.

### Why CCGMS Multi-Receive fails (fixed in chrpai/ccgmsterm)

CCGMS reads the header in a tight loop and, when no byte is waiting, `rs232_get` returns with the **carry
flag set** and the accumulator left unchanged. The multi-receive loop in `multixfer.s` never checks the
carry. Once it has stored the first filename character, the accumulator holds the character count, so
every empty poll appends a bogus "character" to the filename. After about 18 of them the check for the
comma before the file type fails and CCGMS leaves multi-receive. The sender can't fix this: the header
bytes have to arrive without gaps relative to the CPU's polling speed, which a 1 MHz CPU at 38400 baud
cannot do. The one-line symptom is the screen reading "Waiting for header..." and then dropping out to the
terminal, with the rest of the header showing up as plain text. The header loops ignored the carry flag and
stored the last byte again. The fix, checking the carry after each `jsr rs232_get` in `multixfer.s`, is on
the `dev` branch of [chrpai/ccgmsterm](https://github.com/chrpai/ccgmsterm). Older CCGMS builds still have
the bug.

## Punter, as implemented

* A block is a 7-byte header, then data: additive checksum (16 bits, little-endian), cyclic checksum
  (16 bits, little-endian; each byte is added and the sum rotated), the size of the next block, and the
  block number (16 bits). The last block has its number's high byte set (`FFxx`).
* Control tokens are the three-letter words `GOO`, `BAD`, `ACK` and the two-letter `S/B`, plus `SYN`.
* A transfer has two phases: first a one-byte file-type block (block number `FFFF`), then the data
  (block 0 carries a header block, then the data blocks).
* The end of a file is a handshake: `ACK`, `S/B` exchanged, `SYN`, `SYN`, then `S/B` sent again.
* **CCGMS file-type byte: 1 = PRG, 2 = SEQ (3 = USR).** Published descriptions give 0 for PRG and 1 for
  SEQ. CCGMS uses the byte directly in a drive command, so 0 puts a NUL into the file name and the drive
  answers `30,syntax error`. Use 1 and 2.

## Lessons learned

These cost the most time, in roughly the order they bit.

* **Get the log first.** The byte-level log (`L`) answered nearly every question and settled several
  arguments about whose fault something was. Build it in before anything else.
* **The type byte** (above) looked like a handshake problem for a long time: CCGMS printed a drive error,
  aborted, and then displayed the rest of the stream as garbage like `s/bs/bs/b`.
* **The end-of-file handshake is timing-sensitive.** An early version discarded the receiver's `GOO`
  while waiting out a one-second gap; the receiver's reply was thrown away and the transfer hung. Keep
  gaps short, listen for `GOO` during them, and ignore a repeated `S/B` (CCGMS sends it twice).
* **CCGMS timeouts are counted in loop iterations, not time**, so they get shorter as the CPU gets
  faster. At 1 to 2 MHz and 9600 baud it is clean. At 4 MHz it starts to lose blocks; at 32 MHz it
  completes but about one block in two needs a retry. Slowing the sender with pacing made turbo-speed runs
  worse, not better, because CCGMS was already impatient. ccgmsterm PR #6 fixed this by counting jiffies.
* **Count TCP segments, not just bytes.** Over telnet, how a block is cut into packets mattered more than
  the line speed. `telnetd.device` split each block at every `0xFF`, which was invisible in the byte log.
* **Retries can look like failure.** A 131-block file with 114 retries still arrives correct. Count
  `RESULT` lines, not warnings.
* **An old library stays in memory.** If a change appears to have no effect, run `Avail FLUSH`.
* **Don't guess the library calling convention.** The XPR 2.0 documentation (in the `xpr_2_0` archive on
  Aminet) gives register assignments for the callbacks, `xpr_ffirst(buffer, pattern)` and
  `xpr_fnext(oldstate, buffer, pattern)`. A wrong guess in a library that runs inside the BBS can take the
  whole node down.
* **Terminals differ even where they claim the same protocol.** Dialtone and CCGMS both say "Multi-Punter"
  and both wait for a header, but only one of them reads it correctly in practice.
* **A clean build environment is part of the test.** The library links no C runtime, so 32-bit
  multiplication, division and modulo (which pull in `libgcc`) fail at link time. A harmless-looking
  `x % 9` broke a build.

## Building

Needs `gcc-m68k-linux-gnu`, `binutils-m68k-linux-gnu` and Python 3 with `pyelftools`:

    ./build.sh                  # produces dist/xprpunter.library
    ./test/run_native.sh        # protocol engine against a reference peer
    ./test/run_vamos.sh         # the real library under vamos (pip install amitools)

`vamos` exits non-zero on its own shutdown warnings even when every test passed, so the CI job judges the
run by the `HARNESS: ALL PASSED` line instead of the exit status.

### Layout

    src/punter.c      protocol engine; no operating-system dependencies
    src/xprpunter.c   XPR glue: callbacks, options, logging, the batch loop
    src/libhead.S     library header, vector table, version string
    tools/            linker scripts and the ELF-to-Amiga-hunk converter
    test/             native and vamos test harnesses
    relay/tnproxy.py  raw-to-telnet relay for terminals that are not telnet-aware
    telnetd-patch/    TCP_NODELAY and single-send patch for telnetd.device v1.91 (script and tests only)

## Troubleshooting

| What you see | Likely cause |
|---|---|
| Log ends with `No response from receiver` and no `RX` lines | The terminal never started its receive, or its receive aborted. Start the terminal's download after the BBS says to |
| CCGMS shows `30,syntax error` | Wrong file-type byte. Use 1 for PRG and 2 for SEQ; this library already does |
| Many `BAD` replies, transfer slow | CCGMS at a fast CPU speed, or a lossy line. Try 1 to 2 MHz and 9600 baud, and no pacing |
| Corrupt files only when they contain `FF` bytes | Telnet doubling reached a non-telnet terminal. Use the relay |
| New library has no effect | The old one is still in memory. `Avail FLUSH` and restart the node |
| Batch download leaves CCGMS on "Waiting for header" | The CCGMS bug above. Use the chrpai/ccgmsterm `dev` build, or Dialtone |
| Log shows `blocksize=100` | The Environment string still has `B100`. Use `B255` or leave `B` out |

## Credits and license

Punter C1 behaviour was worked out from public descriptions and from reading the CCGMS Term source
(BSD-3-Clause, https://github.com/mist64/ccgmsterm) for reference; no CCGMS code is included. The XPR 2.0
interface is described in the Aminet `xpr_2_0` archive. Released under the MIT License; see `LICENSE`.
