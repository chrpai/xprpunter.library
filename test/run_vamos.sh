#!/bin/sh
# Builds the m68k harness and runs it under vamos with the real hunk library.
set -e
cd "$(dirname "$0")/.."
PY=${PYTHON:-python3}; VAMOS=${VAMOS:-vamos}
CFLAGS="-m68000 -Os -ffreestanding -fno-builtin -fno-pic -fno-pie -fomit-frame-pointer -fno-tree-loop-distribute-patterns -fno-delete-null-pointer-checks -ffixed-a6 -Wall -Wno-unused-parameter"
sh ./build.sh >/dev/null 2>&1
m68k-linux-gnu-gcc $CFLAGS -c test/amiga_test.c -o build/amiga_test.o
m68k-linux-gnu-gcc -m68000 -c test/amiga_stubs.S -o build/amiga_stubs.o
m68k-linux-gnu-ld -z noexecstack -static -nostdlib --emit-relocs -T tools/exe.ld -o build/amiga_test.elf build/amiga_stubs.o build/amiga_test.o
$PY tools/elf2hunk.py build/amiga_test.elf build/ptest
rm -rf /tmp/vamos_libs && mkdir -p /tmp/vamos_libs/libs && cp dist/xprpunter.library /tmp/vamos_libs/libs/
rm -rf /tmp/vamos_ram; mkdir -p /tmp/vamos_ram
$VAMOS -V pt:/tmp/vamos_libs -V ram:/tmp/vamos_ram -a libs:pt:libs "$@" build/ptest
