import struct,sys
def be(d,o): return struct.unpack('>I',d[o:o+4])[0]
def parse(d):
    o=0; assert be(d,0)==0x3f3; o=4
    while be(d,o): n=be(d,o); o+=4+4*n
    o+=4; tsize=be(d,o); first=be(d,o+4); last=be(d,o+8); o+=12
    sizes=[be(d,o+4*i)&0x3fffffff for i in range(tsize)]; o+=4*tsize
    segs=[]; cur=None
    while o<len(d):
        t=be(d,o)&0x3fffffff; o+=4
        if t in (0x3e9,0x3ea):
            n=be(d,o); o+=4; cur={'type':t,'hdr':o-8,'off':o,'n':n,'data':d[o:o+4*n],'relocs':{}}; segs.append(cur); o+=4*n
        elif t==0x3eb:
            n=be(d,o); o+=4; cur={'type':t,'hdr':o-8,'off':o,'n':n,'data':b'','relocs':{}}; segs.append(cur)
        elif t==0x3ec:
            cur['reloc_off']=o-4
            while True:
                n=be(d,o); o+=4
                if n==0: break
                h=be(d,o); o+=4
                cur['relocs'].setdefault(h,[]).extend(be(d,o+4*i) for i in range(n)); o+=4*n
            cur['reloc_end']=o
        elif t==0x3f2: cur['end']=o
        else: raise Exception(hex(t))
    return sizes,segs
