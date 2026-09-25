---@type Plugin
local mode = ...

mode.name = "Hide and Seek"
mode.author = "Sandium"
mode.description = "Ready lobby, one seeker, a 60-second hiding phase, and a bounded Round arena."

mode.defaultConfig = {
    minX = 1503.18, maxX = 1775.96,
    minY = 14.66, maxY = 62.15,
    minZ = 1041.94, maxZ = 1292.84,
    seekerX = 1640.01, seekerY = 24.84, seekerZ = 1128.15,
    hiderRadius = 18,
    lobbySeconds = 120,
    hideSeconds = 60,
    huntSeconds = 360,
    endingSeconds = 10,
    fogDensity = 6,
}

local flagsPath = "/opt/subrosa/stream/flags.txt"
local phase = "lobby"
local participants = {}
local seeker = nil
local knife = nil
local knifePending = nil
local knifeKind = nil
-- Keep the authoritative item out of the client's right-hand visual slot.
-- Slot 0 is the primary hand: the knife must be HELD, not pocketed.
local knifeSlot = 0
local secondTick = 0
local actionTick = 0
local lastPublished = ""
local previousHomicideHud = "1"
local HIDE_SEEK_TYPE = 20 -- custom mode; no native Round team/ready screen
local soloBot = nil
local polling = false

local function number(name)
    return tonumber(mode.config[name]) or mode.defaultConfig[name]
end

local function activePlayer(ply)
    return ply and isActive(ply) and not ply.isBot
end

