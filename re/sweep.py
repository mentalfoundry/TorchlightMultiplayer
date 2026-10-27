# sweep.py func_start lo hi : linear-sweep disassembly of [lo, hi] starting from the function start
import sys
from xref import *
from tldis import annot
fs, lo, hi = (int(a, 16) for a in sys.argv[1:4])
o = fs - T0
for ins in md.disasm(text[o:o + (hi - fs) + 16], fs):
    if lo <= ins.address <= hi:
        print(f"  {ins.address:08x} {ins.mnemonic:6} {ins.op_str} {annot(ins.op_str)}")
    if ins.address > hi: break
