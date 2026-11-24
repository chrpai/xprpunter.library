/*
 * xprpunter.c - XPR (eXternal PRotocol) glue for the Punter C1 engine.
 *
 * XPR callbacks receive their arguments in 68k registers, so every call into
 * the host goes through a tiny inline-asm trampoline.  No libc is used.
 */
#include "punter.h"

/* ------------------------------------------------------------------ */
/* XPR structures (xproto.h v2.0 layout)                               */

struct XPR_IO {
    char *xpr_filename;
    long (*xpr_fopen)(void);
    long (*xpr_fclose)(void);
    long (*xpr_fread)(void);
    long (*xpr_fwrite)(void);
    long (*xpr_sread)(void);
    long (*xpr_swrite)(void);
    long (*xpr_sflush)(void);
    long (*xpr_update)(void);
    long (*xpr_chkabort)(void);
    long (*xpr_chkmisc)(void);
    long (*xpr_gets)(void);
    long (*xpr_setserial)(void);
    long (*xpr_ffirst)(void);
    long (*xpr_fnext)(void);
    long (*xpr_finfo)(void);
    long (*xpr_fseek)(void);
    long xpr_extension;
    long *xpr_data;
    long (*xpr_options)(void);
};

struct XPR_UPDATE {
    long  xpru_updatemask;
    char *xpru_protocol;
    char *xpru_filename;
    long  xpru_filesize;
    char *xpru_msg;
    char *xpru_errormsg;
    long  xpru_blocks;
    long  xpru_blocksize;
    long  xpru_bytes;
    long  xpru_errors;
    long  xpru_timeouts;
    long  xpru_packettype;
    long  xpru_packetdelay;
    long  xpru_chardelay;
    char *xpru_blockcheck;
    char *xpru_expecttime;
    char *xpru_elapsedtime;
    long  xpru_datarate;
    long  xpru_reserved[5];
};

#define XPRU_PROTOCOL  0x00000001L
#define XPRU_FILENAME  0x00000002L
#define XPRU_FILESIZE  0x00000004L
#define XPRU_MSG       0x00000008L
#define XPRU_BLOCKS    0x00000020L
#define XPRU_BLOCKSIZE 0x00000040L
#define XPRU_BYTES     0x00000080L
#define XPRU_ERRORS    0x00000100L
#define XPRU_TIMEOUTS  0x00000200L

/* ------------------------------------------------------------------ */
/* Register-argument trampoline: jsr (fn) with a0/a1/d0/d1 loaded       */

static inline long xcall(void *fn, void *a0, void *a1, long d0, long d1)
{
    register long  r_d0 __asm__("d0") = d0;
    register long  r_d1 __asm__("d1") = d1;
    register void *r_a0 __asm__("a0") = a0;
    register void *r_a1 __asm__("a1") = a1;
    __asm__ volatile ("jsr (%[f])"
                      : "+r"(r_d0), "+r"(r_d1), "+r"(r_a0), "+r"(r_a1)
                      : [f] "a"(fn)
                      : "memory", "cc");
    return r_d0;
}


/* ------------------------------------------------------------------ */
/* Optional diagnostic log (environment option "L").
 * Writes every byte sent/received, engine events and status messages to
 * CNet:punter.log (falls back to RAM:punter.log).  Debug aid only: it uses
 * one global buffer, so enable it for a single line at a time.            */

static inline long amicall(void *base, int lvo, long d0, long d1, long d2, long d3)
{
    register long r_d0 __asm__("d0") = d0, r_d1 __asm__("d1") = d1;
    register long r_d2 __asm__("d2") = d2, r_d3 __asm__("d3") = d3;
    long fn = (long)base + lvo;
    __asm__ volatile ("move.l %%a6,-(%%sp)\n\t"
                      "move.l %[b],%%a6\n\t"
                      "jsr (%[f])\n\t"
                      "move.l (%%sp)+,%%a6"
                      : "+r"(r_d0), "+r"(r_d1)
                      : [b] "a"(base), [f] "a"(fn), "r"(r_d2), "r"(r_d3)
                      : "memory", "cc", "a0", "a1");
    return r_d0;
}

