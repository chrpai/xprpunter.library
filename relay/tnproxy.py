#!/usr/bin/env python3
"""
tnproxy - raw TCP  <->  telnet relay, for 8-bit file transfers.

The C64 Ultimate's modem emulation (and terminals such as CCGMS) talk a RAW
byte stream.  telnetd.device on the Amiga talks TELNET, where byte 0xFF is an
escape (IAC) and must travel as 0xFF 0xFF.  Through a plain connection every
Punter/Xmodem block containing 0xFF is corrupted.

This relay sits in between (run it on the PC that hosts WinUAE):

   C64U modem --raw--> tnproxy --telnet--> telnetd.device (Amiga)

 C64 -> Amiga : every 0xFF byte is doubled.
 Amiga -> C64 : telnet commands are answered/removed, 0xFF 0xFF becomes 0xFF.
 It negotiates BINARY and SGA, refuses every other option.

Usage:   python tnproxy.py  --listen 2323  --target 127.0.0.1:5432
Then on the C64:  ATDT <pc-ip>:2323
Python 3.7+.  No other packages needed.
"""
import argparse, asyncio, sys, time

IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240
BINARY, ECHO, SGA = 0, 1, 3
ACCEPT_LOCAL = {BINARY, SGA}            # options we agree to enable ("WILL")
ACCEPT_REMOTE = {BINARY, SGA, ECHO}     # options we ask/allow the server to enable


class Telnet:
    """Incremental telnet decoder for the server -> client direction."""
    def __init__(self, send):
        self.send = send                  # callback taking bytes (to server)
        self.state = 0                    # 0 data, 1 IAC, 2 cmd+opt, 3 SB, 4 SB-IAC
        self.cmd = 0
        self.we = {}                      # options we have enabled
        self.they = {}                    # options the server has enabled

    def offer(self):
        out = bytearray()
        for o in (BINARY, SGA):
            self.we[o] = True; self.they[o] = True
            out += bytes((IAC, WILL, o, IAC, DO, o))
        self.send(bytes(out))

    def feed(self, data):
        out = bytearray()
        for b in data:
            s = self.state
            if s == 0:
                if b == IAC: self.state = 1
                else: out.append(b)
            elif s == 1:
                if b == IAC: out.append(255); self.state = 0
                elif b in (DO, DONT, WILL, WONT): self.cmd = b; self.state = 2
                elif b == SB: self.state = 3
                else: self.state = 0               # NOP, BRK, AYT ... dropped
            elif s == 2:
                self._option(self.cmd, b); self.state = 0
            elif s == 3:
                if b == IAC: self.state = 4        # subnegotiation ignored
            elif s == 4:
                self.state = 0 if b == SE else 3
        return bytes(out)

    def _option(self, cmd, opt):
        if cmd == DO:                              # server wants us to enable opt
            want = opt in ACCEPT_LOCAL
            if self.we.get(opt, False) != want:
                self.we[opt] = want
                self.send(bytes((IAC, WILL if want else WONT, opt)))
            elif not want and opt not in self.we:
                self.we[opt] = False
                self.send(bytes((IAC, WONT, opt)))
        elif cmd == DONT:
            if self.we.get(opt, False):
                self.we[opt] = False
                self.send(bytes((IAC, WONT, opt)))
        elif cmd == WILL:                          # server offers to enable opt
            want = opt in ACCEPT_REMOTE
            if self.they.get(opt, False) != want:
                self.they[opt] = want
                self.send(bytes((IAC, DO if want else DONT, opt)))
            elif not want and opt not in self.they:
                self.they[opt] = False
                self.send(bytes((IAC, DONT, opt)))
        elif cmd == WONT:
            if self.they.get(opt, False):
                self.they[opt] = False
                self.send(bytes((IAC, DONT, opt)))


def log(msg):
    t = time.time()
    print(time.strftime("%H:%M:%S", time.localtime(t)) + ".%03d" % int((t % 1) * 1000), msg, flush=True)


def hexs(b, n=24):
    return " ".join("%02X" % x for x in b[:n]) + (" ..." if len(b) > n else "")


async def handle(creader, cwriter, target, verbose, gather=0):
    peer = cwriter.get_extra_info("peername")
    log(f"C64 side connected from {peer}; connecting to {target[0]}:{target[1]}")
    try:
        sreader, swriter = await asyncio.open_connection(*target)
    except OSError as e:
        log(f"cannot reach target: {e}")
        cwriter.write(b"\r\nNO CARRIER\r\n"); await cwriter.drain(); cwriter.close(); return

    tn = Telnet(lambda b: swriter.write(b))
    tn.offer()
    counts = {"up": 0, "down": 0, "ff_up": 0, "ff_down": 0}

    async def up():                                # C64 -> Amiga: double FF
        while True:
            d = await creader.read(4096)
            if not d: break
            counts["up"] += len(d); n = d.count(b"\xff"); counts["ff_up"] += n
            swriter.write(d.replace(b"\xff", b"\xff\xff"))
            await swriter.drain()
            if verbose: log(f"C64 -> Amiga {len(d):4d} bytes: {hexs(d)}")

    async def down():                              # Amiga -> C64: decode telnet
        while True:
            d = await sreader.read(4096)
            if not d: break
            out = tn.feed(d)
            if verbose and out: log(f"Amiga -> relay {len(d):4d} wire bytes")
            if gather and out:                  # coalesce: wait until the line is quiet
                while True:
                    try:
                        more = await asyncio.wait_for(sreader.read(4096), gather / 1000.0)
                    except asyncio.TimeoutError:
                        break
                    if not more: break
                    out += tn.feed(more)
            await swriter.drain()
            if out:
                counts["down"] += len(out); n = out.count(b"\xff"); counts["ff_down"] += n
                cwriter.write(out); await cwriter.drain()
                if verbose: log(f"relay -> C64   {len(out):4d} bytes: {hexs(out)}")

    tasks = [asyncio.ensure_future(up()), asyncio.ensure_future(down())]
    try:
        await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
    finally:
        for t in tasks: t.cancel()
        for w in (cwriter, swriter):
            try: w.close()
            except Exception: pass
        log("connection closed: " + ", ".join(f"{k}={v}" for k, v in counts.items()))


async def main():
    ap = argparse.ArgumentParser(description="raw TCP <-> telnet relay (8-bit clean)")
    ap.add_argument("--listen", type=int, default=2323, help="port the C64 dials (default 2323)")
    ap.add_argument("--bind", default="0.0.0.0", help="address to listen on")
    ap.add_argument("--target", required=True, help="host:port of telnetd.device, e.g. 127.0.0.1:5432")
    ap.add_argument("--gather", type=int, default=0, metavar="MS",
                    help="hold data for the C64 until the Amiga has been quiet for MS ms, then send it in one piece (try 15)")
    ap.add_argument("-v", "--verbose", action="store_true", help="log every chunk")
    a = ap.parse_args()
    host, _, port = a.target.rpartition(":")
    target = (host or "127.0.0.1", int(port))
    srv = await asyncio.start_server(lambda r, w: handle(r, w, target, a.verbose, a.gather), a.bind, a.listen)
    log(f"listening on {a.bind}:{a.listen}  ->  telnet {target[0]}:{target[1]}")
    async with srv:
        await srv.serve_forever()

if __name__ == "__main__":
    try: asyncio.run(main())
    except KeyboardInterrupt: pass
