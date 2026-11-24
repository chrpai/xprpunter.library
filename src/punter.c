/*
 * punter.c - Punter C1 file transfer engine (sender + receiver).
 *
 * Wire format (all multi-byte values little endian):
 *
 *   offset 0  2  additive checksum   (sum of bytes 4..end, 16 bit)
 *   offset 2  2  cyclic checksum     (xor byte, rotate left 1, 16 bit)
 *   offset 4  1  size of the NEXT block (including its 7 header bytes)
 *   offset 5  2  block index (0,1,2...; last block is $FFFF)
 *   offset 7  .. payload (max 248 bytes)
 *
 * Handshake tokens (3 ASCII bytes): GOO BAD ACK S/B SYN
 *
 * Session: phase A sends one 8 byte block carrying the file type
 * (0 = PRG, 1 = SEQ), phase B sends a 7 byte header-only block followed by
 * the data blocks.  Each phase ends with an ACK, S/B, SYN, SYN, S/B exchange.
 *
 * The receive-side lost-sync handling follows the structure of the BSD-2
 * licensed CGTerm / CCGMS Future implementations (Copyright (c) 2003, 2016
 * Per Olofsson; modified by Michael Steil), which are known to interoperate
 * with real Commodore 64 software.  The sender is an independent
 * implementation with proper retry handling.
 */
#include "punter.h"

#define T_NONE  0
#define T_GOO   1
#define T_BAD   2
#define T_ACK   3
#define T_SB    4
#define T_SYN   5
#define T_ABORT (-1)

#define MAXBLK  255
#define HDR     7

#define HS_TMO   3000L      /* ms to wait for a handshake token            */
#define HS_TRIES 10         /* retries for a mid-session handshake         */
#define BYTE_TMO 4000L      /* ms inter-byte timeout while receiving block */
#define RESP_TMO 10000L     /* ms to wait for GOO/BAD after sending block  */
#define BLK_TRIES 30

static const char tokstr[6][4] = { "???", "GOO", "BAD", "ACK", "S/B", "SYN" };
static const char tokname[6][12] = { "tok:none", "tok:GOO", "tok:BAD", "tok:ACK", "tok:S/B", "tok:SYN" };

typedef struct {
    pt_io *io;
    pt_opts o;
    unsigned char win[3];
    long bytes, blocks, errors, timeouts;
    unsigned char buf[MAXBLK + 1];
    unsigned lastidx;
    int aborted;
} pt;

/* ------------------------------------------------------------------ */

static void trace(pt *p, const char *m)
{
    if (p->io->trace) p->io->trace(p->io->u, m);
}

static void status(pt *p, const char *m)
{
    if (p->io->status)
        p->io->status(p->io->u, m, p->bytes, p->blocks, p->errors, p->timeouts);
}

static int chk_abort(pt *p)
{
    if (!p->aborted && p->io->aborted && p->io->aborted(p->io->u))
        p->aborted = 1;
    return p->aborted;
}

static void purge(pt *p)
{
    int i;
    for (i = 0; i < 3; i++) p->win[i] = 0;
    if (p->io->purge) p->io->purge(p->io->u);
}

static int wr(pt *p, const unsigned char *b, int n)
{
    return p->io->wr(p->io->u, b, n) == 0;
}

static void put_token(pt *p, int t)
{
    wr(p, (const unsigned char *)tokstr[t], 3);
}

/* Read bytes through a 3 byte sliding window until a token shows up. */
static int read_token(pt *p, long tmo)
{
    unsigned char c;
    int junk = 0, t;
    while (junk < 96) {
        if (chk_abort(p)) return T_ABORT;
        if (!p->io->rd(p->io->u, &c, tmo)) {
            p->timeouts++;
            trace(p, tokname[0]);
            return T_NONE;
        }
        p->win[0] = p->win[1]; p->win[1] = p->win[2]; p->win[2] = c;
        for (t = T_GOO; t <= T_SYN; t++) {
            if (p->win[0] == (unsigned char)tokstr[t][0] &&
                p->win[1] == (unsigned char)tokstr[t][1] &&
                p->win[2] == (unsigned char)tokstr[t][2]) {
                p->win[0] = p->win[1] = p->win[2] = 0;
                trace(p, tokname[t]);
                return t;
            }
        }
        junk++;
    }
    return T_NONE;
}

