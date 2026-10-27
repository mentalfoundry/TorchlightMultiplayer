# Parse MSVC x86 RTTI: TypeDescriptor -> CompleteObjectLocator -> vtable, plus class hierarchy.
import pefile, struct, re, json
pe = pefile.PE(r"D:\GAMES\Torchlight\Torchlight.exe", fast_load=True)
IB = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
def u32(va): return struct.unpack_from("<I", img, va-IB)[0]
def cstr(va):
    o=va-IB; e=img.index(b"\0",o); return img[o:e].decode('latin1')
secs={s.Name.rstrip(b'\0').decode():(IB+s.VirtualAddress, IB+s.VirtualAddress+s.Misc_VirtualSize) for s in pe.sections}
rd0,rd1=secs['.rdata']; tx0,tx1=secs['.text']; dt0,dt1=secs['.data']
def in_(va,r): return r[0]<=va<r[1]
# TypeDescriptors: name ".?AV" at td+8
tds={}
for m in re.finditer(rb"\.\?A[VU][^\0]+\0", img):
    td = IB+m.start()-8
    if in_(td,(dt0,dt1)) or in_(td,(rd0,rd1)): tds[td]=cstr(IB+m.start())
# COLs in .rdata: sig 0, ptd in tds
cols={}
for off in range(rd0-IB, rd1-IB-20, 4):
    sig,o,cd,ptd,pchd = struct.unpack_from("<5I", img, off)
    if sig==0 and ptd in tds and in_(pchd,(rd0,rd1)):
        cols[IB+off]=(o,cd,ptd,pchd)
# vtables: pointer to COL followed by pointer into .text
res=[]
colset=set(cols)
for off in range(rd0-IB, rd1-IB-8, 4):
    p=struct.unpack_from("<I",img,off)[0]
    if p in colset:
        vt=IB+off+4
        # count entries
        n=0
        while True:
            f=u32(vt+4*n)
            if not in_(f,(tx0,tx1)): break
            if n>0 and (vt+4*n) in vtstarts_guard if False else False: break
            n+=1
            if n>600: break
        o,cd,ptd,pchd=cols[p]
        # hierarchy
        sig,attr,nb,pbca=struct.unpack_from("<4I",img,pchd-IB)
        bases=[]
        for i in range(nb):
            bcd=u32(pbca+4*i)
            bptd,ncb,mdisp,pdisp,vdisp,battr=struct.unpack_from("<6I",img,bcd-IB)[:6]
            bases.append((tds.get(bptd,"?"),mdisp))
        res.append(dict(vtable=hex(vt),cls=tds[ptd],offset=o,nfuncs=n,bases=bases))
# trim vtable lengths to not overlap next vtable's COL pointer
vts=sorted(int(r['vtable'],16) for r in res)
for r in res:
    v=int(r['vtable'],16)
    nxt=[x for x in vts if x>v]
    if nxt: r['nfuncs']=min(r['nfuncs'],(nxt[0]-4-v)//4)
res.sort(key=lambda r:r['cls'])
json.dump(res,open("vtables.json","w"),indent=1)
print(len(tds),"typedescs",len(cols),"COLs",len(res),"vtables")
