"""Read-only probe of the 0.38f client event ring for blood-hit debugging."""

import ctypes as C
import struct
import sys

pid = int(sys.argv[1])
kernel = C.WinDLL("kernel32", use_last_error=True)
psapi = C.WinDLL("psapi", use_last_error=True)
kernel.OpenProcess.argtypes = [C.c_uint32, C.c_int, C.c_uint32]
kernel.OpenProcess.restype = C.c_void_p
kernel.ReadProcessMemory.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p,
                                     C.c_size_t, C.POINTER(C.c_size_t)]
kernel.ReadProcessMemory.restype = C.c_int
psapi.EnumProcessModules.argtypes = [C.c_void_p, C.c_void_p, C.c_uint32,
                                     C.POINTER(C.c_uint32)]
psapi.EnumProcessModules.restype = C.c_int

handle = kernel.OpenProcess(0x0410, 0, pid)
if not handle:
    raise OSError(C.get_last_error(), "OpenProcess failed")
modules = (C.c_void_p * 16)()
needed = C.c_uint32()
if not psapi.EnumProcessModules(handle, modules, C.sizeof(modules), C.byref(needed)):
    raise OSError(C.get_last_error(), "EnumProcessModules failed")
base = modules[0]

def read(address, count):
    buffer = C.create_string_buffer(count)
    got = C.c_size_t()
    if not kernel.ReadProcessMemory(handle, C.c_void_p(address), buffer,
                                    count, C.byref(got)) or got.value != count:
        raise OSError(C.get_last_error(), f"ReadProcessMemory {address:#x} failed")
    return buffer.raw

counter = struct.unpack("<I", read(base + 0x49C1DC64, 4))[0]
print(f"base={base:#x} nextEvent={counter}")
counts = {}
for back in range(1, min(counter, 256) + 1):
    index = (counter - back) & 0xffff
    event = read(base + 0x473DE460 + index * 0x80, 0x30)
    kind, tick = struct.unpack_from("<ii", event, 0)
    counts[kind] = counts.get(kind, 0) + 1
    pos = struct.unpack_from("<fff", event, 8)
    source, hit_type = struct.unpack_from("<ii", event, 0x20)
    if kind == 1 and hit_type == 3:
        print(f"event[{index}] tick={tick} blood={hit_type} pos={tuple(round(x, 2) for x in pos)}")
    elif back <= 45:
        print(f"event[{index}] kind={kind} tick={tick} a={source} b={hit_type} pos={tuple(round(x, 2) for x in pos)}")
print("kinds", counts)