/* Send `snd` (0 = nothing), wait for `want`; on any other outcome send again. */
static int handshake(pt *p, int snd, int want, int tries, long tmo)
{
    while (tries-- > 0) {
        if (chk_abort(p)) return 0;
        if (snd) put_token(p, snd);
        if (read_token(p, tmo) == want) return 1;
    }
    return 0;
}

/* Opening handshake.  Send `snd` ONCE, then just listen: every token that
 * arrives (other than `want`) is answered by repeating `snd`, and on a silent
 * line the token is repeated only every 10 seconds.  Repeating it every few
 * seconds fills the other side's input buffer with stale tokens while its
 * user is still typing a file name, which derails the receiver. */
static int start_handshake(pt *p, int snd, int want, int secs)
{
    int quiet = 0, guard = 400;
    long waited = 0, limit = ((long)secs << 10) - ((long)secs << 4) - ((long)secs << 3);
    put_token(p, snd);
    while (waited < limit && guard-- > 0) {
        int t = read_token(p, 5000L);
        if (t == T_ABORT || chk_abort(p)) return 0;
        if (t == want) return 1;
        if (t == T_NONE) {
            waited += 5000L;
            if (++quiet >= (p->io->beacon ? 1 : 2)) {
                if (p->io->beacon) p->io->beacon(p->io->u);
                put_token(p, snd); quiet = 0;
            }
        } else {
            put_token(p, snd);
        }
    }
    return 0;
}

/* Wait `ms` while throwing away anything that arrives. */
static void drain_wait(pt *p, long ms)
{
    unsigned char c;
    int n = 0;
    while (n++ < 64 && p->io->rd(p->io->u, &c, ms)) { /* discard */ }
    purge(p);
}

/* ------------------------------------------------------------------ */

static void make_cksum(unsigned char *b, int len)
{
    unsigned sum = 0, clc = 0;
    int i;
    for (i = 4; i < len; i++) {
        sum = (sum + b[i]) & 0xffffu;
        clc ^= b[i];
        clc = ((clc << 1) | (clc >> 15)) & 0xffffu;
    }
    b[0] = (unsigned char)sum; b[1] = (unsigned char)(sum >> 8);
    b[2] = (unsigned char)clc; b[3] = (unsigned char)(clc >> 8);
}

static int check_cksum(const unsigned char *b, int len)
{
    unsigned char t[MAXBLK + 1];
    int i;
    for (i = 0; i < len; i++) t[i] = b[i];
    make_cksum(t, len);
    return t[0] == b[0] && t[1] == b[1] && t[2] == b[2] && t[3] == b[3];
}

/* ------------------------------------------------------------------ */
/* Sender                                                              */

/* Send the block held in p->buf[0..len) with retries.  Checksum is added. */
/* `typeblk` != 0: this is the 8 byte file type block.  Receivers disagree about
 * two fields in it (the type code, and the unused next-size byte), so after a
 * failed attempt the sender cycles through the known variants:
 *   0: spec values   1: type code flipped   2: next-size 4   3: both         */
static const char *const varname[4] = {
    "type block variant 0 (spec)", "type block variant 1 (type code flipped)",
    "type block variant 2 (next-size 4)", "type block variant 3 (flipped, next-size 4)" };

