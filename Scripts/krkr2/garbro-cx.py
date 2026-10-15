#!/usr/bin/env python3
# Exports a KiriKiri CxDec scheme from GARbro's game database (ArcFormats/Resources/Formats.dat,
# MIT licence, https://github.com/crskycode/GARbro) as a Shiori scheme file (SHCX), read by
# ShioriKrkr (Packages/ShioriKrkrKit/Engine/cx_decrypt.cpp) and, on the Mac, by the AetherKrkr
# reference harness (AETHERKIRI_CX_SCHEME, patch 0015). The app builds the same files itself
# (GARbroDatabase.swift); this script is for the Mac harnesses.
#
# Hx titles (KiriKiri Z's Hxv4 archives: GARbro's HxCrypt) are written as SHCX version 2: the
# same CxDec part, then an Hx section with the index keys per archive, the filter key, the
# random type and GARbro's names list if it has one (layout: Engine/shiori_hx.cpp, HxScheme).
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
            if isinstance(k,str) and isinstance(v,dict) and v.get('__class','').endswith(('CxEncryption','.HxCrypt')):
                yield k,v
def hx_key(k1,k2):
    k1=bytes(k1 or b''); k2=bytes(k2 or b'')
    return (k1,k2) if len(k1)==32 and len(k2)==16 else None
def hx_names(db_path, names_file):
    # GARbro's names list ("hash:name" lines, 16 hex digits for a directory, 64 for a file),
    # beside Formats.dat in ArcFormats/Resources, when GARbro has one for the title.
    import os
    dirs={}; files={}
    path=os.path.join(os.path.dirname(db_path), names_file or '')
    if names_file and os.path.isfile(path):
        for line in open(path,encoding='utf-8',errors='replace'):
            h,_,name=line.rstrip('\r\n').partition(':')
            if len(h)==16: dirs[bytes.fromhex(h)]=name
            elif len(h)==64: files[bytes.fromhex(h)]=name
    return dirs,files
def hx_section(v, db_path):
    out=bytearray(b'HXv4')+struct.pack('<IQ',v.get('RandomType') or 0,(v.get('FilterKey') or 0)&0xffffffffffffffff)
    default=hx_key(v.get('IndexKey1'),v.get('IndexKey2'))
    out+=bytes([1 if default else 0])+(default[0]+default[1] if default else bytes(48))
    keys=[]
    d=v.get('IndexKeyDict')
    if isinstance(d,dict):
        for kv in d.get('KeyValuePairs') or []:
            kv=deref(kv)
            if not isinstance(kv,dict): continue
            name=deref(kv.get('key')); hk=deref(kv.get('value'))
            k=hx_key(hk.get('Key1'),hk.get('Key2')) if isinstance(hk,dict) else None
            if isinstance(name,str) and k: keys.append((name,k))
    out+=struct.pack('<I',len(keys))
    for name,(k1,k2) in keys:
        n=name.encode('utf-8'); out+=struct.pack('<H',len(n))+n+k1+k2
    for table in hx_names(db_path, v.get('NamesFile')):
        out+=struct.pack('<I',len(table))
        for h,name in table.items():
            n=name.encode('utf-8'); out+=h+struct.pack('<H',len(n))+n
    return bytes(out)
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
    hx=v.get('__class','').endswith('.HxCrypt')
    out=bytearray(b'SHCX')+struct.pack('<III',2 if hx else 1,v['m_mask'],v['m_offset'])
    out+=bytes(v['PrologOrder'])+bytes(v['OddBranchOrder'])+bytes(v['EvenBranchOrder'])+b'\0\0\0'
    out+=struct.pack('<1024I',*[c & 0xffffffff for c in control])
    if hx: out+=hx_section(v, sys.argv[1])
    open(sys.argv[3],'wb').write(out)
    print('wrote', sys.argv[3], len(out), 'bytes for', title)
