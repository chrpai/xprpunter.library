#!/bin/sh
# Build xprpunter.library (needs gcc-m68k-linux-gnu, binutils-m68k-linux-gnu,
# python3 + pyelftools).  Output: dist/xprpunter.library
set -e
cd "$(dirname "$0")"
PY=${PYTHON:-python3}
CC=m68k-linux-gnu-gcc
LD=m68k-linux-gnu-ld
CFLAGS="-m68000 -Os -ffreestanding -fno-builtin -fno-pic -fno-pie -fomit-frame-pointer -fno-tree-loop-distribute-patterns -ffixed-a6 -Wall -Wextra -Wno-unused-parameter"
mkdir -p build dist
$CC $CFLAGS -c src/punter.c    -o build/punter.o
$CC $CFLAGS -c src/xprpunter.c -o build/xprpunter.o
$CC -m68000 -c src/libhead.S   -o build/libhead.o
$LD -z noexecstack -static -nostdlib --emit-relocs -T tools/lib.ld -o build/xprpunter.elf \
    build/libhead.o build/xprpunter.o build/punter.o
$PY tools/elf2hunk.py build/xprpunter.elf dist/xprpunter.library
