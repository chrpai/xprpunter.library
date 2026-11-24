/* m68k harness: loads the real xprpunter.library under vamos, supplies XPR
 * callbacks, and drives it against a small *reactive* scripted peer. */
typedef unsigned char u8;

extern void stub_fopen(void), stub_fclose(void), stub_fread(void), stub_fwrite(void),
            stub_sread(void), stub_swrite(void), stub_sflush(void), stub_update(void),
            stub_chkabort(void), stub_finfo(void);

struct XPR_IO {
    char *xpr_filename;
    long (*xpr_fopen)(void), (*xpr_fclose)(void), (*xpr_fread)(void), (*xpr_fwrite)(void),
         (*xpr_sread)(void), (*xpr_swrite)(void), (*xpr_sflush)(void), (*xpr_update)(void),
         (*xpr_chkabort)(void), (*xpr_chkmisc)(void), (*xpr_gets)(void), (*xpr_setserial)(void),
         (*xpr_ffirst)(void), (*xpr_fnext)(void), (*xpr_finfo)(void), (*xpr_fseek)(void);
    long xpr_extension;
    long *xpr_data;
    long (*xpr_options)(void);
};
struct XPR_UPDATE {
    long mask; char *protocol; char *filename; long filesize; char *msg; char *errormsg;
    long blocks, blocksize, bytes, errors, timeouts, packettype, packetdelay, chardelay;
    char *blockcheck, *expecttime, *elapsedtime; long datarate; long reserved[5];
};

void *memcpy(void *d, const void *s, unsigned long n)
{ u8 *dd = d; const u8 *ss = s; while (n--) *dd++ = *ss++; return d; }
void *memset(void *d, int v, unsigned long n)
{ u8 *dd = d; while (n--) *dd++ = (u8)v; return d; }
/* harness-only 32-bit multiply (68000 has none); the library itself needs no libgcc */
unsigned long __mulsi3(unsigned long a, unsigned long b)
{ unsigned long r = 0; while (b) { if (b & 1) r += a; a <<= 1; b >>= 1; } return r; }

/* ---------------- Amiga system calls ---------------- */
static inline long amicall(void *base, int lvo, long d0, long d1, long d2, long d3, void *a0, void *a1)
{
    /* a6 is gcc's frame pointer on m68k, so it is loaded inside the asm
     * template instead of via a register variable (build uses -ffixed-a6). */
    register long r_d0 __asm__("d0") = d0, r_d1 __asm__("d1") = d1;
    register long r_d2 __asm__("d2") = d2, r_d3 __asm__("d3") = d3;
    register void *r_a0 __asm__("a0") = a0, *r_a1 __asm__("a1") = a1;
    long fn = (long)base + lvo;
    __asm__ volatile ("move.l %%a6,-(%%sp)\n\t"
                      "move.l %[b],%%a6\n\t"
                      "jsr (%[f])\n\t"
                      "move.l (%%sp)+,%%a6"
                      : "+r"(r_d0), "+r"(r_d1), "+r"(r_a0), "+r"(r_a1)
                      : [b] "a"(base), [f] "a"(fn), "r"(r_d2), "r"(r_d3)
                      : "memory", "cc");
    return r_d0;
}
void *SysBase, *DOSBase; long outfh; void say(const char *s) { long n = 0; while (s[n]) n++; amicall(DOSBase, -48, 0, outfh, (long)s, n, 0, 0); }
static void sayn(const char *s, long v) {
    char b[16]; int i = 15; b[i] = 0; int neg = v < 0; unsigned long u = neg ? -v : v;
    if (!u) b[--i] = '0';
    while (u) { unsigned long q = 0, r = u; while (r >= 10) { r -= 10; q++; } b[--i] = '0' + (char)r; u = q; }
    if (neg) b[--i] = '-';
    say(s); say(b + i); say("\n");
}

/* ---------------- test data ---------------- */
#define MAXD 4096
u8 orig[MAXD], rcv[MAXD + 512], inq[16384];
long origlen, rcvlen, inhead, intail;
unsigned lcg = 1; void fill(long n) { origlen = n; for (long i = 0; i < n; i++) { lcg = lcg * 1664525u + 1013904223u; orig[i] = (u8)(lcg >> 24); }
    if (n > 40) { orig[10]='G';orig[11]='O';orig[12]='O';orig[30]='A';orig[31]='C';orig[32]='K'; } }