#define LOGCAP 8192
static struct {
    char *buf;  long len;
    void *sysbase, *dosbase;
    long  fh;
    long  t0;
    long  ds[3];
    int   on;
} G;

static long log_ticks(void)             /* 1/50 s units */
{
    long m;
    amicall(G.dosbase, -192, 0, (long)G.ds, 0, 0);     /* DateStamp(&ds) */
    m = G.ds[1];
    return (m << 12) - (m << 10) - (m << 6) - (m << 3) + G.ds[2];
}

static void log_flush(void)
{
    if (G.fh && G.len) amicall(G.dosbase, -48, 0, G.fh, (long)G.buf, G.len);
    G.len = 0;
}

static void log_ch(char c)
{
    if (G.len >= LOGCAP - 1) log_flush();
    G.buf[G.len++] = c;
}

static void log_str(const char *s) { while (*s) log_ch(*s++); }

static void log_dec(long v)
{
    static const long p10[] = { 100000L, 10000L, 1000L, 100L, 10L, 1L };
    int i, started = 0;
    if (v < 0) { log_ch('-'); v = -v; }
    for (i = 0; i < 6; i++) {
        char d = '0';
        while (v >= p10[i]) { v -= p10[i]; d++; }
        if (d != '0' || started || i == 5) { log_ch(d); started = 1; }
    }
}

static void log_hex(unsigned v)
{
    static const char h[] = "0123456789ABCDEF";
    log_ch(h[(v >> 4) & 15]); log_ch(h[v & 15]);
}

static void log_stamp(void)             /* "[tttttt] " relative ticks */
{
    log_ch('['); log_dec(log_ticks() - G.t0); log_str("] ");
}

static void log_begin(void)
{
    static char n1[] = "CNet:punter0.log";
    static char n2[] = "RAM:punter0.log";
    static int seq;
    const char *names[3];
    int i;
    seq = (seq >= 9) ? 1 : seq + 1;
    n1[11] = (char)('0' + seq); n2[10] = (char)('0' + seq);
    names[0] = n1; names[1] = n2; names[2] = 0;
    G.on = 0; G.fh = 0; G.len = 0;
    G.sysbase = *(void *volatile *)(long)4;
    G.buf = (char *)amicall(G.sysbase, -198, LOGCAP, 0x10001L, 0, 0);  /* AllocMem CLEAR|PUBLIC */
    if (!G.buf) return;
    G.dosbase = 0;
    {
        register void *a1 __asm__("a1") = (void *)"dos.library";
        register long d0 __asm__("d0") = 0;
        __asm__ volatile ("move.l %%a6,-(%%sp)\n\tmove.l %[b],%%a6\n\tjsr -552(%%a6)\n\tmove.l (%%sp)+,%%a6"
                          : "+r"(d0), "+r"(a1) : [b] "a"(G.sysbase) : "memory", "cc", "a0", "d1");
        G.dosbase = (void *)d0;
    }
    if (!G.dosbase) return;
    for (i = 0; names[i] && !G.fh; i++)
        G.fh = amicall(G.dosbase, -30, 0, (long)names[i], 1006L, 0);   /* Open(name, MODE_NEWFILE) */
    if (!G.fh) return;
    G.on = 1;
    G.t0 = log_ticks();
    log_str("xprpunter.library 1.15 diagnostic log (times in 1/50 s)\r\n");
}

