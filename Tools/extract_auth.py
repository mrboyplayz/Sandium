import io

SRC = r"D:\SteamLibrary\steamapps\common\Sub Rosa\decompilation\work\rebuild\client\subrosa.c"
DST = r"D:\SteamLibrary\steamapps\common\Sub Rosa\auth_sm.txt"

data = io.open(SRC, encoding="utf-8", errors="replace").read()
i = data.find("sub_14006BED0()")
j = data.find("14006BF1E", i)
func = data[i:j+40]

lines = func.split("\n")
out = []
for ln in lines:
    s = ln.strip()
    if (s.startswith("__int64 v") or s.startswith("unsigned int v") or s.startswith("unsigned int *") or
        s.startswith("int v") or s.startswith("char *") or s.startswith("float v") or s.startswith("char v") or
        s.startswith("_BYTE") or s.startswith("_DWORD") or s.startswith("_QWORD") or s.startswith("__int128") or
        s.startswith("double ") or s.startswith("__int64 *") or s.startswith("unsigned __int64 v") or
        s.startswith("signed int")) and s.endswith(";"):
        continue
    out.append(ln)

io.open(DST, "w", encoding="utf-8", newline="\n").write("\n".join(out))
print("wrote", len(out), "lines")
