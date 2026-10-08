#!/usr/bin/env python3
# Exports a KiriKiri CxDec scheme from GARbro's game database (ArcFormats/Resources/Formats.dat,
# MIT licence, https://github.com/crskycode/GARbro) as a Shiori scheme file for the engine
# (AETHERKIRI_CX_SCHEME; layout in AetherKrkr's XP3ArchiveCxDecoder.cpp, patch 0015).
#
# usage: garbro-cx.py <Formats.dat> "<GARbro title>" <out.shcx>
#        garbro-cx.py <Formats.dat> --list [filter]
# Scheme files hold key material for commercial titles: keep them out of public packages.
import sys,zlib,struct
class R:
    def __init__(s,d): s.d=d; s.p=0
    def u8(s): v=s.d[s.p]; s.p+=1; return v
    def i32(s): v=struct.unpack_from('<i',s.d,s.p)[0]; s.p+=4; return v
    def u32(s): v=struct.unpack_from('<I',s.d,s.p)[0]; s.p+=4; return v
    def lp(s):
        n=0; sh=0
        while True:
            b=s.u8(); n|=(b&0x7f)<<sh; sh+=7
            if not b&0x80: break
        v=s.d[s.p:s.p+n].decode('utf-8','replace'); s.p+=n; return v
PRIM={1:('?',1),2:('B',1),3:('c',2),5:('d',16),6:('d',8),7:('h',2),8:('i',4),9:('q',8),10:('b',1),11:('f',4),12:('q',8),13:('q',8),14:('H',2),15:('I',4),16:('Q',8),18:('s',0)}
def prim(r,t):
    if t==18: return r.lp()
    if t==1: return bool(r.u8())
    if t==3: # char utf8
        b=r.u8(); return chr(b)
    if t==5: return r.lp()
    fmt,n=PRIM[t]; v=struct.unpack_from('<'+fmt,r.d,r.p)[0]; r.p+=n; return v
objs={}; classes={}
class Ref:
    def __init__(s,i): s.i=i
    def __repr__(s): return '@%d'%s.i
def read_values(r, cls):
    name, members, btypes, addinfo = cls
    vals={}
    for m,bt,ai in zip(members,btypes,addinfo):
        if bt==0: vals[m]=prim(r,ai)
        else: vals[m]=read_record(r)
    return vals
def read_types(r,count):
    bts=[r.u8() for _ in range(count)]; ai=[]
    for bt in bts:
        if bt==0 or bt==7: ai.append(r.u8())
        elif bt==3: ai.append(r.lp())
        elif bt==4: ai.append((r.lp(), r.i32()))
        else: ai.append(None)
    return bts,ai
def read_record(r):
    t=r.u8()
    if t==0: r.p+=16; return None
    if t==12: r.i32(); r.lp(); return read_record(r)
    if t in (4,5):
        oid=r.i32(); name=r.lp(); n=r.i32(); members=[r.lp() for _ in range(n)]
        bts,ai=read_types(r,n)
        if t==5: r.i32()
        cls=(name,members,bts,ai); classes[oid]=cls
        o={'__class':name}; objs[oid]=o; o.update(read_values(r,cls)); return Ref(oid)
    if t in (2,3):
        oid=r.i32(); name=r.lp(); n=r.i32(); members=[r.lp() for _ in range(n)]
        if t==3: r.i32()
        cls=(name,members,[2]*n,[None]*n); classes[oid]=cls
        o={'__class':name}; objs[oid]=o
        for m in members: o[m]=read_record(r)
        return Ref(oid)
    if t==1:
        oid=r.i32(); mid=r.i32(); cls=classes[mid]; classes[oid]=cls
        o={'__class':cls[0]}; objs[oid]=o; o.update(read_values(r,cls)); return Ref(oid)
    if t==6:
        oid=r.i32(); s=r.lp(); objs[oid]=s; return Ref(oid)
    if t==8:
        pt=r.u8(); return prim(r,pt)
    if t==9: return Ref(r.i32())
    if t==10: return None
    if t==13: n=r.u8(); return [None]*n
    if t==14: n=r.i32(); return [None]*n
    if t==15:
        oid=r.i32(); n=r.i32(); pt=r.u8()
        if pt==2: v=bytes(r.d[r.p:r.p+n]); r.p+=n
        else: v=[prim(r,pt) for _ in range(n)]
        objs[oid]=v; return Ref(oid)
    if t in (16,17):
        oid=r.i32(); n=r.i32(); out=[]
        while len(out)<n:
            v=read_record(r)
            if isinstance(v,list): out.extend(v)
            else: out.append(v)
        objs[oid]=out; return Ref(oid)
    if t==7:
        oid=r.i32(); at=r.u8(); rank=r.i32(); lens=[r.i32() for _ in range(rank)]
        if at in (3,4,5): [r.i32() for _ in range(rank)]
        bt=r.u8(); ai=None
        if bt in (0,7): ai=r.u8()
        elif bt==3: ai=r.lp()
        elif bt==4: ai=(r.lp(),r.i32())
        total=1
        for l in lens: total*=l
        out=[]
        while len(out)<total:
            if bt==0: out.append(prim(r,ai))
            else:
                v=read_record(r)
                if isinstance(v,list): out.extend(v)
                else: out.append(v)
        objs[oid]=out; return Ref(oid)
    if t==11: return 'END'
    raise Exception('record %d at %d'%(t,r.p-1))
def load(path):
    d=open(path,'rb').read(); out=zlib.decompressobj(15).decompress(d[12:])
    r=R(out)
    while r.p<len(out):
        v=read_record(r)
        if v=='END': break
    return objs
def deref(v,depth=0):
    if isinstance(v,Ref): v=objs.get(v.i)
    if depth>6: return v
    if isinstance(v,dict): return {k:deref(x,depth+1) for k,x in v.items()}
    if isinstance(v,list) and len(v)<50: return [deref(x,depth+1) for x in v]
    return v
def schemes():
    for oid,o in objs.items():
        if isinstance(o,dict) and 'key' in o and 'value' in o:
            k=deref(o['key']); v=deref(o['value'])
            if isinstance(k,str) and isinstance(v,dict) and v.get('__class','').endswith('CxEncryption'):
                yield k,v
if __name__=='__main__':
    load(sys.argv[1])
    if sys.argv[2]=='--list':
        flt=(sys.argv[3] if len(sys.argv)>3 else '').lower()
        for k,_ in sorted(schemes()):
            if flt in k.lower(): print(k)
        sys.exit(0)
    title=sys.argv[2]
    found=[v for k,v in schemes() if k==title]
    if not found: sys.exit('no CxEncryption scheme named %r' % title)
    v=found[0]
    control=v['ControlBlock']
    if not isinstance(control,list) or len(control)!=1024:
        sys.exit('scheme %r has no embedded control block (it reads one from %s)' % (title, v.get('TpmFileName')))
    out=bytearray(b'SHCX')+struct.pack('<III',1,v['m_mask'],v['m_offset'])
    out+=bytes(v['PrologOrder'])+bytes(v['OddBranchOrder'])+bytes(v['EvenBranchOrder'])+b'\0\0\0'
    out+=struct.pack('<1024I',*[c & 0xffffffff for c in control])
    open(sys.argv[3],'wb').write(out)
    print('wrote', sys.argv[3], len(out), 'bytes for', title)
