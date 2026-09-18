# Boxing2.lua → Sandium client addon port plan

Source: user's `boxing2.lua` — a RosaServer plugin (Linux LD_PRELOAD into
subrosadedicated.x64). RosaServer = same stack as Sandium (sol2/moonjit/subhook).
Reference: https://github.com/jpxs-intl/RosaServer (hooks.cpp / engine.h).

## Game functions to hook (signatures from RosaServer engine.h)
- Engine::physicsSimulation()                     -> "Physics" per-tick hook
- Engine::rigidBodySimulation()                   -> "PhysicsRigidBodies"
- Engine::addCollisionRigidBodyOnRigidBody(aBodyID, bBodyID, aLocalPos, bLocalPos, normal, f,f,f,f) -> "CollideBodies"
- Engine::humanLimbInverseKinematics(humanID, trunkBone, branchBone, Vector* dest, RotMatrix* rot, Vector* a, float a, float rot, float strength, float* quat, Vector*, Vector*, Vector*, char flags) -> "HumanLimbInverseKinematics"
- Engine::createHuman(Vector* pos, RotMatrix* rot, int playerID)  -> "HumanCreate"/"PostHumanCreate"
- Engine::humanApplyDamage(humanID, bone, unk, damage)

## Sandium-side status
- Same subhook mechanism already in place (Sandium/Hooks.cpp).
- Windows client offsets for these functions: NOT YET FOUND. The decomp
  (decompilation/work/rebuild/client/subrosa.c) contains them as sub_* — locate
  via RosaServer signatures + behavior. humanApplyDamage exists near bone
  health writes; physicsSimulation is the giant physics step.
- Client binary runs the sim locally in singleplayer/Practice, so server-style
  hooks work there. ONLINE they only affect the local view (server authority).
- Human struct already has: inputFlags, position, viewYaw/Pitch, bones[16]
  (Bone), inventorySlots[6], per-limb health (headHealth etc.) — everything
  boxing2.lua touches. Needs Lua usertypes (Humans + Bone) in LuaManager.

## boxing2.lua port notes
- `plugin:addHook(x)` -> Sandium `Hook(x, fn, "post")` once hooks above exist.
- `man:getRigidBody(i)` -> `human:getBone(i)` (new binding over Human.bones[16];
  expose rotVel for the look-direction swing logic — check Bone fields).
- `bit32` -> LuaJIT `bit` lib (moonjit has it; open the bit library in
  LuaManager or shim bit32 = require bit).
- Commands (/punch etc.) -> no chat API client-side; make them bind keys or a
  DrawMenu toggle instead.
- events.createSound -> needs the game's play-sound function hooked/exposed
  (RosaServer createEventSound — sound event writer exists in client too).
- Players/chat/phoneNumbers -> not applicable client-side; damage-indicator and
  block commands need rethinking or dropping.

## Phases
1. Bind Human/Bone/InputFlags to Lua (structs exist, mechanical).
2. Reverse Windows offsets for physicsSimulation + addCollisionRigidBodyOnRigidBody
   (hook those two only) -> damage/knockback on punch collisions works in
   singleplayer.
3. IK + sounds + createHuman hooks for the swing animation.
4. Port boxing2.lua onto the new API.
