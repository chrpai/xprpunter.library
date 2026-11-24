/* Native test-suite for the Punter engine: socketpair + fork, scaled time,
 * optional line noise.  Build: see run_native.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <signal.h>
#include "../src/punter.h"
#include "ref_peer.h"

#define TS 60              /* time scale: engine ms / TS */
static int fd;
static unsigned noise_den;     /* 0 = no noise; else 1/noise_den per byte */
static unsigned rng = 12345;
static unsigned rnd(void){ rng = rng*1103515245u + 12345u; return (rng>>8); }
static long wbytes;

static int rd_ms(unsigned char *b, long ms)
{
    struct pollfd p = { fd, POLLIN, 0 };
    long t = ms / TS; if (t < 1) t = 1;
    if (poll(&p, 1, (int)t) <= 0) return 0;
    return read(fd, b, 1) == 1;
}
static void wr_byte(unsigned char c)
{
    if (noise_den) {
        unsigned r = rnd() % noise_den;
        if (r == 0) return;                    /* drop */
        if (r == 1) c ^= (unsigned char)(1u << (rnd() & 7));  /* flip bit */
    }
    wbytes++;
    if (write(fd, &c, 1) != 1) { /* peer gone */ }
}

/* --- pt_io for the engine under test --- */
static unsigned char *src; static long srclen, srcpos;
static unsigned char *dst; static long dstlen;
static int io_rd(void *u, unsigned char *b, long ms){ (void)u; return rd_ms(b, ms); }
static int io_wr(void *u, const unsigned char *b, int n){ (void)u; for(int i=0;i<n;i++) wr_byte(b[i]); return 0; }
static void io_purge(void *u){ (void)u; unsigned char c; struct pollfd p={fd,POLLIN,0}; while(poll(&p,1,0)>0) if(read(fd,&c,1)!=1) break; }
static int io_src(void *u, unsigned char *b, int n){ (void)u; long r=srclen-srcpos; if(r>n) r=n; memcpy(b,src+srcpos,r); srcpos+=r; return (int)r; }
static int io_dst(void *u, const unsigned char *b, int n){ (void)u; memcpy(dst+dstlen,b,n); dstlen+=n; return 0; }
static int verbose;
static void io_trace(void *u, const char *m){ (void)u; if(verbose) fprintf(stderr,"  [trace] %s\n",m); }
static pt_io myio = { 0, io_rd, io_wr, io_purge, 0, io_src, io_dst, 0, io_trace, 0 };

/* --- reference-peer glue --- */
static int ref_in(int t){ unsigned char c; return rd_ms(&c,t) ? c : -1; }
static void ref_out(int c){ wr_byte((unsigned char)c); }
static void ref_sleep(int s){ usleep((useconds_t)(s*1000000L/TS)); io_purge(0); }

static unsigned char *mk(long n, unsigned seed)
{
    unsigned char *p = malloc(n ? n : 1);
    unsigned s = seed;
    for (long i = 0; i < n; i++) { s = s*1664525u + 1013904223u; p[i] = (unsigned char)(s >> 24); }
    /* sprinkle token-like sequences to try to confuse the sliding window */
    if (n > 40) { memcpy(p+10,"GOOBADACKS/BSYN",15); memcpy(p+30,"ACKACK",6); }
    return p;
}

enum { MINE_SEND_REF_RECV, REF_SEND_MINE_RECV, MINE_MINE };

