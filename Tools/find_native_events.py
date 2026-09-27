from pathlib import Path
import pefile
import capstone

exe = Path(r"D:\SteamLibrary\steamapps\common\Sub Rosa\subrosa.exe")
pe = pefile.PE(str(exe))
section = next(x for x in pe.sections if x.Name.rstrip(b"\0") == b".text")
base = pe.OPTIONAL_HEADER.ImageBase
start = base + section.VirtualAddress
dis = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
instructions = list(dis.disasm(section.get_data(), start))
for index, inst in enumerate(instructions):
    if inst.mnemonic == "shl" and inst.op_str.endswith(", 7"):
        region = instructions[max(0, index - 16): index + 28]
        print(f"\nCandidate RVA {inst.address - base:#x}")
        for line in region:
            print(f"  {line.address - base:#x}: {line.mnemonic} {line.op_str}")