static int send_block(pt *p, int len, int typeblk)
{
    int attempt, extra;
    unsigned char base = p->buf[7];
    if (!typeblk) make_cksum(p->buf, len);
    for (attempt = 0; attempt < BLK_TRIES; attempt++) {
        int t;
        if (typeblk) {
            int v = 0;                    /* no variants: CCGMS needs 1=PRG, 2=SEQ */
            p->buf[4] = (v & 2) ? 4 : HDR;
            p->buf[7] = (v & 1) ? (unsigned char)(base ^ 1) : base;
            make_cksum(p->buf, len);
            trace(p, varname[v]);
        }
        if (!handshake(p, T_ACK, T_SB, HS_TRIES, HS_TMO)) return 0;
        purge(p);                     /* anything queued now is stale */
        if (!wr(p, p->buf, len)) return 0;
        t = read_token(p, RESP_TMO);
        /* A receiver may send S/B twice (CCGMS does); the extra one can arrive
         * after our input flush.  It is not an answer to the block: ignore it
         * and keep waiting for GOO or BAD. */
        for (extra = 0; t == T_SB && extra < 4; extra++) {
            trace(p, "send: ignoring extra S/B");
            t = read_token(p, RESP_TMO);
        }
        if (t == T_ABORT) return 0;
        if (t == T_GOO) return 1;
        p->errors++;
        trace(p, t == T_BAD ? "send: BAD, retransmit" : "send: no reply, retry");
        status(p, "Retrying block");
    }
    return 0;
}

/* ACK/S/B, SYN/SYN, then three S/B a second apart (original sender timing). */
/* wantgoo: after the S/B tokens the receiver (CCGMS) sends GOO.  Its timing
 * depends on CPU speed (turbo vs stock), so try fast spacing first, then the
 * spec's ~1 s spacing.  Returns 1 with the GOO already consumed, 0 on failure.
 * Without wantgoo (end of file) just send the tokens and return 1. */
static int end_off(pt *p, int wantgoo)
{
    int i, pass, t;
    if (!handshake(p, T_ACK, T_SB, HS_TRIES, HS_TMO)) return 0;
    if (!handshake(p, T_SYN, T_SYN, HS_TRIES, HS_TMO)) return 0;
    if (!wantgoo) {
        for (i = 0; i < 3; i++) {
            put_token(p, T_SB);
            if (i < 2) drain_wait(p, 40);
            if (chk_abort(p)) return 0;
        }
        return 1;
    }
    /* Real senders send ONE S/B, then listen ~1 s (the receiver proceeds
     * after the first S/B and sends GOO).  Listen instead of discarding. */
    (void)pass;
    for (i = 0; i < 3; i++) {
        put_token(p, T_SB);
        t = read_token(p, 1000L);
        if (t == T_GOO) return 1;
        if (t == T_ABORT) return 0;
    }
    t = read_token(p, 30000L);   /* receiver opens its output file first */
    if (t == T_GOO) return 1;
    return 0;
}

void pt_default_opts(pt_opts *o)
{
    o->ftype = PT_TYPE_PRG;
    o->blocksize = MAXBLK;
    o->start_wait = 60;
    o->log = 0;
    o->pace = 0;
    o->autop = 0;
    o->multi = 0;
    o->chunk = 1;
}

static int num(const char **s)
{
    int v = 0, any = 0;
    while (**s >= '0' && **s <= '9') {
        v = (v << 3) + (v << 1) + (**s - '0');   /* v*10 without __mulsi3 */
        if (v > 9999) v = 9999;
        (*s)++; any = 1;
    }
    return any ? v : -1;
}


void pt_parse_opts(const char *s, pt_opts *o)
{
    if (!s) return;
    while (*s) {
        char k = *s++;
        if (k >= 'a' && k <= 'z') k -= 32;
        if (k == 'T') {
            char v = *s;
            if (v >= 'a' && v <= 'z') v -= 32;
            if (v == 'A') o->ftype = PT_TYPE_AUTO;
            else if (v == 'P') o->ftype = PT_TYPE_PRG;
            else if (v == 'S') o->ftype = PT_TYPE_SEQ;
            if (v) s++;
        } else if (k == 'B') {
            int v = num(&s);
            if (v >= 16 && v <= MAXBLK) o->blocksize = v;
        } else if (k == 'L') {
            o->log = 1;
        } else if (k == 'M') {
            o->multi = 1;
            if (*s == '2') { o->multi = 2; s++; }
        } else if (k == 'D') {
            int v = num(&s);
            if (v >= 0 && v <= 3) o->pace = v;
        } else if (k == 'K') {
            int v = num(&s);
            if (v >= 1 && v <= 63) o->chunk = v;
        } else if (k == 'W') {
            int v = num(&s);
            if (v >= 5 && v <= 600) o->start_wait = v;
        }
        while (*s && *s != ',' && *s != ' ' && *s != ';') s++;
        while (*s == ',' || *s == ' ' || *s == ';') s++;
    }
}

