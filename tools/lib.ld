/* Single-hunk layout: everything in one code hunk, loaded at 0, relocated by the Amiga loader */
ENTRY(_start)
SECTIONS
{
  . = 0;
  .text : {
    *libhead.o(.text)
    *(.text .text.*)
    *(.rodata .rodata.*)
    *(.data .data.*)
    *(.bss .bss.* COMMON)
    . = ALIGN(4);
  }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu*) }
}