static void log_end(void)
{
    if (G.on) {
        log_str("-- end of log --\r\n");
        log_flush();
        amicall(G.dosbase, -36, 0, G.fh, 0, 0);              /* Close */
    }
    if (G.dosbase) {
        register void *a1 __asm__("a1") = G.dosbase;
        __asm__ volatile ("move.l %%a6,-(%%sp)\n\tmove.l %[b],%%a6\n\tjsr -414(%%a6)\n\tmove.l (%%sp)+,%%a6"
                          : "+r"(a1) : [b] "a"(G.sysbase) : "memory", "cc", "d0", "d1", "a0");
    }
    if (G.buf) {
        register void *a1 __asm__("a1") = G.buf;
        register long d0 __asm__("d0") = LOGCAP;
        __asm__ volatile ("move.l %%a6,-(%%sp)\n\tmove.l %[b],%%a6\n\tjsr -210(%%a6)\n\tmove.l (%%sp)+,%%a6"
                          : "+r"(d0), "+r"(a1) : [b] "a"(G.sysbase) : "memory", "cc", "d1", "a0");
    }
    G.on = 0; G.buf = 0; G.dosbase = 0; G.fh = 0;
}

static void log_bytes(const char *dir, const unsigned char *b, int n)
{
    int i, max = n > 40 ? 40 : n;
    log_stamp(); log_str(dir); log_ch(' '); log_dec(n); log_str(" byte(s):");
    for (i = 0; i < max; i++) { log_ch(' '); log_hex(b[i]); }
    if (n > max) log_str(" ...");
    log_str("  |");
    for (i = 0; i < max; i++) log_ch((b[i] >= 32 && b[i] < 127) ? (char)b[i] : '.');
    log_str("|\r\n");
}

/* ------------------------------------------------------------------ */
/* Per-call context                                                     */

typedef struct {
    struct XPR_IO *io;
    void *fh;               /* host file handle */
    char *name;
    long  filesize;
    int   have_size;
    const char *hdrname;    /* Multi-Punter: header to repeat while waiting */
    int   hdrtype;
} ctx;

static long ms_to_us(long ms)           /* ms * 1000 without __mulsi3 */
{
    return (ms << 10) - (ms << 4) - (ms << 3);
}

static int g_pad;
static void mp_header(struct XPR_IO *io, const char *full, int ftype);
static void cb_beacon(void *u)
{
    ctx *c = (ctx *)u;
    if (c->hdrname) mp_header(c->io, c->hdrname, c->hdrtype);
}

static int cb_rd(void *u, unsigned char *b, long ms)
{
    ctx *c = (ctx *)u;
    long r = xcall((void *)c->io->xpr_sread, b, 0, 1, ms_to_us(ms));
    if (G.on) {
        if (r == 1) log_bytes("RX", b, 1);
        else { log_stamp(); log_str("RX timeout/none (rc="); log_dec(r); log_str(")\r\n"); }
    }
    return r == 1;
}

int g_pace, g_chunk, g_auto, g_level;                   /* output pacing (options D / K) */

static void *g_dos;                     /* dos.library base for Delay() */
static void dos_delay(int ticks)
{
    if (g_dos) amicall(g_dos, -198, ticks, ticks, 0, 0);   /* Delay(timeout in d1) */
}

static void pace_begin(const pt_opts *o)
{
    g_pace = o->pace; g_chunk = o->chunk < 1 ? 1 : o->chunk; g_dos = 0;
    g_auto = 0; g_level = 0;
    if (g_pace > 0 || o->multi) {
        void *sb = *(void *volatile *)(long)4;
        register void *a1 __asm__("a1") = (void *)"dos.library";
        register long d0 __asm__("d0") = 0;
        __asm__ volatile ("move.l %%a6,-(%%sp)\n\tmove.l %[b],%%a6\n\tjsr -552(%%a6)\n\tmove.l (%%sp)+,%%a6"
                          : "+r"(d0), "+r"(a1) : [b] "a"(sb) : "memory", "cc", "a0", "d1");
        g_dos = (void *)d0;
        if (!g_dos) { g_pace = 0; g_auto = 0; }
    }
}

