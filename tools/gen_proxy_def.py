# Generates src/proxy/OgreGUIRenderer.def: forward every export of the game's original
# OgreGUIRenderer.dll (renamed to OgreGUIRenderer_orig.dll on install) by ordinal and name.
import pefile, sys
src = sys.argv[1] if len(sys.argv) > 1 else r"D:\GAMES\Torchlight\OgreGUIRenderer.dll"
pe = pefile.PE(src)
lines = ["LIBRARY OgreGUIRenderer", "EXPORTS"]
for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
    n = e.name.decode()
    lines.append(f"    {n}=OgreGUIRenderer_orig.{n} @{e.ordinal}")
open("src/proxy/OgreGUIRenderer.def", "w").write("\n".join(lines) + "\n")
print(len(lines) - 2, "exports")
