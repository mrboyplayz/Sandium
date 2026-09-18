# CS Auth protocol — reverse engineering state (UNFINISHED)

## 2026-09-18 — join flow fully mapped; Join IP shipped; master J/K live
- **No real version incompatibility exists**: client (Steam + Noxus 0.38f) join
  carries ver bytes 0x26 (=dword_1402B231C, "0.38") and 0x47 (=dword_1402B26C8);
  the VPS ELF's constant at vaddr 0x2e9f00 (cmp in the join parse, ref via
  disasm @0xd1404) is ALSO 0x47. "Incompatible version" seen earlier was from
  malformed synthetic packets, not the real client.
- **Synthetic join to live server (127.0.0.1:27584)**: `7DFP\x02\x26<tok4><ses4>
  <id32>\x47<phone4>` → server replies `7DFP\x03<len>Incompatible version`
  (26 B) — my field order guess is wrong (server parse reads u32,u32,32B,u32,
  32B THEN ver8; disasm @0xd1350-0xd1410). All-0x47 packet → silence (dropped
  earlier). Server also checks sender against a master-introduce table
  (0x669b20, stride 0x2e1e0, matched by src ip/port/qword) — unknown senders
  go to the version-check path, matched ones to type-4/7 handlers.
- **'J' (0x4a) list request**: friend's client (94.99.165.6) sent bursts of
  `7DFP\x4a` after auth — proof the H/I auth fix WORKED. Reply format (client
  parse @241331): `7DFP\x4a + u32 count + u32 start + count×(u32 ip reversed +
  u16 port LE)`. Client then pings each entry (`7DFP`+u32 time, 9 B); server
  info reply populates the list. **Master now replies** (tested).
- **'K' (0x4b) introduce**: connect screen sends `7DFP\x4b + u32 token + u32
  accept + u32 altIP + u16 altPort` (19 B) to the MASTER; client advances its
  join latch (179668132 index... actually state idx 179662132) to 2 on ANY
  `7DFP\x4b + u32` reply, then sends the real type-2 join straight to the game
  server. **Master now replies** `7DFP\x4b+u32(1)` (tested).
- **Game server never registered with the master**: its serverinfo.php parse
  (dedicated decomp @126520-640) uses port+2 → registers to UDP 28017 (nothing
  listens; 0 packets ever seen on 28015/28017 from the server in tcpdump).
  Also possible its recv gets partial data (broken-pipe resets in our log).
  If the master-introduce table gate blocks real joins, next step: make the
  master send the server a type-4/7 introduce (RE @dedicated d06d3/d0791)
  and/or bind UDP 28017.
- **Sandium DirectJoin.cpp** (from a prior session) already had the browser
  "Testing Server" button + K-gate forcing + version auto-cycler (cycler needs
  menu==2 && joined==2, only reachable once K replies — now live). This session
  added: JoinServer(ip,port) generalization + **main-menu "Join IP" panel**
  (IP/port text entries via newly registered game string-entry widget
  DrawMenuTextEntryFunc @RVA 0x70200; linux 0) → Connect writes both game-addr
  pairs + menu state 2 = vanilla connecting screen. DLL built + deployed to
  the Noxus client folder.
- **Next test**: launch Noxus client → main menu "Join IP" → Connect
  (185.227.111.150:27584 default) → watch master log for INTRO K + server for
  join. Debug overlay (left column, yellow) shows menu/auth/kgate/joined/v2.

---
(original notes below)

## SOLVED 2026-09-17 (implemented + synthetic-tested, awaiting real client test)
The recv pump for the auth state machine is **sub_14011C760** (not sub_14007C1C0 —
that one is the game-channel parser). sub_14011C760 recvfrom's on the master UDP
socket, validates `7DFP` (dword_1402B2658), sets the stream read-pos to 4 (header
stripped) and the state machine at sub_14006BED0 parses the payload **natively
(x86 little-endian)**:
- Payload byte 0x48 ('H') = Steam auth reply: u32 token (→state[134050334]) +
  u32 accept (→state[134050338]; 0 ⇒ state=-1 fail) + [u32 phone if accept].
- Payload byte 0x49 ('I') = CS auth reply: u32 ok; if 1 ⇒ u32 phone
  (→state[134050337]) and state=2 (authed, menu "%03d-%04d" of the uint).
