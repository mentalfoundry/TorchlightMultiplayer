import pefile, sys, collections
pe = pefile.PE(r"D:\GAMES\Torchlight\Torchlight.exe")
print("Machine", hex(pe.FILE_HEADER.Machine), "ImageBase", hex(pe.OPTIONAL_HEADER.ImageBase), "Entry", hex(pe.OPTIONAL_HEADER.AddressOfEntryPoint))
print("DllChars", hex(pe.OPTIONAL_HEADER.DllCharacteristics), "Timestamp", pe.FILE_HEADER.TimeDateStamp)
for s in pe.sections:
    print(s.Name.rstrip(b'\0').decode(), hex(s.VirtualAddress), hex(s.Misc_VirtualSize), hex(s.SizeOfRawData), round(s.get_entropy(),2))
print("--- imports")
for e in pe.DIRECTORY_ENTRY_IMPORT:
    names=[(i.name.decode() if i.name else f"ord{i.ordinal}") for i in e.imports]
    print(e.dll.decode(), len(names))
    if e.dll.decode().lower() in ("ws2_32.dll","wsock32.dll","wininet.dll","kernel32.dll") :
        pass
    if e.dll.decode().lower() not in ("ogremain.dll","ceguibase.dll","msvcp90.dll","msvcr90.dll","kernel32.dll","user32.dll","particleuniverse.dll"):
        print("   ", names[:80])
