import sys; import os; sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from hunks import parse
import machine68k as M, struct
def run(path):
    d=open(path,'rb').read(); sizes,segs=parse(d)
    m=M.Machine(M.CPUType.M68020,1024); cpu=m.cpu; mem=m.mem
    bases=[0x10000,0x30000,0x40000]
    for i,s in enumerate(segs):
        if s['data']: mem.w_block(bases[i],s['data'])
    for i,s in enumerate(segs):
        for h,offs in s['relocs'].items():
            for o in offs: mem.w32(bases[i]+o,(mem.r32(bases[i]+o)+bases[h])&0xffffffff)
    code=bases[2]
    LIB=0x80000
    for lvo in range(6,0x200,6): mem.w16(LIB-lvo,0x4e75)   # every LVO: rts
    A5=0x90000; mem.w32(A5+0x1a,LIB); mem.w32(A5+0x46,0x91000)
    SP=0xa0000; calls=[]; stop=code+0x1fc4
    state={}
    def hook(pc):
        if 'regs' in state: return
        if LIB-0x200<pc<LIB:
            r=[cpu.r_reg(i) for i in range(16)]
            calls.append((LIB-pc, r[0], r[1], r[2], r[3], mem.r32(r[8]), r[14]))
        if pc==stop:
            state['regs']=[cpu.r_reg(i) for i in range(16)]
    # simpler: put STOP-like trap: write 'illegal' at stop and catch exception? use hook polling
    cpu.set_instr_hook_callback(hook)
    for i in range(16): cpu.w_reg(i,0)
    cpu.w_reg(7,5)
    cpu.w_reg(6,0); cpu.w_reg(13,A5); cpu.w_reg(14,0xdead0000); cpu.w_reg(15,SP)
    mem.w8(SP+0x31,2)
    cpu.w_pc(code+0x1f76)
    for _ in range(100):
        cpu.execute(10)
        if 'regs' in state: break
    return calls,state.get('regs')
for p in sys.argv[1:]:
    calls,regs=run(p)
    print(p)
    for c in calls: print("  LVO -%d  d0=%d d1=%#x d2=%d d3=%d *a0=%d a6=%#x"%c)
    print("  at 0x1fc4 (gethostbyaddr): d0(len)=%d d1(type)=%d d2=%d d3=%d sp=%#x"%(regs[0],regs[1],regs[2],regs[3],regs[15]) if regs else "  never reached 0x1fc4")