static void push(const char *s) { while (*s) inq[intail++] = (u8)*s++; }
static void pushb(const u8 *b, int n) { while (n--) inq[intail++] = *b++; }

static void ck(u8 *b, int len) {
    unsigned s = 0, c = 0;
    for (int i = 4; i < len; i++) { s = (s + b[i]) & 0xffff; c ^= b[i]; c = ((c << 1) | (c >> 15)) & 0xffff; }
    b[0] = s; b[1] = s >> 8; b[2] = c; b[3] = c >> 8;
}

/* ---------------- host callbacks (C side) ---------------- */
int mode;            /* 1 = library sends (peer receives), 2 = library receives */
long updates, lastbytes; char lastmsg[48]; long lastsize, lastproto;
long rpos;
int sawsize = -1;
long n_sread, n_swrite, n_fopen, n_fread, n_sflush;

long c_fopen(char *n, char *m) { n_fopen++; (void)n; (void)m; rpos = 0; return 0x1234; }
long c_fclose(long h) { (void)h; return 0; }
long c_fread(u8 *b, long size, long cnt, long h)
{
    (void)h; long n = cnt; (void)size; if (n > origlen - rpos) n = origlen - rpos;
    for (long i = 0; i < n; i++) b[i] = orig[rpos + i];
    rpos += n; return n;
}
long c_fwrite(u8 *b, long size, long cnt, long h)
{
    (void)h; (void)size; long n = cnt;
    for (long i = 0; i < n; i++) rcv[rcvlen + i] = b[i];
    rcvlen += n; return n;
}
long c_finfo(char *n, long type) { (void)n; sawsize = 1; return type == 1 ? origlen : 0; }
long c_sflush(void) { n_sflush++; inhead = intail; return 0; }
long c_chkabort(void) { return 0; }
long c_update(struct XPR_UPDATE *u)
{
    updates++;
    if (u->mask & 0x80) lastbytes = u->bytes;
    if (u->mask & 0x08) { int i = 0; while (u->msg[i] && i < 47) { lastmsg[i] = u->msg[i]; i++; } lastmsg[i] = 0; }
    if (u->mask & 4) lastsize = u->filesize;
    if ((u->mask & 1) && u->protocol && u->protocol[0] == 'P') lastproto = 1;
    return 0;
}
long c_sread(u8 *b, long size, long to)
{
    (void)to; n_sread++;
    if (size != 1) return -1;                       /* engine only ever reads 1 byte */
    if (inhead < intail) { b[0] = inq[inhead++]; if (inhead == intail) inhead = intail = 0; return 1; }
    return -1;                                      /* timeout */
}

/* ---- reactive peer #1: a Punter RECEIVER (tests library send) ---- */
int  rej, dupsb;
int  ph, inblk, need, got, nexts, endsb; u8 blk[256]; u8 w[3];
long rx_off; int rxtype; int tok(const char *t) { return w[0]==(u8)t[0] && w[1]==(u8)t[1] && w[2]==(u8)t[2]; }
static void peer_recv_byte(u8 c)
{
    if (inblk) {
        blk[got++] = c;
        if (got == need) {
            unsigned char t[256]; for (int i = 0; i < got; i++) t[i] = blk[i];
            ck(t, got);
            inblk = 0;
            if (dupsb) push("S/B");          /* stale duplicate S/B before the answer */
            if (rej > 0) { rej--; push("BAD"); }
            else if (t[0]==blk[0] && t[1]==blk[1] && t[2]==blk[2] && t[3]==blk[3]) {
                push("GOO");
                if (ph == 1) { rxtype = blk[7]; ph = 2; }
                else if (ph == 3) { nexts = blk[4]; ph = 4; }
                else if (ph == 4) {
                    for (int i = 7; i < got; i++) rcv[rx_off++] = blk[i];
                    nexts = blk[4];
                    if (blk[5] == 0xff && blk[6] == 0xff) ph = 5;
                }
            } else push("BAD");
        }
        return;
    }
    w[0] = w[1]; w[1] = w[2]; w[2] = c;
    if (tok("GOO") && ph == 0) { push("GOO"); }
    else if (tok("ACK")) {
        push("S/B");
        if (ph == 0 || ph == 1) { ph = 1; inblk = 1; need = 8; got = 0; }
        else if (ph == 3) { inblk = 1; need = 7; got = 0; }
        else if (ph == 4) { inblk = 1; need = nexts; got = 0; }
    }
    else if (tok("SYN")) { push("SYN"); }
    else if (tok("S/B")) {
        if (ph == 2) { if (++endsb == 3) { push("GOO"); ph = 3; endsb = 0; } }
    }
}

