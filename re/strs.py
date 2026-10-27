import re
data=open(r"D:\GAMES\Torchlight\Torchlight.exe","rb").read()
out=[]
for m in re.finditer(rb"[\x20-\x7e]{5,}",data): out.append(("A",m.start(),m.group().decode()))
for m in re.finditer(rb"(?:[\x20-\x7e]\x00){5,}",data): out.append(("W",m.start(),m.group().decode('utf-16le')))
out.sort(key=lambda x:x[1])
with open("strings.txt","w",encoding="utf-8") as f:
    for t,o,s in out: f.write(f"{t} {o:08x} {s}\n")
print(len(out))
