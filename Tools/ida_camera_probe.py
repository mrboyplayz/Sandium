import os

import ida_auto
import ida_funcs
import ida_hexrays
import ida_nalt
import ida_lines
import ida_pro
import idautils
import idc

ida_auto.auto_wait()
base = ida_nalt.get_imagebase()
out_path = os.path.join(os.environ.get("TEMP", "."), "subrosa_camera_probe.txt")
with open(out_path, "w", encoding="utf-8") as out:
    for target_rva in (0x4CBFAA4, 0x4CBFAC4, 0x43F7C360, 0x43F7C398, 0x43F79DE4):
        target = base + target_rva
        out.write("\n=== Xrefs to RVA %X EA %X ===\n" % (target_rva, target))
        for xref in idautils.XrefsTo(target, 0):
            out.write("xref from RVA %X type=%s\n" % (xref.frm - base, xref.type))
            cur = xref.frm
            for _ in range(4):
                cur = idc.prev_head(cur)
            for _ in range(9):
                if cur == idc.BADADDR:
                    break
                out.write("%X: %s\n" % (cur - base, idc.generate_disasm_line(cur, 0) or ""))
                cur = idc.next_head(cur)

    for rva in (0x96967, 0x97567, 0x16356c, 0x215d40, 0x21626a, 0x234b10, 0x240961):
        ea = base + rva
        func = ida_funcs.get_func(ea)
        out.write("\n=== RVA %X EA %X function=%s ===\n" %
                  (rva, ea, hex(func.start_ea) if func else "none"))
        start = max(base, ea - 32)
        end = ea + 48
        cur = idc.prev_head(ea + 1, start)
        while cur != idc.BADADDR and cur < end:
            out.write("%X: %s\n" % (cur - base, idc.generate_disasm_line(cur, 0) or ""))
            cur = idc.next_head(cur, end)
        if func:
            try:
                cfunc = ida_hexrays.decompile(func.start_ea)
                if cfunc:
                    out.write("--- pseudocode ---\n%s\n" % str(cfunc))
            except Exception as exc:
                out.write("decompile error: %r\n" % exc)

ida_pro.qexit(0)