/* ---- reactive peer #2: a Punter SENDER (tests library receive) ---- */
int ss; long sp_off; int sp_idx; void send_data_block(void)
{
    u8 b[256]; int n = origlen - sp_off; if (n > 248) n = 248; if (n < 0) n = 0;
    int last = n < 248;
    int nn = origlen - (sp_off + n); if (nn > 248) nn = 248; if (nn < 0) nn = 0;
    b[4] = nn + 7; b[5] = last ? 0xff : sp_idx & 0xff; b[6] = last ? 0xff : sp_idx >> 8;
    for (int i = 0; i < n; i++) b[7 + i] = orig[sp_off + i];
    ck(b, 7 + n); pushb(b, 7 + n); sp_off += n; sp_idx++;
    ss = last ? 11 : 10;
}
static void peer_send_byte(u8 c)
{
    w[0] = w[1]; w[1] = w[2]; w[2] = c;
    if (tok("GOO")) {
        if (ss == 0) { push("ACK"); ss = 1; }
        else if (ss == 2) { push("ACK"); ss = 3; }
        else if (ss == 3) { push("ACK"); ss = 4; }
        else if (ss == 6) { push("ACK"); ss = 7; }
        else if (ss == 8) { push("ACK"); ss = 9; }
        else if (ss == 10 || ss == 11) { push("ACK"); ss = ss == 10 ? 9 : 12; }
    } else if (tok("S/B")) {
        if (ss == 1) { u8 b[8]; b[4] = 7; b[5] = 0xff; b[6] = 0xff; b[7] = 1; ck(b, 8); pushb(b, 8); ss = 2; }
        else if (ss == 4) { push("SYN"); ss = 5; }
        else if (ss == 7) {
            u8 b[7]; int n = origlen > 248 ? 248 : origlen; b[4] = n + 7; b[5] = 0; b[6] = 0; ck(b, 7); pushb(b, 7);
            ss = 8; sp_off = 0; sp_idx = 1;
        }
        else if (ss == 9) send_data_block();
        else if (ss == 12) { push("SYN"); ss = 13; }
    } else if (tok("SYN")) {
        if (ss == 5) { push("S/B"); push("S/B"); push("S/B"); ss = 6; }
        else if (ss == 13) { push("S/B"); ss = 14; }
    }
}
long c_swrite(u8 *b, long n)
{
    n_swrite++;
    for (long i = 0; i < n; i++) { if (mode == 1) peer_recv_byte(b[i]); else peer_send_byte(b[i]); }
    return 0;
}

