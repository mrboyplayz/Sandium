# .cmc (character model) format — work in progress

Goal: pose tool rendering the REAL Sub Rosa player model (user request).
Loader decomp (Rosetta stone): `decompilation/work/rebuild/client/subrosa.c:225752`
`asset_load_character_cmc_candidate` (0x1400E8580). Sample: `data/model/msuit1.cmc`
(561,332 bytes; mhead*.cmo = heads, msuit/fcasual etc = bodies).

## Confirmed by empirical parsing
- Header: `CMod` magic, u32 version=2, u32 partCount=16.
- 16x vec3 pivots immediately after (0x0c..0xcb). These are the LIMB PIVOTS:
  [0]=(0,0,0) pelvis, [1]=(0,.211,0) chest, [2]=(0,.216,0) head-ish,
  [3]=(0,.244,.01) neck-ish, [4]=(-.188,.147,.017) L shoulder,
  [5..6] L leg offsets, [7]=(.188,.147,.017) R shoulder, [8..9] R leg,
  [10..12] L hip/knee/foot, [13..15] R hip/knee/foot (exact naming TBD).
- u32 @0xcc = 2004 (likely total vertex count). Vertex stream @0xd0, 24B stride
  (pos3f, uv2f, 1f) per the .cmo v3 spec.
- Normals/second stream @0xbcc4 (after 20 zero bytes), 12B stride.
- Faces @0x87160: u32 count=668, then 668 x 3 u32 indices. ~64 trailing bytes.
- Packet doc (networking.html): humans sync as 16 parts, each part rotation =
  quaternion (2-bit largest-component + 3x12-bit components). Limb health names:
  chest, head, leftArm, rightArm, leftLeg, rightLeg.

## Unsolved
- The 0x11ab4..0x87160 region (~485KB, 84% small-u32) is per-part data
  (likely per-part vertex/face split or weights) — loader decomp lines
  ~225980-226060 show nested loops: outer reads 3xu32, inner loops 16 parts
  reading 3xu32 + 3 floats (x1.125 scale!) + 1 u32. The 1.125 scale hints
  packed positions (byte/short encoded) rather than raw floats.

## Next steps
1. Read the loader decomp line by line (it is complete, just garbled).
2. Finish parse -> per-part mesh + pivot -> feed pose tool (Tools/pose_tool.html
   currently has a placeholder blocky rig; swap rig data for parsed model).
