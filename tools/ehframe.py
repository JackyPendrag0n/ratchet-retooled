"""Parse .eh_frame (0x83a50c) and LSDAs of the pinned EBOOT to list landing pads."""
import struct,sys
d=None
BASE=0x10000
def rd(a,n): o=a-BASE; return d[o:o+n]
def u32(a): return struct.unpack('>I',rd(a,4))[0]
def uleb(a):
    r=s=0
    while True:
        b=rd(a,1)[0]; a+=1; r|=(b&0x7f)<<s; s+=7
        if b<0x80: return r,a
def sleb(a):
    r=s=0
    while True:
        b=rd(a,1)[0]; a+=1; r|=(b&0x7f)<<s; s+=7
        if b<0x80:
            if b&0x40: r-=1<<s
            return r,a
def enc(a,e):
    if e==0xff: return None,a
    fmt=e&0x0f; app=e&0x70; base=a
    if fmt==0x00 or fmt==0x0b or fmt==0x03: v=u32(a); n=4
    elif fmt==0x04: v=struct.unpack('>Q',rd(a,8))[0]; n=8
    elif fmt==0x01: v,a2=uleb(a); n=a2-a
    elif fmt==0x02: v=struct.unpack('>H',rd(a,2))[0]; n=2
    else: raise ValueError(hex(e))
    if fmt in (0x0b,) and v&0x80000000: v-=1<<32
    if app==0x10: v=(base+v)&0xffffffff
    return v,a+n
def landing_pads(data):
    """Return (fdes, pads): pads are (function_pc, landing_pad) pairs."""
    global d
    d = data
    lo,hi=0x83a50c,0x83a50c+0x1acc
    a=lo; cies={}; fdes=[]
    while a<hi:
        ln=u32(a)
        if ln==0: break
        cid=u32(a+4); body=a+8; end=a+4+ln
        if cid==0:
            p=body+1; aug=b''
            while rd(p,1)!=b'\0': aug+=rd(p,1); p+=1
            p+=1; _,p=uleb(p); _,p=sleb(p); _,p=uleb(p)
            info=dict(aug=aug.decode(),fde_enc=0,lsda_enc=0xff)
            if aug.startswith(b'z'):
                _,p=uleb(p)
                for ch in aug[1:]:
                    if ch==ord('R'): info['fde_enc']=rd(p,1)[0]; p+=1
                    elif ch==ord('L'): info['lsda_enc']=rd(p,1)[0]; p+=1
                    elif ch==ord('P'):
                        pe=rd(p,1)[0]; p+=1; _,p=enc(p,pe)
            cies[a]=info
        else:
            cie=cies[a+4-cid]
            pc,p=enc(body,cie['fde_enc']); rng,p=enc(p,cie['fde_enc']&0x0f)
            lsda=None
            if cie['aug'].startswith('z'):
                al,p2=uleb(p)
                if cie['lsda_enc']!=0xff and al and int.from_bytes(rd(p2, 8 if cie['lsda_enc']&0x0f==4 else 4),'big'): lsda,_=enc(p2,cie['lsda_enc'])
            fdes.append((pc,rng,lsda))
        a=end
    pads=[]
    for pc,rng,lsda in fdes:
        if not lsda: continue
        p=lsda; lpe=rd(p,1)[0]; p+=1
        lpstart=pc
        if lpe!=0xff: lpstart,p=enc(p,lpe)
        tte=rd(p,1)[0]; p+=1
        if tte!=0xff: _,p=uleb(p)
        cse=rd(p,1)[0]; p+=1
        cl,p=uleb(p); end=p+cl
        while p<end:
            st,p=enc(p,cse); ln,p=enc(p,cse); lp,p=enc(p,cse); act,p=uleb(p)
            if lp: pads.append((pc,lpstart+lp))

    return fdes, pads


if __name__ == '__main__':
    fdes, pads = landing_pads(open(sys.argv[1], 'rb').read())
    print('FDEs', len(fdes), 'with LSDA', sum(1 for f in fdes if f[2]), 'landing pads', len(pads))
    for f, lp in pads:
        print('%x %x' % (f, lp))