/* ---------------- tests ---------------- */
static int run(void *xpr, long size, int m, char *env, int verify_type)
{
    struct XPR_IO io; long ok;
    char *p = (char *)&io; for (unsigned i = 0; i < sizeof io; i++) p[i] = 0;
    io.xpr_fopen = (void *)stub_fopen; io.xpr_fclose = (void *)stub_fclose;
    io.xpr_fread = (void *)stub_fread; io.xpr_fwrite = (void *)stub_fwrite;
    io.xpr_sread = (void *)stub_sread; io.xpr_swrite = (void *)stub_swrite;
    io.xpr_sflush = (void *)stub_sflush; io.xpr_update = (void *)stub_update;
    io.xpr_chkabort = (void *)stub_chkabort; io.xpr_finfo = (void *)stub_finfo;
    io.xpr_extension = 1;
    mode = m; fill(size);
    n_sread = n_swrite = n_fopen = n_fread = n_sflush = 0; inhead = intail = 0; rcvlen = 0; rx_off = 0; rpos = 0; updates = 0; lastbytes = -1; lastmsg[0] = 0; lastsize = -9;
    ph = inblk = need = got = nexts = endsb = ss = 0; rxtype = -1; sawsize = -1; lastproto = 0;
    w[0] = w[1] = w[2] = 0;

    io.xpr_filename = env;
    ok = amicall(xpr, -36, 0, 0, 0, 0, &io, 0);           /* XProtocolSetup */
    if (!ok) { say("  Setup failed\n"); return 0; }
    io.xpr_filename = m == 1 ? "ram:test.txt" : "ram:out.prg";
    ok = amicall(xpr, m == 1 ? -42 : -48, 0, 0, 0, 0, &io, 0);   /* Send / Receive */
    amicall(xpr, -30, 0, 0, 0, 0, &io, 0);                /* XProtocolCleanup */
    long got_len = m == 1 ? rx_off : rcvlen;
    int good = ok == 1 && got_len == origlen;
    for (long i = 0; good && i < origlen; i++) if (rcv[i] != orig[i]) good = 0;
    if (m == 1 && verify_type >= 0 && rxtype != verify_type) good = 0;
    if (io.xpr_data != 0) good = 0;                        /* Cleanup must clear the slot */
    if (updates == 0 || lastproto != 1) good = 0;
    say(m == 1 ? "  send " : "  recv "); sayn("size=", size);
    say(good ? "    PASS" : "    FAIL"); sayn("  rc=", ok); sayn("    bytes got=", got_len);
    say("    last status: "); say(lastmsg); sayn("  updates=", updates);
    if (m == 1) sayn("    peer saw file type=", rxtype);
    return good;
}

void harness_main(void)
{
    SysBase = *(void * volatile *)(long)4;
    DOSBase = (void *)amicall(SysBase, -552, 0, 0, 0, 0, 0, "dos.library");
    outfh = amicall(DOSBase, -60, 0, 0, 0, 0, 0, 0);
    void *xpr = (void *)amicall(SysBase, -552, 0, 0, 0, 0, 0, "xprpunter.library");
    if (!xpr) { say("OpenLibrary(xprpunter.library) FAILED\n"); return; }
    say("xprpunter.library opened OK\n");
    sayn("  lib version=", *(unsigned short *)((char *)xpr + 20));
    int fails = 0; const long sz[] = { 0, 5, 240, 248, 249, 600, 3000 };
    for (unsigned i = 0; i < sizeof sz / sizeof *sz; i++) fails += !run(xpr, sz[i], 1, "TS,B255", 2);
    for (unsigned i = 0; i < sizeof sz / sizeof *sz; i++) fails += !run(xpr, sz[i], 1, "TP,B64", 1);
    for (unsigned i = 0; i < sizeof sz / sizeof *sz; i++) fails += !run(xpr, sz[i], 2, "", -1);
    say("-- with diagnostic log enabled (option L) --\n");
    fails += !run(xpr, 300, 1, "TS,B64,L", 2);
    fails += !run(xpr, 300, 2, "L", -1);
    say("-- with output pacing (D1,K4) --\n");
    fails += !run(xpr, 300, 1, "TS,B64,D1,K4,L", 2);
    fails += !run(xpr, 300, 2, "D1,K8", -1);
    say("-- peer sends a duplicate S/B before each answer --\n");
    dupsb = 1; fails += !run(xpr, 300, 1, "TS,B64,L", 2); dupsb = 0;
    say("-- auto-pace: peer rejects first 5 blocks --\n");
    rej = 5; fails += !run(xpr, 300, 1, "TS,B64,A,L", -1); rej = 0;   /* type code flips after failures */
    amicall(SysBase, -414, 0, 0, 0, 0, 0, xpr);            /* CloseLibrary */
    say(fails ? "HARNESS: FAILURES\n" : "HARNESS: ALL PASSED\n");
    sayn("failures=", fails);
}