Client requests: 'H' = `7DFP 0x48` + 32B id (from state[73991096]) + u32 len +
len-byte echo of the last master reply; 'I' = `7DFP 0x49` + u32 token + u32 flag.
Phone int = prefix*10000+suffix (2560001 → "256-0001"). Keys: phones.json entry
`udp:<hex of the 32B id>`; 'I' resolves via token→phone session map (token =
sha1(key)[:4] as LE u32), addr fallback.
noxus_master.py now answers 0x48/0x49; synthetic H→I→repeat exchange verified
over the live UDP 28015 (accept=1, phone 2560005, consistent on repeat).
**Remaining: one real client relaunch to confirm phone shows + server list.**

---
(original RE notes below)

Goal: make the Noxus client complete auth against our master (185.227.111.150)
so phone numbers show in the menu, the server list populates, and joins work.

## What works already
- Noxus client → VPS:80 GET /anewzero/serverinfo.php ✓ (response: "078 185.227.111.150 28015")
- VPS dedicated server patched to same master ✓
- Master UDP 28015 replies to client 0x40 requests with
  `7DFP 0x40 <reversed ip 4B> <port 2B LE>` ✓ (log: "UDP out ... len=11")

## The blocker
The client's auth state machine (sub_14006BED0) sits in state 0 ("Steam Auth...")
and never advances to state 1 ("CS Auth...") or 2 (authed, phone shown).
State var: dword_140404A6C[179668277].

States (menu strings at .rdata 0x275a30+):
  0 = "Steam Auth..." / "Steam Ticket..." / "Steam Init Failed"
  1 = "CS Auth..."
  2 = authed — menu shows phone via "%03d-%04d", dword[134050337] = phone as
      (prefix*10000 + suffix), e.g. 2560001 → "256-0001". Display string in exe:
      "%s %03d-%04d" (the %s is a name/label).

State 0 → 1 transition: requires dword[134050338] != 0 (Steam auth accepted by
master). Our master never sends whatever sets that.

## Key state variables (giant .data array dword_140404A6C, indices, not bytes)
- [179668277] auth state (0/1/2)
- [134050337] phone number uint
- [134050338] steam-auth-accepted flag
- [278384725] steam ready (we force true via hotfix ✓)
- [278384729] "auth request sent" latch
- [278384730] reply length, [278384731...] reply data
- [278384987] "reply received" flag
- [289301092..295] outgoing packet builder (size 5, address, port)

## The exchange (from decomp sub_14006BED0 + sub_14007C1C0)
- State 0: client registers reply handler sub_1401880C0 via sub_140198D20, then
  on reply receipt sends 5-byte `7DFP 0x48` packets (sub_14007B180 memsets).
- sub_1401880C0(_BOOL *a1, unsigned int a2): stores reply data —
  dword[278384730] = a2 (len); memmove(&dword[278384731], a1, a2);
  dword[278384987] = 1. So ANY UDP reply from the master populates the buffer;
  the CLIENT parses it in sub_14007C1C0.
- sub_14007C1C0 checks reply[0..2] == "7DF" and reply[3] == 'P' (0x50) —
  **our master's replies must start "7DFP"** — then branches on further bytes:
  reply[3-area] == 'P' → stores ntohl(reply[0..3]) into state[112673951-slot]
  and ntohs(reply[4..5]) — i.e. reads u32 BE + u16 BE from the reply.
  reply[3] == 'f' (0x66) → continuation path calling sub_14007C3A0.

## Unknown (what the next session must find)
1. The exact expected CONTENT of the Steam-auth reply that flips
   dword[134050338] — sub_14007C1C0's byte-3=='P' branch reads state, but the
   deeper parse (probably sub_14007C3A0 or the state-0 handler) decides.
2. The 'I' (0x49) request sent in state 1 (CS Auth) and the master reply that
   completes it (state 1 → 2 = phone assigned).
3. Where the client's /phone and /hlist HTTP GETs fit — the exe contains
   "/phone", "/hlist", "adminphone=" strings; the master serves these on
   TCP 80 AND 28015 now.

## Where to look
- subrosa.c (client decomp): sub_14006BED0 (state machine),
  sub_14007C1C0 (UDP reply parser), sub_14007C3A0 ('f' continuation),
  sub_1401880C0 (reply storer), sub_140198D20 (handler registration).
- Live capture: VPS `tcpdump -i any -A udp port 28015` while a client joins —
  compare client requests vs our replies byte-for-byte.
- The real srnoxus.ddns.net master is DEAD (unresolvable) — no ground truth
  capture possible; must match the client's parse.

## Everything else working as of this note
- Sandium rebrand, boxing, billboards ([E] weapon_name), car-exit survival,
  freecam (F10/WASD/QE/CTRL+F10), addon enable/disable menu, pose tool
  (placeholder), file-streaming port 28000 reserved in firewall.