int pt_send(pt_io *io, const pt_opts *o, int ftype)
{
    pt P;
    pt *p = &P;
    unsigned char *cur, *nxt;
    unsigned char curbuf[MAXBLK], nxtbuf[MAXBLK];
    int maxpay, curlen, nxtlen, r, i;
    unsigned idx;

    P.io = io; P.o = *o;
    P.bytes = P.blocks = P.errors = P.timeouts = 0;
    P.aborted = 0; P.lastidx = 0;
    purge(p);
    maxpay = o->blocksize - HDR;
    if (maxpay < 9) maxpay = 9;
    if (maxpay > MAXBLK - HDR) maxpay = MAXBLK - HDR;

    /* ---- Phase A: file type ---- */
    status(p, "Waiting for receiver");
    {
        if (!start_handshake(p, T_GOO, T_GOO, o->start_wait)) {
            status(p, chk_abort(p) ? "Aborted" : "No response from receiver");
            return PT_FAIL;
        }
    }
    status(p, "Sending file type");
    p->buf[4] = HDR;
    p->buf[5] = 0xff; p->buf[6] = 0xff;
    p->buf[7] = (ftype == PT_TYPE_SEQ) ? 2 : 1;   /* CCGMS: 1=PRG 2=SEQ */
    if (!send_block(p, HDR + 1, 1)) goto fail;
    if (!end_off(p, 1)) goto fail;   /* returns with receiver's GOO consumed */

    /* ---- Phase B: file data ---- */
    status(p, "Starting transfer");

    cur = curbuf; nxt = nxtbuf;
    r = io->src_read(io->u, cur, maxpay);
    if (r < 0) goto fail;
    curlen = r;

    /* header-only block announces size of first data block */
    p->buf[4] = (unsigned char)(curlen + HDR);
    p->buf[5] = 0; p->buf[6] = 0;
    if (!send_block(p, HDR, 0)) goto fail;

    idx = 1;
    for (;;) {
        int last = (curlen < maxpay);
        if (!last) {
            r = io->src_read(io->u, nxt, maxpay);
            if (r < 0) goto fail;
            nxtlen = r;
        } else {
            nxtlen = 0;
        }
        p->buf[4] = (unsigned char)(nxtlen + HDR);
        p->buf[5] = (unsigned char)(last ? 0xff : (idx & 0xff));
        p->buf[6] = (unsigned char)(last ? 0xff : ((idx >> 8) & 0xff));
        for (i = 0; i < curlen; i++) p->buf[HDR + i] = cur[i];
        if (!send_block(p, HDR + curlen, 0)) goto fail;
        p->bytes += curlen;
        p->blocks++;
        status(p, "Sending");
        if (last) break;
        { unsigned char *t = cur; cur = nxt; nxt = t; }
        curlen = nxtlen;
        idx++;
        if (idx >= 0xff00u) goto fail;   /* file too large for Punter */
    }
    if (!end_off(p, 0)) goto fail;
    status(p, "Transfer complete");
    return PT_OK;
fail:
    status(p, chk_abort(p) ? "Aborted" : "Transfer failed");
    return PT_FAIL;
}

/* ------------------------------------------------------------------ */
/* Receiver                                                            */

static int starts_with(const unsigned char *b, const char *s)
{
    while (*s) if (*b++ != (unsigned char)*s++) return 0;
    return 1;
}

/* Receive one block of `len` bytes.  Returns the next block size (>=0) or
 * -1 on failure.  `save` non-zero: payload (len-7 bytes) goes to the file. */
