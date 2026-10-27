from xref import *
import numpy as np, sys
arr=np.frombuffer(text,dtype=np.uint8)
idx=np.nonzero(arr==0xE8)[0]
idx=idx[idx+5<=len(text)]
rel=(arr[idx+1].astype(np.int64)|(arr[idx+2].astype(np.int64)<<8)|(arr[idx+3].astype(np.int64)<<16)|(arr[idx+4].astype(np.int64)<<24))
rel=np.where(rel>=2**31,rel-2**32,rel)
tgt=T0+idx+5+rel
np.save("calls_src.npy",T0+idx); np.save("calls_tgt.npy",tgt)
