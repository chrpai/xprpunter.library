"""Run telnetd.device's telnet-mode send hook on a 68020 core (machine68k) with a
stub bsdsocket base, and compare the original (0x3060) with the patched one
(the address the hook at 0x1486 points to). Usage: emutest_iac.py orig patched"""
import os, sys, random, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hunks import parse
import machine68k as M

def load(path):
    d = open(path, 'rb').read(); sizes, segs = parse(d)
    m = M.Machine(M.CPUType.M68020, 1024); mem = m.mem
    bases = [0x10000, 0x30000, 0x40000]
    for i, s in enumerate(segs):
        if s['data']: mem.w_block(bases[i], s['data'])
    for i, s in enumerate(segs):
        for h, offs in s['relocs'].items():
            for o in offs: mem.w32(bases[i]+o, (mem.r32(bases[i]+o)+bases[h]) & 0xffffffff)
    return m, bases[2]

def hook_target(m, code):
    # decode lea d16(pc),a0 at 0x1486
    return code + 0x1488 + struct.unpack('>h', bytes([m.mem.r8(code+0x1488), m.mem.r8(code+0x1489)]))[0]

def run(m, func, data):
    cpu, mem = m.cpu, m.mem
    LIB, BUF, CNT, SP, RET = 0x80000, 0x88000, 0x8f000, 0xa0000, 0x9f000
    mem.w16(LIB-0x42, 0x4e75); mem.w16(RET, 0x4e71)
    mem.w_block(BUF, data); mem.w32(CNT, 0)
    sends = []; done = {}
    def hook(pc):
        if done: return
        if pc == LIB-0x42:
            a0, d1 = cpu.r_reg(8), cpu.r_reg(1)
            sends.append((cpu.r_reg(0), cpu.r_reg(2), bytes(mem.r_block(a0, d1)) if d1 else b''))
        if pc == RET: done['r'] = [cpu.r_reg(i) for i in range(16)]
    cpu.set_instr_hook_callback(hook)
    regs = [0x11111111*i & 0xffffffff for i in range(16)]
    regs[0], regs[1], regs[8], regs[9], regs[14] = 7, len(data), LIB, BUF, 0xdead0000
    for i in range(15): cpu.w_reg(i, regs[i])
    sp = SP - 8; mem.w32(sp, RET); mem.w32(sp+4, CNT); cpu.w_reg(15, sp)
    cpu.w_pc(func)
    for _ in range(200000):
        cpu.execute(50)
        if done: break
    r = done['r']
    saved = all(r[i] == regs[i] for i in (2,3,4,5,6,7,10,11,12,13,14)) and r[15] == sp + 4
    return sends, mem.r32(CNT), saved

# Musashi has one global CPU and memory, so reload the file before every run
def run_file(path, data):
    m, code = load(path)
    func = hook_target(m, code)
    sends, cnt, saved = run(m, func, data)
    m.cleanup()
    return func - code, sends, cnt, saved
random.seed(1)
cases = [b'', b'A', b'\xff', b'\xff\xff', b'hello', b'\x95\x75\xcc\x19\xff\x01\x00' + bytes(random.randrange(256) for _ in range(248)),
         b'\xff'*300, bytes(random.randrange(256) for _ in range(1100)), b'x'*509 + b'\xff' + b'yz']
ok = True
for c in cases:
    fo, so, co, svo = run_file(sys.argv[1], c)
    fn, sn, cn, svn = run_file(sys.argv[2], c)
    assert fo == 0x3060 and fn != 0x3060, (hex(fo), hex(fn))
    so_s = b''.join(x[2] for x in so); sn_s = b''.join(x[2] for x in sn)
    exp = c.replace(b'\xff', b'\xff\xff')
    good = so_s == exp and sn_s == exp and co == cn == len(exp) and svn and all(x[0] == 7 and x[1] == 0 for x in sn)
    ok &= good
    print("len %4d  FFs %3d  original: %3d send()s  patched: %d send()s  stream+counter match: %s  regs/stack kept: %s"
          % (len(c), c.count(255), len(so), len(sn), so_s == sn_s == exp and co == cn, svn))
print("ALL OK" if ok else "MISMATCH")
