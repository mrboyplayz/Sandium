-- Suitium Boxing: LMB punches the nearest human in reach.
-- Aim high for headshots, level for body shots, low for legs.
-- Knockback shoves the victim away. Singleplayer / Practice.
local REACH = 3.2          -- world units
local COOLDOWN = 20        -- frames between punches
local KNOCKBACK = 6.0      -- bone velocity impulse

local cooldown = 0

local function distance(a, b)
    local dx, dy, dz = a.x - b.x, a.y - b.y, a.z - b.z
    return math.sqrt(dx * dx + dy * dy + dz * dz)
end

Hook("DrawHUD", function()
    local me = Humans.GetLocal()
    if not me then return end

    if cooldown > 0 then
        cooldown = cooldown - 1
        return
    end

    -- input flag bit 0 = left mouse button (packet docs)
    if me.inputFlags % 2 ~= 1 then
        return
    end

    -- nearest other human within reach
    local target, targetDist
    for _, human in ipairs(Humans.GetAll()) do
        if human ~= me then
            local d = distance(me.position, human.position)
            if d <= REACH and (not targetDist or d < targetDist) then
                target, targetDist = human, d
            end
        end
    end
    if not target then
        return
    end

    -- pick the limb from aim pitch: down = legs, level = chest, up = head
    local bone, healthKey
    local pitch = me.viewPitch
    if pitch > 0.25 then
        bone, healthKey = 3, "headHealth"
    elseif pitch < -0.35 then
        bone, healthKey = 10, "leftLegHealth"
    else
        bone, healthKey = 2, "chestHealth"
    end

    local damage = 8 + math.random(12)
    if healthKey == "headHealth" then
        damage = damage * 2
    end

    local current = target[healthKey]
    if current > 0 then
        target[healthKey] = math.max(0, current - damage)
    end

    -- knockback: shove the victim's pelvis away from the puncher
    local d = math.max(targetDist, 0.1)
    local dx = (target.position.x - me.position.x) / d
    local dy = (target.position.y - me.position.y) / d
    local dz = (target.position.z - me.position.z) / d
    local pelvis = target:getBone(0)
    pelvis.velocity.x = pelvis.velocity.x + dx * KNOCKBACK
    pelvis.velocity.y = pelvis.velocity.y + dy * KNOCKBACK * 0.4
    pelvis.velocity.z = pelvis.velocity.z + dz * KNOCKBACK

    cooldown = COOLDOWN
end, "post")
