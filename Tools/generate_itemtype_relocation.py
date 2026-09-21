"""Generate Sandium's item-table relocation patch list from the IDA export."""

import argparse
import json
from pathlib import Path


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("references", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()

document = json.loads(args.references.read_text())
references = document["references"]
lines = [
    "// Generated from IDA 9.3 decoded ItemType references; do not edit manually.",
    "#pragma once",
    "#include <array>",
    "#include <cstdint>",
    "",
    "namespace itemtypes::detail {",
    "struct ReferencePatch { std::uint32_t instructionRva; std::uint8_t instructionSize; std::uint8_t displacementOffset; std::uint32_t targetOffset; bool ripRelative; };",
    f"inline constexpr std::array<ReferencePatch, {len(references)}> referencePatches{{{{",
]
for reference in references:
    lines.append(
        "    {{0x{instruction_rva:X}u, {instruction_size}u, {displacement_offset}u, "
        "0x{target_offset:X}u, {rip_relative}}},".format(
            instruction_rva=reference["instruction_rva"],
            instruction_size=reference["instruction_size"],
            displacement_offset=reference["displacement_offset"],
            target_offset=reference["target_offset"],
            rip_relative="true" if reference["rip_relative"] else "false",
        )
    )
lines.extend(["}};", "}", ""])
args.output.write_text("\n".join(lines), newline="\n")
print(f"wrote {len(references)} relocation patches to {args.output}")