static int run(int mode, long len, unsigned noise, int ftype, const char *name)
{
    int sv[2], status = 0, ok = 0;
    unsigned char *data = mk(len, (unsigned)len*7+1);
    noise_den = 0; wbytes = 0;
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    pid_t pid = fork();
    if (pid == 0) {                         /* peer (child) */
        fd = sv[1]; close(sv[0]);
        noise_den = noise; rng = 777;
        pt_opts o; pt_default_opts(&o); o.start_wait = 30;
        int rc = 0;
        if (mode == MINE_SEND_REF_RECV) {
            unsigned char *out = malloc(len + 4096); int ol = 0;
            rc = ref_recv(ref_in, ref_out, ref_sleep, out, &ol);
            if (rc && ol == len && memcmp(out, data, len) == 0) _exit(0);
            if (verbose) fprintf(stderr,"  ref_recv rc=%d got %d expected %ld\n", rc, ol, len);
            _exit(rc ? 2 : 1);
        } else if (mode == REF_SEND_MINE_RECV) {
            rc = ref_xmit(ref_in, ref_out, ref_sleep, data, (int)len);
            _exit(rc ? 0 : 1);
        } else {                            /* MINE_MINE: child = sender */
            src = data; srclen = len; srcpos = 0;
            rc = pt_send(&myio, &o, ftype);
            _exit(rc == PT_OK ? 0 : 1);
        }
    }
    fd = sv[0]; close(sv[1]);
    noise_den = noise; rng = 4242;
    pt_opts o; pt_default_opts(&o); o.start_wait = 30;
    dst = malloc(len + 8192); dstlen = 0;
    if (mode == MINE_SEND_REF_RECV) {
        src = data; srclen = len; srcpos = 0;
        int rc = pt_send(&myio, &o, ftype);
        waitpid(pid, &status, 0);
        ok = (rc == PT_OK) && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    } else {
        int ft = 0;
        int rc = pt_receive(&myio, &o, &ft);
        waitpid(pid, &status, 0);
        ok = (rc == PT_OK) && dstlen == len && memcmp(dst, data, len) == 0 &&
             (mode != MINE_MINE || ft == ftype);
        if (!ok && verbose) fprintf(stderr,"  rc=%d got %ld expected %ld ft=%d\n", rc, dstlen, len, ft);
    }
    printf("%-28s len=%-6ld noise=%-5u %s\n", name, len, noise, ok ? "PASS" : "FAIL");
    close(fd); free(dst); free(data);
    return ok;
}

int main(int argc, char **argv)
{
    int fails = 0;
    signal(SIGPIPE, SIG_IGN);
    if (argc > 1 && !strcmp(argv[1], "-v")) verbose = 1;
    /* 249 (last block = 1 payload byte) omitted: the reference receiver never saves payloads of blocks <= 8 bytes (upstream quirk) */
    static const long sizes_ref_recv[] = { 0, 5, 241, 248, 250, 496, 1000, 5000 };
    static const long sizes_all[] = { 0, 1, 5, 247, 248, 249, 496, 497, 1000, 5000 };
    for (unsigned i = 0; i < sizeof sizes_ref_recv/sizeof *sizes_ref_recv; i++)
        fails += !run(MINE_SEND_REF_RECV, sizes_ref_recv[i], 0, PT_TYPE_PRG, "mine->ref (clean)");
    for (unsigned i = 0; i < sizeof sizes_all/sizeof *sizes_all; i++)
        fails += !run(REF_SEND_MINE_RECV, sizes_all[i], 0, 0, "ref->mine (clean)");
    for (unsigned i = 0; i < sizeof sizes_all/sizeof *sizes_all; i++)
        fails += !run(MINE_MINE, sizes_all[i], 0, (i & 1) ? PT_TYPE_SEQ : PT_TYPE_PRG, "mine<->mine (clean)");
    /* noisy line: sender->receiver direction in child, receiver->sender in parent */
    static const unsigned noises[] = { 3000, 1200, 600 };
    for (unsigned n = 0; n < 3; n++)
        for (unsigned i = 4; i < sizeof sizes_all/sizeof *sizes_all; i++)
            fails += !run(MINE_MINE, sizes_all[i], noises[n], PT_TYPE_PRG, "mine<->mine (noisy)");
    for (unsigned n = 0; n < 2; n++)
        fails += !run(MINE_SEND_REF_RECV, 1000, noises[n], PT_TYPE_PRG, "mine->ref (noisy)");
    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED", fails, fails==1?"":"s");
    return fails != 0;
}
