import pefile, struct, sys, capstone, json
pe = pefile.PE(r"D:\GAMES\Torchlight\Torchlight.exe", fast_load=True)
IB=pe.OPTIONAL_HEADER.ImageBase; img=pe.get_memory_mapped_image()
tx=pe.sections[0]; T0=IB+tx.VirtualAddress; text=img[tx.VirtualAddress:tx.VirtualAddress+tx.Misc_VirtualSize]
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32); md.detail=False
def find_imm(val):
    b=struct.pack("<I",val); out=[]; i=text.find(b)
    while i!=-1: out.append(T0+i); i=text.find(b,i+1)
    return out
def func_start(va):
    # scan back for typical prologue or int3/padding boundary
    o=va-T0
    for k in range(o, max(0,o-0x4000), -1):
        if text[k-1] in (0xCC,0xC3) and text[k] in (0x55,0x53,0x56,0x57,0x83,0x8B,0x6A,0x64,0x81,0x51,0xA1,0xB8):
            if text[k-1]==0xCC or (text[k-2:k-1]!=b'' ): return T0+k
    return None
def dis(va,n=40):
    o=va-T0
    for ins in md.disasm(text[o:o+n*8],va,count=n): print(f"  {ins.address:08x} {ins.mnemonic} {ins.op_str}")
if __name__=="__main__":
    for a in sys.argv[1:]:
        v=int(a,16); refs=find_imm(v); print(hex(v),"refs:",[hex(r) for r in refs][:30])
