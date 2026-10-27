# fn.py addr... -> containing function (from Ghidra's functions.txt)
import sys, bisect
starts = []
for line in open(r"D:\CODE\TorchlightMultiplayer\re\functions.txt"):
    p = line.split()
    if p:
        try: starts.append((int(p[0], 16), line.strip()))
        except ValueError: pass
starts.sort(); keys = [s for s, _ in starts]
for a in sys.argv[1:]:
    v = int(a, 16); i = bisect.bisect_right(keys, v) - 1
    print(f"{v:08x} in {starts[i][1] if i >= 0 else '?'}")
