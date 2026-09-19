---@type Plugin
local plugin = ...
plugin.name = "Bones"
plugin.author = "Sandium"
plugin.description = "Broken bones: hard impacts break limbs (HP to zero, renders black, clients play the crack), broken legs force the straight-legs movement state, broken limbs lose their IK drive (dangle), and 'break arm'/'break leg' chat commands break your own limbs."

local BREAK_DAMAGE = 20 -- single-hit damage needed to break a limb

-- skeleton bone groups (enum.body)
local function groupForBone(bone)
    if bone == 3 then return "headHP" end
    if bone >= 0 and bone <= 2 then return "chestHP" end
    if bone >= 4 and bone <= 6 then return "leftArmHP" end
    if bone >= 7 and bone <= 9 then return "rightArmHP" end
    if bone >= 10 and bone <= 12 then return "leftLegHP" end
    if bone >= 13 and bone <= 15 then return "rightLegHP" end
    return nil
end

local function groupIndexForBone(bone)
    if bone == 3 then return 0 end
    if bone >= 0 and bone <= 2 then return 1 end
    if bone >= 4 and bone <= 6 then return 2 end
    if bone >= 7 and bone <= 9 then return 3 end
    if bone >= 10 and bone <= 12 then return 4 end
    if bone >= 13 and bone <= 15 then return 5 end
    return nil
end

local function bonesForGroup(group)
    if group == 0 then return {3} end
    if group == 1 then return {0, 1, 2} end
    if group == 2 then return {4, 5, 6} end
    if group == 3 then return {7, 8, 9} end
    if group == 4 then return {10, 11, 12} end
    if group == 5 then return {13, 14, 15} end
    return {}
end

local GROUP_FIELDS = { "headHP", "chestHP", "leftArmHP", "rightArmHP", "leftLegHP", "rightLegHP" }

-- crack event log: clients poll /stream/events.txt and play the sound with
-- distance volume, so everyone near the break hears it
local eventSeq = 0
local eventsFile = "/opt/subrosa/stream/events.txt"

local function logCrackEvent(human)
    eventSeq = eventSeq + 1
    local pos = human.pos
    local line
    if pos then
        line = string.format("crack %d %.2f %.2f %.2f", eventSeq, pos.x, pos.y, pos.z)
    else
        line = string.format("crack %d 0 0 0", eventSeq)
    end
    local f = io.open(eventsFile, "a")
    if f then
        f:write(line .. "\n")
        f:close()
    end
end

-- broken[humanKey][groupIndex] = true
local broken = {}

local function humanKey(human)
    return tostring(human)
end

local function isBroken(human, group)
    local b = broken[humanKey(human)]
    return b and b[group]
end

local humanRefs = {}

local function breakGroup(human, group)
    local key = humanKey(human)
    broken[key] = broken[key] or {}
    broken[key][group] = true
    humanRefs[key] = human
    human[GROUP_FIELDS[group + 1]] = 0
    logCrackEvent(human)
    if group == 4 or group == 5 then
        human.movementState = 6 -- straight legs: very weak movement
    end
end

-- break detection
plugin:addHook("HumanDamage", function(human, bone, damage)
    if damage < BREAK_DAMAGE then return end
    local group = groupIndexForBone(bone)
    if not group then return end
    local field = GROUP_FIELDS[group + 1]
    if human[field] <= 0 then return end
    breakGroup(human, group)
    plugin:print("Broke a bone (bone " .. bone .. ", " .. field .. ") with " .. damage .. " damage")
end)

-- human references for the per-tick pin (cleared on delete)
-- per-tick: keep broken limbs at zero (health regen guard) + straight legs
plugin:addHook("Physics", function()
    for key, human in pairs(humanRefs) do
        local groups = broken[key]
        if groups and human then
            local anyLeg = false
            for group, _ in pairs(groups) do
                human[GROUP_FIELDS[group + 1]] = 0
                if group == 4 or group == 5 then anyLeg = true end
            end
            if anyLeg then
                human.movementState = 6
            end
        end
    end
end)

-- chat commands: 'break arm' / 'break leg' breaks the speaker's own limb
plugin:addHook("PlayerChat", function(ply, message)
    if not message then return end
    local msg = message:lower()
    local human = ply.human
    if not human then return end
    if msg == "break arm" then
        breakGroup(human, math.random(2, 3))
        plugin:print(ply.name .. " broke an arm")
    elseif msg == "break leg" then
        breakGroup(human, math.random(4, 5))
        plugin:print(ply.name .. " broke a leg")
    end
end)

-- the dangle: broken limbs lose their IK drive (strength zero)
plugin:addHook("HumanLimbInverseKinematics", function(human, trunkBoneID, branchBoneID,
        destination, destinationAxis, vecA, a, rot, strength, vecB, vecC, vecD, flags)
    local trunkGroup = groupIndexForBone(trunkBoneID)
    local branchGroup = groupIndexForBone(branchBoneID)
    if isBroken(human, trunkGroup) or isBroken(human, branchGroup) then
        strength.value = 0.0
    end
end)

-- cleanup
plugin:addHook("HumanDelete", function(human)
    local key = humanKey(human)
    broken[key] = nil
    humanRefs[key] = nil
end)