static int recv_block(pt *p, int len, int save)
{
    int errorcnt = BLK_TRIES;
    int cnt;
    unsigned char c;

restart:
    if (chk_abort(p)) return -1;
    put_token(p, T_SB);
    cnt = 0;
    while (cnt < len) {
        if (chk_abort(p)) return -1;
        if (!p->io->rd(p->io->u, &c, BYTE_TMO)) {
            p->timeouts++;
            if (cnt == 3 && starts_with(p->buf, "S/B")) {
                trace(p, "recv: remote echoed S/B - cancelled");
                return -1;
            }
            trace(p, "recv: byte timeout inside block");
            status(p, "Block timed out, retrying");
            if (!handshake(p, T_BAD, T_ACK, HS_TRIES, HS_TMO)) return -1;
            if (errorcnt-- <= 0) return -1;
            p->errors++;
            goto restart;
        }
        p->buf[cnt++] = c;
        if (cnt == 4 && starts_with(p->buf, "ACK")) {
            /* sender repeated its ACK: we lost sync */
            status(p, "Lost sync, retrying...");
            if (p->buf[3] == 'A') goto restart;
            p->buf[0] = p->buf[3];
            cnt = 1;
        }
        if (cnt == 8 &&
            (starts_with(p->buf + 2, "ACKACK") ||
             starts_with(p->buf + 2, "CKACKA") ||
             starts_with(p->buf + 2, "KACKAC"))) {
            status(p, "Lost sync, retrying...");
            goto restart;
        }
    }
    if (check_cksum(p->buf, len)) {
        if (!handshake(p, T_GOO, T_ACK, HS_TRIES, HS_TMO)) return -1;
        p->lastidx = (unsigned)p->buf[5] | ((unsigned)p->buf[6] << 8);
        if (save && len > HDR) {
            if (p->io->dst_write(p->io->u, p->buf + HDR, len - HDR) != 0) {
                status(p, "Write error");
                return -1;
            }
            p->bytes += len - HDR;
        }
        if (save) p->blocks++;
        return p->buf[4];
    }
    p->errors++;
    trace(p, "recv: checksum mismatch");
    status(p, "Checksum failed, retrying");
    if (!handshake(p, T_BAD, T_ACK, HS_TRIES, HS_TMO)) return -1;
    if (errorcnt-- <= 0) return -1;
    goto restart;
}

int pt_receive(pt_io *io, const pt_opts *o, int *ftype_out)
{
    pt P;
    pt *p = &P;
    int next;

    P.io = io; P.o = *o;
    P.bytes = P.blocks = P.errors = P.timeouts = 0;
    P.aborted = 0; P.lastidx = 0;
    purge(p);

    status(p, "Waiting for sender");
    {
        if (!start_handshake(p, T_GOO, T_ACK, o->start_wait)) {
            status(p, chk_abort(p) ? "Aborted" : "No response from sender");
            return PT_FAIL;
        }
    }

    /* ---- Phase A ---- */
    status(p, "Receiving file type");
    if (recv_block(p, HDR + 1, 0) < 0) goto fail;
    if (ftype_out) *ftype_out = (p->buf[7] == 2) ? PT_TYPE_SEQ : PT_TYPE_PRG;

    if (!handshake(p, T_GOO, T_ACK, HS_TRIES, HS_TMO)) goto fail;
    if (!handshake(p, T_SB, T_SYN, HS_TRIES, HS_TMO)) goto fail;
    if (!handshake(p, T_SYN, T_SB, HS_TRIES, HS_TMO)) goto fail;
    /* sender repeats S/B up to 3 times; swallow them, then settle */
    read_token(p, 2500);
    read_token(p, 2500);
    drain_wait(p, 500);
    if (chk_abort(p)) goto fail;

    /* ---- Phase B ---- */
    if (!handshake(p, T_GOO, T_ACK, HS_TRIES, HS_TMO)) goto fail;
    status(p, "Starting transfer");
    next = recv_block(p, HDR, 0);
    if (next < 0) goto fail;

    for (;;) {
        if (next < HDR) goto fail;
        next = recv_block(p, next, 1);
        if (next < 0) goto fail;
        status(p, "Receiving");
        if (p->lastidx >= 0xff00u) break;
    }

    status(p, "Finishing");
    if (handshake(p, T_SB, T_SYN, HS_TRIES, HS_TMO))
        handshake(p, T_SYN, T_SB, HS_TRIES, HS_TMO);
    status(p, "Transfer complete");
    return PT_OK;
fail:
    status(p, chk_abort(p) ? "Aborted" : "Transfer failed");
    return PT_FAIL;
}
