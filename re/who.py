import numpy as np, sys
src=np.load("calls_src.npy"); tgt=np.load("calls_tgt.npy")
for a in sys.argv[1:]:
    v=int(a,16); print(a,"called from:",[hex(x) for x in src[tgt==v]][:20])