static void pace_end(void)
{
    if (g_dos) {
        void *sb = *(void *volatile *)(long)4;
        register void *a1 __asm__("a1") = g_dos;
        __asm__ volatile ("move.l %%a6,-(%%sp)\n\tmove.l %[b],%%a6\n\tjsr -414(%%a6)\n\tmove.l (%%sp)+,%%a6"
                          : "+r"(a1) : [b] "a"(sb) : "memory", "cc", "d0", "d1", "a0");
    }
    g_dos = 0; g_pace = 0; g_auto = 0;
}

static int cb_wr(void *u, const unsigned char *b, int n)
{
    ctx *c = (ctx *)u;
    long r;
    if (g_pace > 0 && n > g_chunk) {
        int off = 0;
        r = 0;
        while (off < n && !r) {
            int m = n - off < g_chunk ? n - off : g_chunk;
            r = xcall((void *)c->io->xpr_swrite, (void *)(b + off), 0, m, 0);
            off += m;
            if (off < n) dos_delay(g_pace);
        }
        if (G.on) log_bytes("TX(paced)", b, n);
        return r != 0;
    }
    r = xcall((void *)c->io->xpr_swrite, (void *)b, 0, n, 0);
    if (G.on) { log_bytes("TX", b, n); if (r) { log_str("   swrite returned "); log_dec(r); log_str("\r\n"); } }
    return r != 0;
}

static void cb_purge(void *u)
{
    ctx *c = (ctx *)u;
    if (G.on) { log_stamp(); log_str("flush input\r\n"); }
    if (c->io->xpr_sflush) xcall((void *)c->io->xpr_sflush, 0, 0, 0, 0);
}

static int cb_aborted(void *u)
{
    ctx *c = (ctx *)u;
    if (c->io->xpr_chkmisc) xcall((void *)c->io->xpr_chkmisc, 0, 0, 0, 0);
    if (c->io->xpr_chkabort)
        return xcall((void *)c->io->xpr_chkabort, 0, 0, 0, 0) != 0;
    return 0;
}

static int cb_src_read(void *u, unsigned char *b, int n)
{
    ctx *c = (ctx *)u;
    /* xpr_fread(buf=A0, size=D0, count=D1, handle=A1) -> items read */
    return (int)xcall((void *)c->io->xpr_fread, b, c->fh, 1, n);
}

static int cb_dst_write(void *u, const unsigned char *b, int n)
{
    ctx *c = (ctx *)u;
    long r = xcall((void *)c->io->xpr_fwrite, (void *)b, c->fh, 1, n);
    return r == n ? 0 : -1;
}

static void cb_trace(void *u, const char *msg)
{
    (void)u;
    if (G.on) { log_stamp(); log_str("   ("); log_str(msg); log_str(")\r\n"); }
}

static const char protoname[] = "Punter C1";

