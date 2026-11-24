/* Register-arg -> C-ABI stubs for the XPR callbacks the harness hands to the
 * library, plus the program entry. */
        .text
        .globl  _start
_start:
        jbsr    harness_main
        rts

        .globl  stub_fopen, stub_fclose, stub_fread, stub_fwrite
        .globl  stub_sread, stub_swrite, stub_sflush, stub_update
        .globl  stub_chkabort, stub_finfo
        .extern c_fopen, c_fclose, c_fread, c_fwrite, c_sread, c_swrite
        .extern c_sflush, c_update, c_chkabort, c_finfo

stub_fopen:     /* a0 name, a1 mode */
        move.l  %a1,-(%sp)
        move.l  %a0,-(%sp)
        jbsr    c_fopen
        addq.l  #8,%sp
        rts
stub_fclose:    /* a0 handle */
        move.l  %a0,-(%sp)
        jbsr    c_fclose
        addq.l  #4,%sp
        rts
stub_fread:     /* a0 buf, d0 size, d1 count, a1 handle */
        move.l  %a1,-(%sp)
        move.l  %d1,-(%sp)
        move.l  %d0,-(%sp)
        move.l  %a0,-(%sp)
        jbsr    c_fread
        lea     16(%sp),%sp
        rts
stub_fwrite:
        move.l  %a1,-(%sp)
        move.l  %d1,-(%sp)
        move.l  %d0,-(%sp)
        move.l  %a0,-(%sp)
        jbsr    c_fwrite
        lea     16(%sp),%sp
        rts
stub_sread:     /* a0 buf, d0 size, d1 timeout */
        move.l  %d1,-(%sp)
        move.l  %d0,-(%sp)
        move.l  %a0,-(%sp)
        jbsr    c_sread
        lea     12(%sp),%sp
        rts
stub_swrite:    /* a0 buf, d0 size */
        move.l  %d0,-(%sp)
        move.l  %a0,-(%sp)
        jbsr    c_swrite
        addq.l  #8,%sp
        rts
stub_sflush:
        jbsr    c_sflush
        rts
stub_update:    /* a0 struct */
        move.l  %a0,-(%sp)
        jbsr    c_update
        addq.l  #4,%sp
        rts
stub_chkabort:
        jbsr    c_chkabort
        rts
stub_finfo:     /* a0 name, d0 type */
        move.l  %d0,-(%sp)
        move.l  %a0,-(%sp)
        jbsr    c_finfo
        addq.l  #8,%sp
        rts
