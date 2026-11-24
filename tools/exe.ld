ENTRY(_start)
SECTIONS { . = 0;
  .text : { *amiga_stubs.o(.text) *(.text .text.*) *(.rodata .rodata.*) *(.data .data.*) *(.bss .bss.* COMMON) . = ALIGN(4); }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu*) } }