static void cb_status(void *u, const char *msg, long bytes, long blocks,
                      long errors, long timeouts)
{
    ctx *c = (ctx *)u;
    struct XPR_UPDATE up;
    int i;
    char *z = (char *)&up;
    if (g_auto) {                       /* auto-pace: slower output as errors pile up */
        int lvl = errors >= 8 ? 4 : errors >= 6 ? 3 : errors >= 4 ? 2 : errors >= 2 ? 1 : 0;
        if (lvl > g_level) {
            g_level = lvl;
            g_pace = lvl == 4 ? 2 : 1;
            g_chunk = lvl == 1 ? 8 : lvl == 2 ? 4 : 1;
            if (G.on) { log_stamp(); log_str("AUTO-PACE level "); log_dec(lvl);
                        log_str(": pause="); log_dec(g_pace); log_str(" chunk="); log_dec(g_chunk); log_str("\r\n"); }
        }
    }
    if (G.on) { log_stamp(); log_str("STATUS: "); log_str(msg); log_str("  bytes="); log_dec(bytes);
                log_str(" blocks="); log_dec(blocks); log_str(" errors="); log_dec(errors);
                log_str(" timeouts="); log_dec(timeouts); log_str("\r\n"); }
    if (!c->io->xpr_update) return;
    for (i = 0; i < (int)sizeof up; i++) z[i] = 0;
    up.xpru_updatemask = XPRU_PROTOCOL | XPRU_FILENAME | XPRU_FILESIZE |
                         XPRU_MSG | XPRU_BLOCKS | XPRU_BLOCKSIZE |
                         XPRU_BYTES | XPRU_ERRORS | XPRU_TIMEOUTS;
    up.xpru_protocol  = (char *)protoname;
    up.xpru_filename  = c->name;
    up.xpru_filesize  = c->have_size ? c->filesize : -1L;
    up.xpru_msg       = (char *)msg;
    up.xpru_blocks    = blocks;
    up.xpru_blocksize = 255;
    up.xpru_bytes     = bytes;
    up.xpru_errors    = errors;
    up.xpru_timeouts  = timeouts;
    xcall((void *)c->io->xpr_update, &up, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Options are packed into xpr_data (an opaque, library-private slot):
 *   bit 31 valid | bits 27-29 pace | bit 26 log | bits 24-25 type |
 *   bits 16-23 blocksize | bits 10-15 chunk | bits 0-9 wait  */

static void load_opts(struct XPR_IO *io, pt_opts *o)
{
    unsigned long v = (unsigned long)io->xpr_data;
    pt_default_opts(o);
    o->ftype = PT_TYPE_AUTO;
    if (v & 0x80000000UL) {
        o->ftype      = (int)((v >> 24) & 3);
        o->log        = (int)((v >> 26) & 1);
        o->pace       = (int)((v >> 27) & 3);
        o->chunk      = (int)((v >> 10) & 63);
        o->multi      = (int)((v >> 30) & 1) ? (((v >> 29) & 1) ? 2 : 1) : 0;
        if (o->chunk < 1) o->chunk = 1;
        o->blocksize  = (int)((v >> 16) & 0xff);
        o->start_wait = (int)(v & 0x3ff);
    }
}

long xpr_setup_c(struct XPR_IO *io)
{
    pt_opts o;
    unsigned long v;
    pt_default_opts(&o);
    o.ftype = PT_TYPE_AUTO;
    pt_parse_opts(io->xpr_filename, &o);       /* init string, may be NULL */
    if (o.start_wait > 0x3ff) o.start_wait = 0x3ff;
    v = 0x80000000UL | ((unsigned long)(o.ftype & 3) << 24) |
        ((unsigned long)(o.log & 1) << 26) |
        ((unsigned long)(o.pace & 3) << 27) |
        ((unsigned long)(o.multi == 2 ? 1 : 0) << 29) |
        ((unsigned long)(o.multi ? 1 : 0) << 30) |
        ((unsigned long)(o.chunk & 63) << 10) |
        ((unsigned long)(o.blocksize & 0xff) << 16) |
        (unsigned long)(o.start_wait & 0x3ff);
    io->xpr_data = (long *)v;
    return 1;
}

long xpr_cleanup_c(struct XPR_IO *io)
{
    io->xpr_data = 0;
    return 1;
}

/* ------------------------------------------------------------------ */

static int ext_is(const char *name, const char *ext)  /* case-insens suffix */
{
    int ln = 0, le = 0, i;
    while (name[ln]) ln++;
    while (ext[le]) le++;
    if (ln < le) return 0;
    for (i = 0; i < le; i++) {
        char a = name[ln - le + i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (a != b) return 0;
    }
    return 1;
}

static int auto_type(const char *name)
{
    static const char *const seq[] = { ".txt", ".seq", ".asc", ".nfo", ".diz",
                                       ".doc", ".bbs", ".lst", ".s", ".asm",
                                       ".readme", ".guide", ".info", 0 };
    int i;
    for (i = 0; seq[i]; i++) if (ext_is(name, seq[i])) return PT_TYPE_SEQ;
    return PT_TYPE_PRG;
}

static unsigned char petup(unsigned char ch)
{
    if (ch >= 'a' && ch <= 'z') return (unsigned char)(ch - 32);
    return ch;
}

/* send one file (already named); log is managed by the caller */
static int send_one(struct XPR_IO *io, char *name, const pt_opts *o, int ftype, int mp)
{
    ctx c;
    pt_io pio;
    int rc;

    c.io = io; c.name = name; c.fh = 0; c.have_size = 0; c.filesize = 0;
    c.hdrname = 0; c.hdrtype = 0;
    c.fh = (void *)xcall((void *)io->xpr_fopen, c.name, (void *)"r", 0, 0);
    if (!c.fh) { if (G.on) log_str("fopen failed\r\n"); return 0; }
    if (io->xpr_finfo) {
        long sz = xcall((void *)io->xpr_finfo, c.name, 0, 1, 0);
        if (sz > 0) { c.filesize = sz; c.have_size = 1; }
    }
    if (ftype == PT_TYPE_AUTO) ftype = auto_type(c.name);
    if (mp) { c.hdrname = c.name; c.hdrtype = ftype; }

    pio.u = &c; pio.rd = cb_rd; pio.wr = cb_wr; pio.purge = cb_purge;
    pio.aborted = cb_aborted; pio.src_read = cb_src_read;
    pio.dst_write = 0; pio.status = cb_status; pio.trace = cb_trace;
    pio.beacon = cb_beacon;
    if (G.on) {
        log_str("SEND file="); log_str(c.name); log_str(" size="); log_dec(c.have_size ? c.filesize : -1L);
        log_str(" ftype="); log_dec(ftype); log_str(" blocksize="); log_dec(o->blocksize);
        log_str(" wait="); log_dec(o->start_wait); log_str(" pace="); log_dec(o->pace); log_str(" chunk="); log_dec(o->chunk); log_str("\r\n");
    }
    rc = pt_send(&pio, o, ftype);
    xcall((void *)io->xpr_fclose, c.fh, 0, 0, 0);
    if (G.on) log_str(rc == PT_OK ? "RESULT: success\r\n" : "RESULT: FAILED\r\n");
    return rc == PT_OK;
}

/* Multi-Punter header: 16 TAB, NAME,P|S, CR  (CCGMS framing) */
static void mp_header(struct XPR_IO *io, const char *full, int ftype)
{
    ctx c;
    unsigned char h[16 + 4 + 20 + 4];
    const char *base = full, *s;
    int n = 0, i, ln = 0;
    for (s = full; *s; s++) if (*s == ':' || *s == '/') base = s + 1;
    c.io = io;
    for (i = 0; i < 16; i++) h[n++] = 9;
    if (g_pad) for (i = 0; i < 4; i++) h[n++] = 0;     /* sacrificial: CCGMS skips NULs */
    while (base[ln] && ln < 16) { h[n++] = petup((unsigned char)base[ln]); ln++; }
    h[n++] = ',';
    h[n++] = (ftype == PT_TYPE_SEQ) ? 'S' : 'P';
    h[n++] = 13;
    cb_wr(&c, h, n);
}

static void mp_end(struct XPR_IO *io)
{
    ctx c;
    unsigned char h[33];
    int i, n = 0;
    c.io = io;
    for (i = 0; i < 16; i++) h[n++] = 9;
    for (i = 0; i < 16; i++) h[n++] = 4;
    h[n++] = 13;
    cb_wr(&c, h, n);
}

long xpr_send_c(struct XPR_IO *io)
{
    pt_opts o;
    int ok = 0;

    if (!io->xpr_filename) return 0;
    load_opts(io, &o);
    pace_begin(&o);
    if (o.log) log_begin();
    g_pad = (o.multi == 2);
    if (G.on) {
        log_str("CALL Send: xpr_ffirst="); log_str(io->xpr_ffirst ? "yes" : "no");
        log_str(" xpr_fnext="); log_str(io->xpr_fnext ? "yes" : "no");
        log_str(" multi="); log_dec(o.multi); log_str("\r\n");
    }

    if (o.multi && io->xpr_ffirst && io->xpr_fnext) {
        char buf[256];
        char *pat = io->xpr_filename;
        long st;
        int n = 0, good = 1, ft;
        st = xcall((void *)io->xpr_ffirst, buf, pat, 0, 0);
        if (G.on) { log_str("ffirst -> "); log_dec(st); log_str(" name="); log_str(st ? buf : "(none)"); log_str("\r\n"); }
        while (st && good) {
            ft = o.ftype == PT_TYPE_AUTO ? auto_type(buf) : o.ftype;
            mp_header(io, buf, ft);
            dos_delay(25);
            good = send_one(io, buf, &o, ft, 1);
            if (!good) break;
            n++;
            dos_delay(100);
            st = xcall((void *)io->xpr_fnext, buf, pat, st, 0);
            if (G.on) { log_str("fnext -> "); log_dec(st); log_str(" name="); log_str(st ? buf : "(none)"); log_str("\r\n"); }
        }
        if (n > 0 && good) mp_end(io);
        ok = good && n > 0;
        if (G.on) { log_str("BATCH files="); log_dec(n); log_str(ok ? " ok\r\n" : " FAILED\r\n"); }
    } else {
        ok = send_one(io, io->xpr_filename, &o, o.ftype, 0);
    }
    if (G.on) log_end();
    pace_end();
    return ok ? 1 : 0;
}

long xpr_receive_c(struct XPR_IO *io)
{
    ctx c;
    pt_io pio;
    pt_opts o;
    int rc, ft = 0;

    c.io = io; c.name = io->xpr_filename; c.fh = 0; c.have_size = 0; c.filesize = 0;
    if (!c.name) return 0;
    load_opts(io, &o);
    pace_begin(&o);
    if (o.log) log_begin();

    c.fh = (void *)xcall((void *)io->xpr_fopen, c.name, (void *)"w", 0, 0);
    if (!c.fh) { if (G.on) { log_str("fopen failed\r\n"); log_end(); } pace_end(); return 0; }

    pio.u = &c; pio.rd = cb_rd; pio.wr = cb_wr; pio.purge = cb_purge;
    pio.aborted = cb_aborted; pio.src_read = 0;
    pio.dst_write = cb_dst_write; pio.status = cb_status; pio.trace = cb_trace;
    pio.beacon = 0;
    if (G.on) {
        log_str("CALL Receive: xpr_ffirst="); log_str(io->xpr_ffirst ? "yes" : "no");
        log_str(" xpr_fnext="); log_str(io->xpr_fnext ? "yes" : "no"); log_str("\r\n");
        log_str("RECEIVE file="); log_str(c.name); log_str(" blocksize="); log_dec(o.blocksize);
        log_str(" wait="); log_dec(o.start_wait); log_str(" pace="); log_dec(o.pace); log_str(" chunk="); log_dec(o.chunk); log_str("\r\n");
    }

    rc = pt_receive(&pio, &o, &ft);
    xcall((void *)io->xpr_fclose, c.fh, 0, 0, 0);
    if (G.on) { log_str(rc == PT_OK ? "RESULT: success\r\n" : "RESULT: FAILED\r\n"); log_end(); }
    pace_end();
    return rc == PT_OK ? 1 : 0;
}

/* gcc may emit calls to these even in freestanding mode */
void *memcpy(void *d, const void *s, unsigned long n)
{
    unsigned char *dd = d; const unsigned char *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
void *memset(void *d, int v, unsigned long n)
{
    unsigned char *dd = d;
    while (n--) *dd++ = (unsigned char)v;
    return d;
}
