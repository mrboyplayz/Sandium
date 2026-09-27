-- Native Sub Rosa blood effects for local Practice Mode.
if Game.isNoxus() then return end

local humanState = {}
local previewUntil = 0
local previewNext = 0
local frame = 0
local damageLogs = 0
local parts = {
    { "headHealth", 3 }, { "chestHealth", 2 },
    { "leftArmHealth", 6 }, { "rightArmHealth", 9 },
    { "leftLegHealth", 12 }, { "rightLegHealth", 15 },
}
local function engineDamage(human)
    local ok, value = pcall(function() return human.damage end)
    return ok and (tonumber(value) or 0) or 0
end

local function impact(position, count)
    if not position then return false end
    local emitted = false
    for i = 1, count do
        local angle = math.random() * math.pi * 2
        local radius = math.sqrt(math.random()) * (count > 1 and 0.36 or 0.2)
        local ox, oz = math.cos(angle), math.sin(angle)
        local sample = Vector3(position.x + ox * radius,
                               position.y + (math.random() - 0.5) * 0.14,
                               position.z + oz * radius)
        local direction = Vector3(ox * 0.55, 0.7, oz * 0.55)
        emitted = NativeBlood.Emit(sample, direction) or emitted
        if i <= 2 then NativeBlood.Ground(sample) end
    end
    return emitted
end

Hook("DrawHUD", function()
    if not Game.isInGame() then
        humanState = {}
        previewUntil = 0
        return
    end
    frame = frame + 1
    if frame % 2 ~= 0 then return end

    local now = UI.unixTime()
    local seen = {}
    for _, human in ipairs(Humans.GetAll()) do
        local id = human.index
        seen[id] = true
        local values = {}
        for i, part in ipairs(parts) do values[i] = human[part[1]] end
        local state = humanState[id]
        local currentDamage = engineDamage(human)
        local bodyID = human:getBone(2).rigidBodyID
        if human.health <= 0 then
            humanState[id] = nil
        elseif not state or state.bodyID ~= bodyID or
               human.health > state.health + 20 or
               human.bloodLevel > state.blood + 20 or
               currentDamage + 20 < state.damage then
            state = { values = values, health = human.health,
                      damage = currentDamage, bodyID = bodyID,
                      blood = human.bloodLevel, bleeding = human.isBleeding,
                      bleedUntil = 0, nextDrop = 0, nextImpact = 0, bone = 2 }
            humanState[id] = state
        else
            local damage, bone = 0, 2
            for i, part in ipairs(parts) do
                local lost = math.max(0, (state.values[i] or values[i]) - values[i])
                if lost > damage then damage, bone = lost, part[2] end
            end
            local healthLost = math.max(0, (state.health or human.health) - human.health)
            local bloodLost = math.max(0, (state.blood or human.bloodLevel) - human.bloodLevel)
            local hitDamage = math.max(0, currentDamage - (state.damage or currentDamage))
            local bleedingStarted = human.isBleeding and not state.bleeding
            state.values = values
            state.health = human.health
            state.blood = human.bloodLevel
            state.damage = currentDamage
            state.bleeding = human.isBleeding
            if damage > 0 or healthLost > 0 or bloodLost > 0 or hitDamage > 0 or bleedingStarted then
                if damageLogs < 12 then
                    print(string.format("[Native Blood] human %d limb %.1f health %.1f blood %.1f hit %d bleeding %s",
                        id, damage, healthLost, bloodLost, hitDamage, tostring(human.isBleeding)))
                    damageLogs = damageLogs + 1
                end
                if damage > 0 then state.bone = bone end
                state.bleedUntil = now + math.min(16, 5 + damage * 0.15)
                if now >= state.nextImpact then
                    state.nextImpact = now + 0.25
                    impact(human:getBone(state.bone).position, damage >= 12 and 2 or 1)
                end
            elseif human.isBleeding then
                state.bleedUntil = math.max(state.bleedUntil, now + 1)
            end
            if now < state.bleedUntil and now >= state.nextDrop then
                state.nextDrop = now + 0.2
                impact(human:getBone(state.bone).position, 1)
            end
        end
    end
    for id in pairs(humanState) do
        if not seen[id] then humanState[id] = nil end
    end

    -- F7 exercises the native path even before a Practice opponent is hurt.
    local me = Humans.GetLocal()
    if not me then return end
    local yaw = me.viewYaw
    local origin = Vector3(me.position.x + math.sin(yaw) * 2.5,
                          me.position.y + 1.4,
                          me.position.z + math.cos(yaw) * 2.5)
    if UI.keyPressed(118) then
        local emitted = impact(origin, 4)
        print("[Native Blood] F7 preview: " .. tostring(emitted))
        previewUntil = now + 4
        previewNext = now
    end
    if now < previewUntil and now >= previewNext then
        previewNext = now + 0.2
        impact(origin, 1)
    end
end, "post")

print("[Native Blood] Practice hook loaded; F7 previews native blood")