local function connectedPlayers()
    local list = {}
    for _, ply in ipairs(players.getNonBots()) do
        if activePlayer(ply) then list[#list + 1] = ply end
    end
    return list
end

local function announce(message)
    for _, ply in ipairs(connectedPlayers()) do ply:sendMessage(message) end
end

local function readFlags()
    local lines = {}
    local input = io.open(flagsPath, "r")
    if input then
        for line in input:lines() do lines[#lines + 1] = line end
        input:close()
    end
    return lines
end

local function publishFlags(force)
    local roster = {}
    for _, ply in ipairs(connectedPlayers()) do
        roster[#roster + 1] = (ply.name or "Player"):gsub("[|;,\r\n]", " "):sub(1, 24)
            .. "|" .. (ply.isReady and "1" or "0")
    end
    local payload = table.concat({
        "hs_active=1",
        "hs_phase=" .. phase,
        "hs_seeker_human=" .. tostring(seeker and seeker.human and seeker.human.index or -1),
        "hs_seconds=" .. math.max(0, math.ceil(server.time / server.TPS)),
        "hs_deadline=" .. tostring(os.time() + math.max(0, math.ceil(server.time / server.TPS))),
        "hs_fog_density=" .. tostring(mode.defaultConfig.fogDensity),
        "hs_roster=" .. table.concat(roster, ";"),
        "homicide_hud=0",
    }, "\n")
    if not force and payload == lastPublished then return end
    lastPublished = payload
    local kept = {}
    for _, line in ipairs(readFlags()) do
        if not line:match("^hs_") and not line:match("^homicide_hud=") then
            kept[#kept + 1] = line
        end
    end
    kept[#kept + 1] = payload
    local temporary = flagsPath .. ".hide-and-seek.tmp"
    local output = io.open(temporary, "w")
    if not output then return end
    output:write(table.concat(kept, "\n"), "\n")
    output:close()
    os.rename(temporary, flagsPath)
end

local function clearFlags()
    local kept = {}
    for _, line in ipairs(readFlags()) do
        if not line:match("^hs_") and not line:match("^homicide_hud=") then
            kept[#kept + 1] = line
        end
    end
    kept[#kept + 1] = "homicide_hud=" .. previousHomicideHud
    local output = io.open(flagsPath, "w")
    if output then
        output:write(table.concat(kept, "\n"), "\n")
        output:close()
    end
end

local function inBounds(pos)
    return pos.x >= number("minX") and pos.x <= number("maxX")
       and pos.y >= number("minY") and pos.y <= number("maxY")
       and pos.z >= number("minZ") and pos.z <= number("maxZ")
end

local function groundAt(x, z)
    local ok, hit = pcall(physics.lineIntersectLevel,
        Vector(x, number("maxY") + 15, z),
        Vector(x, number("minY") - 8, z), false)
    if ok and hit and hit.hit and hit.pos and hit.normal
       and hit.normal.y > 0.5 and hit.pos.y >= number("minY")
       and hit.pos.y <= number("maxY") - 2 then
        return hit.pos.y + 0.8
    end
    return number("seekerY")
end

local function hiderSpawn(index, count)
    local angle = 2 * math.pi * (index - 1) / count
    local x = number("seekerX") + number("hiderRadius") * math.cos(angle)
    local z = number("seekerZ") + number("hiderRadius") * math.sin(angle)
    x = math.max(number("minX") + 3, math.min(number("maxX") - 3, x))
    z = math.max(number("minZ") + 3, math.min(number("maxZ") - 3, z))
    return Vector(x, groundAt(x, z), z)
end

local function validHuman(ply)
    local human = ply and isActive(ply) and ply.human
    return human and human.isAlive and human or nil
end

local function giveKnife(human)
    local kind = knifeKind or itemTypes.getByName("M9 Bayonet")
    if not kind then return false end
    knifeKind = kind
    local position = human.pos
    local item = items.create(kind, Vector(position.x, position.y + 1, position.z), orientations.n)
    if not item then return false end
    knifePending = item
    local mounted = human:mountItem(item, knifeSlot)
    knifePending = nil
    if not mounted then
        item:remove()
        return false
    end
    knife = item
    human.data.bayonetItem = item
    human.data.bayonetCooldown = 0
    item.isInPocket = false
    item:update()
    mode:print(string.format("Seeker bayonet mounted: item=%d type=%d slot=%d",
        item.index, kind.index, knifeSlot))
    return true
end

local function ensureKnife()
    local human = validHuman(seeker)
    if not human or phase ~= "hunt" then return end
    if knife and knife.isActive and knife.parentHuman
       and knife.parentHuman.index == human.index then
        if knife.isInPocket then
            knife.isInPocket = false
            knife:update()
        end
        return
    end
    if knife and knife.isActive then knife:remove() end
    knife = nil
    knifePending = nil
    if not giveKnife(human) then mode:print("Unable to equip seeker knife") end
end

local function startRound(ready, chosenSeeker)
    server.sunTime = (19 * 60 + 30) * 60 * server.TPS
    seeker = chosenSeeker or ready[math.random(#ready)]
    participants = {}
    knife = nil
    local hiders = {}
    for _, ply in ipairs(ready) do
        local role = ply == seeker and "seeker" or "hider"
        participants[ply.index] = { player = ply, role = role }
        ply.team = role == "seeker" and 0 or 1
        ply.suitColor = role == "seeker" and 1 or 4
        ply.model = 0
        ply:update()
        if role == "hider" then hiders[#hiders + 1] = ply end
    end
    -- Spawn hiders first. The seeker has a body far outside the arena so the
    -- client can identify its role and cover the entire screen until release.
    for i, ply in ipairs(hiders) do
        local pos = hiderSpawn(i, #hiders)
        local human = humans.create(pos, orientations.n, ply)
        if human then
            human.data.hsLastSafe = pos
            ply:sendMessage("Hide! The seeker arrives in 60 seconds.")
        end
    end
    local staged = humans.create(Vector(0, 80, 0), orientations.n, seeker)
    if not staged then
        mode:print("Seeker could not spawn; restarting lobby")
        server:reset()
        return
    end
    staged.isImmortal = true
    staged:teleport(Vector(0, 80, 0))
    seeker:sendMessage("You are the seeker. Wait for the hiding timer.")
    phase = "hide"
    server.state = STATE_GAME
    server.time = number("hideSeconds") * server.TPS
    publishFlags(true)
end

local function finishRound(message)
    if phase == "ending" then return end
    phase = "ending"
    server.state = STATE_GAME
    server.time = number("endingSeconds") * server.TPS
    announce(message)
    publishFlags(true)
end

local function checkWinner()
    if not validHuman(seeker) then
        finishRound("Hiders win! The seeker is gone.")
        return true
    end
    local livingHiders = 0
    for _, entry in pairs(participants) do
        if entry.role == "hider" and validHuman(entry.player) then
            livingHiders = livingHiders + 1
        end
    end
    if livingHiders == 0 then
        finishRound("Seeker wins! All hiders were found.")
        return true
    end
    return false
end

local function startSolo(ply, role)
    if phase ~= "lobby" then
        ply:sendMessage("Solo practice can only start in the lobby.")
        return
    end
    if #connectedPlayers() ~= 1 then
        ply:sendMessage("Solo practice requires you to be the only player online.")
        return
    end
    local bot = players.createBot()
    if not bot then
        ply:sendMessage("Could not create a practice bot.")
        return
    end
    soloBot = bot
    bot.name = "Practice Bot"
    bot.isZombie = role == "hider"
    bot.isReady = true
    startRound({ply, bot}, role == "seeker" and ply or bot)
end

mode.commands["/hsready"] = {
    info = "Toggle Hide and Seek ready status.",
    call = function(ply)
        if phase == "lobby" and activePlayer(ply) then
            ply.isReady = not ply.isReady
            publishFlags(true)
        end
    end,
}

mode.commands["/hstest"] = {
    info = "Start solo Hide and Seek practice against a bot.",
    usage = "<seeker|hider>",
    call = function(ply, _, args)
        local role = args[1] or "seeker"
        if role ~= "seeker" and role ~= "hider" then
            ply:sendMessage("Use /hstest seeker or /hstest hider")
            return
        end
        if activePlayer(ply) then startSolo(ply, role) end
    end,
}

mode:addEnableHandler(function(isReload)
    for _, line in ipairs(readFlags()) do
        local value = line:match("^homicide_hud=(.*)$")
        if value then previousHomicideHud = value end
    end
    server.type = HIDE_SEEK_TYPE
    server.roundTeamDamage = 0
    if not isReload then server:reset() end
end)

mode:addDisableHandler(function()
    clearFlags()
end)

mode:addHook("ResetGame", function()
    server.type = HIDE_SEEK_TYPE
    server.levelToLoad = "round"
    server.roundTeamDamage = 0
end)

mode:addHook("PostResetGame", function()
    if soloBot and isActive(soloBot) then soloBot:remove() end
    soloBot = nil
    phase = "lobby"
    participants = {}
    seeker = nil
    knife = nil
    secondTick = 0
    actionTick = 0
    server.state = STATE_GAME
    server.time = number("lobbySeconds") * server.TPS
    server.sunTime = (19 * 60 + 30) * 60 * server.TPS
    for _, ply in ipairs(connectedPlayers()) do
        ply.isReady = false
        ply.teamSwitchTimer = 0
    end
    publishFlags(true)
end)

mode:addHook("Logic", function()
    server.roundTeamDamage = 0
    server.time = math.max(0, server.time - 1)
    secondTick = (secondTick + 1) % math.max(1, math.floor(server.TPS + 0.5))
    actionTick = (actionTick + 1) % math.max(1, math.floor(server.TPS / 4))

    if phase == "lobby" then
        local connected = connectedPlayers()
        local ready = {}
        for _, ply in ipairs(connected) do
            ply.teamSwitchTimer = 0
            if ply.isReady then ready[#ready + 1] = ply end
        end
        if #ready >= 2 and (#ready == #connected or server.time == 0) then
            startRound(ready)
        elseif #ready == 1 and #connected == 1 and server.time == 0 then
            startSolo(ready[1], "seeker")
        elseif server.time == 0 then
            server.time = 30 * server.TPS
        end
    elseif phase == "hide" then
        local human = validHuman(seeker)
        if human then
            human:teleport(Vector(0, 80, 0))
            human:setVelocity(Vector(0, 0, 0))
        end
        if not checkWinner() and server.time == 0 then
            phase = "hunt"
            server.time = number("huntSeconds") * server.TPS
            human = validHuman(seeker)
            if human then
                human.isImmortal = false
                human:teleport(Vector(number("seekerX"), number("seekerY"), number("seekerZ")))
                human.data.hsLastSafe = human.pos
                ensureKnife()
            end
            announce("The seeker has spawned!")
            publishFlags(true)
        end
    elseif phase == "hunt" then
        if soloBot and seeker == soloBot and validHuman(soloBot) then
            for _, entry in pairs(participants) do
                if entry.role == "hider" and validHuman(entry.player) then
                    soloBot.botDestination = entry.player.human.pos
                    break
                end
            end
        end
        if not checkWinner() then
            if server.time == 0 then
                finishRound("Hiders win! Time ran out.")
            elseif secondTick == 0 then
                ensureKnife()
            end
        end
    elseif phase == "ending" and server.time == 0 then
        server:reset()
    end

    if phase == "hide" or phase == "hunt" then
        for _, entry in pairs(participants) do
            local human = validHuman(entry.player)
            if human and not (phase == "hide" and entry.role == "seeker") then
                if inBounds(human.pos) then
                    human.data.hsLastSafe = Vector(human.pos.x, human.pos.y, human.pos.z)
                else
                    human:teleport(human.data.hsLastSafe or
                        Vector(number("seekerX"), number("seekerY"), number("seekerZ")))
                end
            end
        end
    end
    if phase == "lobby" and actionTick == 0 and not polling then
            polling = true
            http.get("http://127.0.0.1:80", "/hs/poll", {}, function(response)
                polling = false
                if not response or response.status ~= 200 then return end
                for ip, action in response.body:gmatch("([^\r\n ]+) ([^\r\n ]+)") do
                    local matches = {}
                    for _, ply in ipairs(connectedPlayers()) do
                        if ply.connection and ply.connection.address == ip then
                            matches[#matches + 1] = ply
                        end
                    end
                    if #matches == 1 then
                        local ply = matches[1]
                        if action == "ready" then
                            ply.isReady = not ply.isReady
                            publishFlags(true)
                        elseif (action == "solo-seeker" or action == "solo-hider") and #connectedPlayers() == 1 then
                            startSolo(ply, action == "solo-seeker" and "seeker" or "hider")
                        end
                    end
                end
            end)
    end
    if secondTick == 0 then
        publishFlags(false)
    end
end)

mode:addHook("ServerSend", function()
    if phase == "lobby" then
        for _, ply in ipairs(connectedPlayers()) do ply.menuTab = 0 end
    end
end)

mode:addHook("ItemLink", function(item, childItem, parentHuman, slot)
    if phase ~= "hide" and phase ~= "hunt" then return end
    if knife and item and item.index == knife.index then
        return hook.override -- no dropping, pocketing, or transferring the knife
    end
    if knifePending and item and item.index == knifePending.index then
        return -- allow the mode's own first mount
    end
    if parentHuman and parentHuman.player then
        local entry = participants[parentHuman.player.index]
        if entry then
            if phase == "hunt" and entry.role == "seeker" and knifeKind
               and item.type and item.type.index == knifeKind.index
               and slot == knifeSlot and not knife then
                return -- allow the mode's initial hand mount
            end
            return hook.override -- players cannot equip other items
        end
    end
end)
