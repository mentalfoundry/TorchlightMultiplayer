import sys; sys.argv=[sys.argv[0]]+sys.argv[1:]
from xref import *
import re
# string pool for annotating push imm / mov imm
def annot(op):
    for m in re.finditer(r"0x([0-9a-f]{6,8})",op):
        v=int(m.group(1),16)
        if IB<=v<IB+len(img):
            o=v-IB; s=img[o:o+80]
            if all(32<=c<127 for c in s[:4]):
                e=s.find(b"\0"); return '"'+s[:e if e>=0 else 60].decode('latin1')+'"'
            if s[1:2]==b"\0" and all(32<=c<127 for c in s[0:8:2]):
                return 'L"'+s.decode('utf-16le','ignore').split("\0")[0]+'"'
    return ""
def dis2(va,n):
    o=va-T0
    for ins in md.disasm(text[o:o+n*10],va,count=n):
        a=annot(ins.op_str)
        print(f"  {ins.address:08x} {ins.mnemonic:6} {ins.op_str} {a}")
if __name__=="__main__":
    va=int(sys.argv[1],16); n=int(sys.argv[2]) if len(sys.argv)>2 else 60
    dis2(va,n)
