---@type Plugin
local plugin = ...
plugin.name = "Footsteps"
plugin.author = "Sandium"
plugin.description = "Authoritative multiplayer footstep events for Sandium clients."

local EVENTS_FILE = "/opt/subrosa/stream/footsteps.txt"
local sequence = os.time() * 1000
local states = {}

local function keyFor(human)
    return tostring(human)
end

local function writeStep(human, position, speed, sample)
    sequence = sequence + 1
    local f = io.open(EVENTS_FILE, "a")
    if f then
        f:write(string.format("step %d %s %.2f %.2f %.2f %.2f %d\n", sequence,
            keyFor(human), position.x, position.y, position.z, speed, sample))
        f:close()
    end
end

-- Physics is server-authoritative, so every client receives the same step
-- positions and cadence instead of independently guessing from snapshots.
plugin:addHook("Physics", function()
    for _, human in ipairs(humans.getAll()) do
        local key = keyFor(human)
        local pos = human.pos
        if pos and human.isAlive and human.vehicle == nil then
            local state = states[key]
            if not state then
                states[key] = { x = pos.x, y = pos.y, z = pos.z, cooldown = 12, sample = 0 }
            else
                local dx, dz = pos.x - state.x, pos.z - state.z
                local distance = math.sqrt(dx * dx + dz * dz)
                local speed = distance * 60.0
                state.x, state.y, state.z = pos.x, pos.y, pos.z
                if state.cooldown > 0 then state.cooldown = state.cooldown - 1 end
                -- Ignore teleports and airborne/physics corrections. Normal
                -- walking is roughly 1-8 units/s in this coordinate system.
                if state.cooldown <= 0 and speed >= 0.55 and speed <= 10.0 then
                    state.sample = (state.sample + 1) % 4
                    writeStep(human, pos, speed, state.sample)
                    -- Running needs a much tighter cadence than walking.
                    -- At sprint speed this bottoms out around 0.18 seconds;
                    -- a normal walk remains roughly 0.4-0.5 seconds.
                    local interval = math.max(11, math.floor(34 - speed * 2.8))
                    state.cooldown = interval
                end
            end
        else
            states[key] = nil
        end
    end
end)

plugin:addHook("HumanDelete", function(human)
    states[keyFor(human)] = nil
end)
