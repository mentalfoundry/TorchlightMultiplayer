import re
data=open(r"D:\GAMES\Torchlight\Torchlight.exe","rb").read()
names=sorted(set(m.group(0).decode() for m in re.finditer(rb"\.\?A[VU][A-Za-z0-9_@?$]{2,200}?@@",data)))
open("rtti_classes.txt","w").write("\n".join(names))
print(len(names))
