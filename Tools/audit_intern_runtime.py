"""Read-only audit of native 0.38f bind buffers; never changes game state."""
import ctypes
import json
import struct
import sys
from pathlib import Path

pid, base = map(int, sys.argv[1:3])
kernel = ctypes.WinDLL('kernel32', use_last_error=True)
kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_bool, ctypes.c_ulong]
kernel.OpenProcess.restype = ctypes.c_void_p
kernel.ReadProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
kernel.CloseHandle.argtypes = [ctypes.c_void_p]
handle = kernel.OpenProcess(0x10, False, pid)
if not handle: raise ctypes.WinError(ctypes.get_last_error())
def read(rva, size):
    buffer = ctypes.create_string_buffer(size)
    count = ctypes.c_size_t()
    if not kernel.ReadProcessMemory(handle, base+rva, buffer, size, ctypes.byref(count)) or count.value != size:
        raise ctypes.WinError(ctypes.get_last_error())
    return buffer.raw
def values(rva, fmt): return struct.unpack(fmt, read(rva, struct.calcsize(fmt)))
def dword(index): return 0x404a6c + index*4
try:
    source = Path(sys.argv[3]).read_bytes()
    nv = struct.unpack_from('<I', source, 204)[0]
    nf = struct.unpack_from('<I', source, 208+nv*276)[0]
    faces = struct.unpack_from('<'+'I'*nf*3, source, 212+nv*276)
    result = {'parents': [values(0x4399DEE0 + i*100, '<i')[0] for i in range(16)]}
    for slot in (3,11):
        header = 0x269a49c0+slot*786828
        count, stride = values(header, '<ii')
        result[str(slot)] = {'count': count, 'stride': stride}
        result[str(slot)]['stored_binds'] = [values(0x26a649cc+slot*786828+i*24,'<6f') for i in range(16)]
        if count != nf*3 or stride != 16: continue
        buffer = read(header+8, count*64)
        error = 0.
        for i,vi in enumerate(faces):
            actual = struct.unpack_from('<3f', buffer, i*64)
            expected = struct.unpack_from('<3f', source, 208+vi*276)
            error = max(error, max(abs(a-b) for a,b in zip(actual,expected)))
        result[str(slot)]['native_bind_max_error'] = error
    result['bind_uniform'] = values(dword(23875685), '<48f')
    result['position_uniform'] = values(dword(23875733), '<48f')
    result['orientation_uniform'] = values(dword(23875781), '<144f')
    print(json.dumps(result, indent=2))
finally:
    kernel.CloseHandle(handle)
