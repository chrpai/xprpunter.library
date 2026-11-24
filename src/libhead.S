/*
 * libhead.S - AmigaOS shared-library skeleton for xprpunter.library
 *
 * Layout of our library base (struct Library = 34 bytes, then ours):
 *    0 ln_Succ   4 ln_Pred   8 ln_Type   9 ln_Pri  10 ln_Name
 *   14 lib_Flags 16 lib_NegSize 18 lib_PosSize 20 lib_Version 22 lib_Revision
 *   24 lib_IdString 28 lib_Sum 32 lib_OpenCnt
 *   34 seglist (BPTR)   38 SysBase
 */
        .text
        .even
        .globl  _start
_start:                                 /* if someone runs us as a program */
        moveq   #-1,%d0
        rts

        .even
RomTag:
        .word   0x4AFC                  /* RTC_MATCHWORD */
        .long   RomTag                  /* rt_MatchTag   */
        .long   EndOfCode               /* rt_EndSkip    */
        .byte   0x80                    /* RTF_AUTOINIT  */
        .byte   1                       /* rt_Version    */
        .byte   9                       /* NT_LIBRARY    */
        .byte   0                       /* rt_Pri        */
        .long   LibName
        .long   LibId
        .long   InitTable

InitTable:
        .long   42                      /* data size: 34 + seglist + SysBase */
        .long   FuncTable
        .long   0                       /* no InitStruct table */
        .long   LibInit

FuncTable:
        .long   LibOpen
        .long   LibClose
        .long   LibExpunge
        .long   LibNull
        .long   XProtocolCleanup        /* -30 */
        .long   XProtocolSetup          /* -36 */
        .long   XProtocolSend           /* -42 */
        .long   XProtocolReceive        /* -48 */
        .long   XProtocolHostMon        /* -54 */
        .long   XProtocolUserMon        /* -60 */
        .long   -1

LibName: .asciz "xprpunter.library"
LibId:   .ascii "$VER: xprpunter.library 1.15 (23.11.2026) Punter C1 XPR protocol\r\n"
         .byte  0
        .even

/* d0 = library base, a0 = seglist, a6 = SysBase  -> d0 = base */
LibInit:
        move.l  %d0,%a1
        move.l  %a0,34(%a1)
        move.l  %a6,38(%a1)
        move.b  #9,8(%a1)               /* ln_Type = NT_LIBRARY */
        move.b  #3,14(%a1)              /* LIBF_CHANGED|LIBF_SUMUSED */
        move.w  #1,20(%a1)              /* version  */
        move.w  #15,22(%a1)              /* revision */
        lea     LibName(%pc),%a0
        move.l  %a0,10(%a1)
        lea     LibId(%pc),%a0
        move.l  %a0,24(%a1)
        rts

LibOpen:                                /* a6 = base */
        addq.w  #1,32(%a6)
        bclr    #2,14(%a6)              /* clear LIBF_DELEXP */
        move.l  %a6,%d0
        rts

LibClose:
        moveq   #0,%d0
        subq.w  #1,32(%a6)
        bne.s   1f
        btst    #2,14(%a6)              /* LIBF_DELEXP pending? */
        beq.s   1f
        bra.s   LibExpunge
1:      rts

LibExpunge:
        tst.w   32(%a6)
        beq.s   2f
        bset    #2,14(%a6)              /* still open: delay */
        moveq   #0,%d0
        rts
2:      movem.l %d2/%a2/%a6,-(%sp)
        move.l  %a6,%a2                 /* a2 = our base */
        move.l  38(%a2),%a6             /* a6 = SysBase  */
        move.l  34(%a2),%d2             /* d2 = seglist  */
        move.l  %a2,%a1
        jsr     -252(%a6)               /* Remove() */
        moveq   #0,%d0
        move.w  16(%a2),%d0             /* NegSize */
        moveq   #0,%d1
        move.w  18(%a2),%d1             /* PosSize */
        move.l  %a2,%a1
        suba.l  %d0,%a1                 /* start of vector area */
        add.l   %d1,%d0                 /* total size */
        jsr     -210(%a6)               /* FreeMem(a1,d0) */
        move.l  %d2,%d0                 /* return seglist */
        movem.l (%sp)+,%d2/%a2/%a6
        rts

LibNull:
        moveq   #0,%d0
        rts

/* XPR entry points: A0 = struct XPR_IO *  ->  C function with stack arg */
        .extern xpr_cleanup_c, xpr_setup_c, xpr_send_c, xpr_receive_c
XProtocolCleanup:
        move.l  %a0,-(%sp)
        jbsr    xpr_cleanup_c
        addq.l  #4,%sp
        rts
XProtocolSetup:
        move.l  %a0,-(%sp)
        jbsr    xpr_setup_c
        addq.l  #4,%sp
        rts
XProtocolSend:
        move.l  %a0,-(%sp)
        jbsr    xpr_send_c
        addq.l  #4,%sp
        rts
XProtocolReceive:
        move.l  %a0,-(%sp)
        jbsr    xpr_receive_c
        addq.l  #4,%sp
        rts
XProtocolHostMon:
XProtocolUserMon:
        moveq   #0,%d0
        rts

        .even
EndOfCode:
