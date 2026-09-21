import ctypes, ctypes.wintypes as wt, struct, sys, time
from PIL import ImageGrab
user32 = ctypes.windll.user32
k32 = ctypes.windll.kernel32
psapi = ctypes.windll.psapi

snap = k32.CreateToolhelp32Snapshot(2, 0)
class PROCENT(ctypes.Structure):
    _fields_=[('dwSize',wt.DWORD),('cntUsage',wt.DWORD),('th32ProcessID',wt.DWORD),
              ('th32DefaultHeapID',ctypes.POINTER(ctypes.c_ulong)),('th32ModuleID',wt.DWORD),
              ('cntThreads',wt.DWORD),('th32ParentProcessID',wt.DWORD),('pcPriClassBase',ctypes.c_long),
              ('dwFlags',wt.DWORD),('szExeFile',ctypes.c_char*260)]
pid=None
e = PROCENT(); e.dwSize=ctypes.sizeof(e)
ok = k32.Process32First(snap, ctypes.byref(e))
while ok:
    if e.szExeFile == b'subrosa.exe': pid = e.th32ProcessID
    ok = k32.Process32Next(snap, ctypes.byref(e))
h = k32.OpenProcess(0x438, False, pid)
mods = (ctypes.c_void_p * 1024)()
count = wt.DWORD(1024)
psapi.EnumProcessModulesEx(h, ctypes.byref(mods), ctypes.sizeof(mods), ctypes.byref(count), 3)
name = ctypes.create_unicode_buffer(512)
base=None
for i in range(count.value // ctypes.sizeof(ctypes.c_void_p)):
    psapi.GetModuleFileNameExW(h, ctypes.c_void_p(mods[i]), name, 512)
    if name.value.lower().endswith('subrosa.exe'): base = mods[i]; break

def write_offset(x, y, z):
    addr = base + 0x75000000 + 46*5072 + 128
    buf = struct.pack('<3f', x, y, z)
    got = ctypes.c_size_t()
    return bool(k32.WriteProcessMemory(h, ctypes.c_void_p(addr), buf, len(buf), ctypes.byref(got)))

hwnd = user32.FindWindowA(None, b"Sub Rosa")
user32.SetForegroundWindow(hwnd)
time.sleep(0.4)
# press 2 via keybd_event (scan code 0x03 = '2')
user32.keybd_event(0x03, 0, 0, 0); time.sleep(0.05); user32.keybd_event(0x03, 0, 2, 0)
time.sleep(1.2)
vals = [
    (0.0, -0.0571, 0.1057),
    (0.0,  0.0640, 0.1090),
    (0.0, -0.0640, 0.0000),
    (0.0, -0.0640, -0.1090),
]
for i, v in enumerate(vals):
    write_offset(*v)
    time.sleep(0.8)
    img = ImageGrab.grab()
    img.save(f'Suitium/Tools/poser/grip_{i}.png')
    print(f'saved grip_{i}.png offset={v}')
