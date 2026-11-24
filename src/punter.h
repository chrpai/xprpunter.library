/*
 * punter.h - Punter C1 ("New Punter") file transfer engine.
 *
 * Portable, freestanding C (no libc).  All I/O goes through the pt_io
 * callback table so the same engine runs inside xprpunter.library on AmigaOS
 * and in the native test-suite.
 *
 * Protocol knowledge: Steve Punter's C1 description plus the analysis at
 * pagetable.com ("The Punter C1 Protocol") and the BSD-2-Clause CGTerm /
 * CCGMS-Future reference implementations by Per Olofsson and Michael Steil.
 */
#ifndef PUNTER_H
#define PUNTER_H

#define PT_OK    1
#define PT_FAIL  0

#define PT_TYPE_AUTO 0
#define PT_TYPE_PRG  1
#define PT_TYPE_SEQ  2

typedef struct pt_io {
    void *u;                                   /* user pointer for callbacks   */
    /* read one byte, wait at most timeout_ms.  1 = got a byte, 0 = timeout.  */
    int  (*rd)(void *u, unsigned char *b, long timeout_ms);
    /* write n bytes.  0 = ok.                                               */
    int  (*wr)(void *u, const unsigned char *b, int n);
    /* discard pending serial input (may be NULL)                            */
    void (*purge)(void *u);
    /* non-zero: user/host asked us to abort (may be NULL)                   */
    int  (*aborted)(void *u);
    /* file to send: read up to n bytes; returns count, 0 = EOF, <0 = error  */
    int  (*src_read)(void *u, unsigned char *b, int n);
    /* file being received: write n bytes.  0 = ok                           */
    int  (*dst_write)(void *u, const unsigned char *b, int n);
    /* progress display (may be NULL)                                        */
    void (*status)(void *u, const char *msg, long bytes, long blocks,
                   long errors, long timeouts);
    /* debug trace (may be NULL)                                             */
    void (*trace)(void *u, const char *msg);
    /* send only: called before each periodic re-send of the opening GOO
       (Multi-Punter uses it to repeat the file header; may be NULL)        */
    void (*beacon)(void *u);
} pt_io;

typedef struct pt_opts {
    int ftype;        /* PT_TYPE_*  (send only; AUTO resolved by caller)     */
    int blocksize;    /* total block size incl. 7 byte header, 16..255       */
    int start_wait;   /* seconds to wait for the other side to start         */
    int log;          /* 1 = write a byte-level diagnostic log (glue layer)  */
    int pace;         /* ticks (1/50 s) to pause between output chunks, 0=off */
    int autop;        /* 1 = escalate pacing automatically after errors      */
    int chunk;        /* bytes per output chunk when pacing (1..63)           */
    int multi;        /* 1 = Multi-Punter batch framing (glue layer)         */
} pt_opts;

void pt_default_opts(pt_opts *o);
/* Parse an environment string such as "TA,B255,W60".  Unknown keys ignored. */
void pt_parse_opts(const char *s, pt_opts *o);

/* ftype: 1 = PRG, 2 = SEQ (PT_TYPE_*).  Returns PT_OK / PT_FAIL.            */
int  pt_send(pt_io *io, const pt_opts *o, int ftype);
/* *ftype_out receives PT_TYPE_PRG / PT_TYPE_SEQ if the type block arrived.  */
int  pt_receive(pt_io *io, const pt_opts *o, int *ftype_out);

#endif
