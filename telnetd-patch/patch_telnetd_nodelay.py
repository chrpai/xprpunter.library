#!/usr/bin/env python3
"""
Patch AmiTCP telnetd.device v1.91 (23.11.94, 68020 build, 30044 bytes) so that
every accepted connection also gets setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &1, 4),
and so telnet-mode writes go out in one send() instead of one per 0xFF byte.

Usage: python3 patch_telnetd_nodelay.py telnetd.device telnetd.device.nodelay

What it does (offsets are relative to the start of hunk 2, the main CODE hunk):
  0x1f76..0x1f8a  original: setsockopt(d7, SOL_SOCKET, SO_KEEPALIVE, &one, 4)
                  on the socket returned by accept() at 0x1dce.
  0x1f8a          jsr -90(a6)  ->  jsr cave(pc)
  0x7108 (new)    cave, appended to the end of hunk 2 (24 bytes):
                    jsr   -90(a6)        ; original SO_KEEPALIVE call
                    move.l d7,d0         ; s
                    moveq #6,d1          ; IPPROTO_TCP
                    moveq #1,d2          ; TCP_NODELAY
                    moveq #4,d3          ; optlen (caller later relies on d3 == 4)
                    lea   one(pc),a0     ; existing constant 1 at 0x0c66
                    jsr   -90(a6)        ; setsockopt
                    moveq #8,d2          ; restore d2 as the original left it
                    rts
  0x1486          lea iacsend(pc),a0 (was lea $3060(pc),a0): telnet mode's send hook
  0x7120 (new)    iacsend, 100 bytes: same contract as the original at 0x3060
                  (d0 socket, d1 length, a1 data, a0 bsdsocket base, 4(sp) pointer
                  to the byte counter), but it doubles 0xFF into a 512-byte stack
                  buffer and calls send() once per buffer instead of once per 0xFF.
Hunk 2 grows from 0x1c42 to 0x1c61 longwords (header size table + HUNK_CODE
length). The cave has no absolute references, so the reloc table is unchanged.
"""
import struct, sys, hashlib

ORIG_SIZE = 30044
H2_DATA = 0x314          # file offset of hunk 2 code
H2_LONGS = 0x1c42
CALL = 0x1f8a
ONE = 0x0c66
IACHOOK = 0x1486

def be32(b, o): return struct.unpack_from('>I', b, o)[0]

def main(src, dst):
    d = bytearray(open(src, 'rb').read())
    if len(d) != ORIG_SIZE: sys.exit("unexpected size %d, expected %d" % (len(d), ORIG_SIZE))
    # sanity checks on the layout this patch assumes
    assert be32(d, 0) == 0x3f3
    assert be32(d, 0x1c) & 0x3fffffff == H2_LONGS, "header size table"
    assert be32(d, 0x30c) == 0x3e9 and be32(d, 0x310) == H2_LONGS, "hunk 2 header"
    cave = H2_LONGS * 4
    assert be32(d, H2_DATA + cave) == 0x3ec, "reloc table should follow hunk 2 code"
    expect = bytes.fromhex('2007223c0000ffff7408760441faece22c6d001a4eaeffa62006')
    assert d[H2_DATA+0x1f76:H2_DATA+0x1f90] == expect, "SO_KEEPALIVE sequence not found"
    assert be32(d, H2_DATA + ONE) == 1, "constant 1 not found"

    # cave code
    lea_ext = cave + 0x0e                     # PC value used by lea d16(pc)
    code = bytes.fromhex('4eaeffa6' '2007' '7206' '7401' '7604') \
         + b'\x41\xfa' + struct.pack('>h', ONE - lea_ext) \
         + bytes.fromhex('4eaeffa6' '7408' '4e75')
    assert len(code) % 4 == 0
    iac = 0x7120
    assert cave + len(code) == iac
    assert d[H2_DATA+IACHOOK:H2_DATA+IACHOOK+4] == bytes.fromhex('41fa1bd8'), "telnet send hook not found"
    code += bytes.fromhex(
        '48e73f3e' '4feffe00' '2e00' '2c01' '2a48' '2649' '244f' '7a00'   # 00 save, buf, setup
        '4a86' '671e' '5386' '101b' '14c0' '5285' 'b03c00ff' '6604'       # 14 loop
        '14c0' '5285'                                                    # 26 double 0xFF
        '0c85000001fe' '6de2' '6112' '60de'                              # 2a buffer full?
        '4a85' '6702' '610a'                                             # 36 done: flush rest
        '4fef0200' '4cdf7cfc' '4e75'                                     # 3c restore, rts
        '2007' '2205' '7400' '41ef0004' '2c4d' '4eaeffbe'                # 46 flush: send()
        '206f0234' 'db90' '45ef0004' '7a00' '4e75')                      # 56 count, reset
    assert len(code) == 0x18 + 0x64
    d[H2_DATA+IACHOOK:H2_DATA+IACHOOK+4] = b'\x41\xfa' + struct.pack('>h', iac - (IACHOOK + 2))
    # redirect the call: jsr d16(pc) = 4eba disp, disp relative to CALL+2
    d[H2_DATA+CALL:H2_DATA+CALL+4] = b'\x4e\xba' + struct.pack('>h', cave - (CALL + 2))
    # grow hunk 2
    newlongs = H2_LONGS + len(code)//4
    d[0x1c:0x20] = struct.pack('>I', (be32(d, 0x1c) & 0xc0000000) | newlongs)
    d[0x310:0x314] = struct.pack('>I', newlongs)
    d[H2_DATA+cave:H2_DATA+cave] = code
    open(dst, 'wb').write(d)
    print("wrote %s (%d bytes) sha256 %s" % (dst, len(d), hashlib.sha256(d).hexdigest()))

if __name__ == '__main__':
    if len(sys.argv) != 3: sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
