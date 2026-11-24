#!/usr/bin/env python3
"""elf2hunk.py - convert a statically linked single-section m68k ELF (linked at
address 0 with --emit-relocs) into an AmigaOS hunk executable with one
HUNK_CODE hunk + HUNK_RELOC32.  Only what xprpunter needs, nothing more."""
import struct, sys
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

R_68K_32, R_68K_PC32, R_68K_PC16, R_68K_PC8, R_68K_NONE = 1, 4, 5, 6, 0

def convert(src, dst):
    with open(src, 'rb') as f:
        elf = ELFFile(f)
        if elf['e_machine'] != 'EM_68K':
            raise SystemExit('not an m68k ELF')
        text = elf.get_section_by_name('.text')
        if text['sh_addr'] != 0:
            raise SystemExit('.text must be linked at 0')
        data = bytearray(text.data())
        # any other allocated section would be silently dropped: refuse
        for s in elf.iter_sections():
            if (s['sh_flags'] & 2) and s.name != '.text' and s['sh_size']:
                raise SystemExit('unexpected allocated section %s' % s.name)
        relocs = []
        for s in elf.iter_sections():
            if not isinstance(s, RelocationSection):
                continue
            tgt = elf.get_section(s['sh_info'])
            if tgt.name != '.text':
                continue
            for r in s.iter_relocations():
                t = r['r_info_type']
                off = r['r_offset']
                if t == R_68K_32:
                    relocs.append(off)
                elif t in (R_68K_PC32, R_68K_PC16, R_68K_PC8, R_68K_NONE):
                    pass            # same hunk, already resolved by the linker
                else:
                    raise SystemExit('unsupported relocation type %d at %#x' % (t, off))
        relocs = sorted(set(relocs))
    while len(data) % 4:
        data.append(0)
    nlong = len(data) // 4
    out = bytearray()
    out += struct.pack('>IIIII', 0x3F3, 0, 1, 0, 0)       # HUNK_HEADER, 1 hunk
    out += struct.pack('>I', nlong)                        # size, MEMF_ANY
    out += struct.pack('>II', 0x3E9, nlong) + bytes(data)  # HUNK_CODE
    if relocs:
        out += struct.pack('>III', 0x3EC, len(relocs), 0)  # RELOC32 -> hunk 0
        out += b''.join(struct.pack('>I', o) for o in relocs)
        out += struct.pack('>I', 0)
    out += struct.pack('>I', 0x3F2)                        # HUNK_END
    with open(dst, 'wb') as f:
        f.write(out)
    print('%s: %d bytes code, %d relocs -> %s (%d bytes)' %
          (src, len(data), len(relocs), dst, len(out)))

if __name__ == '__main__':
    convert(sys.argv[1], sys.argv[2])
