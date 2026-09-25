---@type Plugin
local plugin = ...
plugin.name = "RunSpeed"
plugin.author = "Sandium"
plugin.description = "Keeps the hip upright and prevents grounded trips."

local MIN_INPUT = 0.01
local JUMP_FLAG = 4 -- Space (1 << 2) in RosaServer's Human inputFlags

local function keepHipUpright(human)
    local hip = human:getRigidBody(0)
    local forward = hip.rot:getForward()
    local forwardLength = math.sqrt(forward.x * forward.x + forward.z * forward.z)

    local forwardX, forwardZ
    if forwardLength > MIN_INPUT then
        forwardX = forward.x / forwardLength
        forwardZ = forward.z / forwardLength
    else
        local yaw = human.viewYaw or 0
        forwardX = math.sin(yaw)
        forwardZ = math.cos(yaw)
    end

    -- Preserve the hip's facing direction while removing pitch and roll.
    local right = hip.rot:getRight()
    local rightX = right.x
    local rightZ = right.z
    local projection = rightX * forwardX + rightZ * forwardZ
    rightX = rightX - projection * forwardX
    rightZ = rightZ - projection * forwardZ
    local rightLength = math.sqrt(rightX * rightX + rightZ * rightZ)
    if rightLength > MIN_INPUT then
        rightX = rightX / rightLength
        rightZ = rightZ / rightLength
    else
        rightX = forwardZ
        rightZ = -forwardX
    end

    hip.rot:set(RotMatrix(
        forwardX, 0, forwardZ,
        0, 1, 0,
        rightX, 0, rightZ))
end

plugin:addHook("PostPhysics", function()
    for _, human in ipairs(humans.getAll()) do
        if human.isActive and human.isAlive and human.vehicle == nil then
            keepHipUpright(human)

            local jumpHeld = math.floor((human.inputFlags or 0) / JUMP_FLAG) % 2 == 1

            -- Cancel non-jump falling/sliding transitions while grounded;
            -- intentional jumps still use the native movement state.
            if human.isOnGround and not jumpHeld and
                    (human.movementState == 1 or human.movementState == 2) then
                human.movementState = 0
            end

        end
    end
end)
