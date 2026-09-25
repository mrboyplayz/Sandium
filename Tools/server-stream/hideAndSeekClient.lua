-- BEGIN HIDE_AND_SEEK_HUD
local wasMouseDown = false
local overlayLogged = false

local function button(label, x, y, w, h, mx, my, click)
    local hover = mx >= x and mx <= x + w and my >= y and my <= y + h
    UI.rect(x, y, w, h, 0.62, 0.62, 0.62, 1)
    UI.rect(x + 1, y + 1, w - 2, h - 2,
        hover and 0.18 or 0.06, hover and 0.18 or 0.06, hover and 0.18 or 0.06, 1)
    UI.textShadow(label, x + w / 2, y + 9, 16, 1, 1, 1, 1, 1)
    return hover and click
end

local function topTimer(label)
    local width = #label * 12 + 28
    UI.rect((1024 - width) / 2, 10, width, 32, 0, 0, 0, 0.82)
    UI.textShadow(label, 512, 17, 17, 1, 1, 1, 1, 1)
end

local function drawHideAndSeek()
    if ServerFlags.Get("hs_active") ~= "1" then return end
    local phase = ServerFlags.Get("hs_phase")
    if not overlayLogged then
        overlayLogged = true
        print("[Hide and Seek] final-frame overlay active, phase=" .. tostring(phase))
    end
    local deadline = tonumber(ServerFlags.Get("hs_deadline"))
    local seconds = math.max(0, deadline and math.ceil(deadline - UI.unixTime())
        or tonumber(ServerFlags.Get("hs_seconds")) or 0)
    local localHuman = Humans.GetLocal()

    if phase == "lobby" then
        UI.textShadow("HIDE AND SEEK", 512, 52, 24, 1, 1, 1, 1, 1)
        UI.textShadow(string.format("STARTS IN %02d:%02d", math.floor(seconds / 60), seconds % 60),
            512, 95, 16, 0.75, 0.75, 0.75, 1, 1)
        UI.rect(218, 126, 588, 1, 0.45, 0.45, 0.45, 1)
        UI.textShadow("PLAYERS", 218, 142, 16, 0.8, 0.8, 0.8, 1, 0)
        local roster = ServerFlags.Get("hs_roster") or ""
        local count = 0
        for name, ready in roster:gmatch("([^|;]+)|([01])") do
            count = count + 1
            if count > 6 then break end
            local rowY = 166 + (count - 1) * 32
            UI.rect(218, rowY, 588, 30, 0.10, 0.10, 0.10, 1)
            UI.textShadow(name, 232, rowY + 6, 15, 1, 1, 1, 1, 0)
            local status = ready == "1" and "READY" or "WAITING"
            UI.textShadow(status, 790, rowY + 6, 15, 0.75, 0.75, 0.75, 1, 2)
        end
        if count == 0 then
            UI.textShadow("Joining lobby...", 512, 172, 17, 0.75, 0.75, 0.75, 1, 1)
        end
        local mx, my, down = UI.mouse()
        local click = down and not wasMouseDown
        wasMouseDown = down
        if button("READY UP / UNREADY", 218, 387, 588, 40, mx, my, click) then
            HideSeek.Action("ready")
        end
        UI.textShadow("Solo practice: /hstest seeker or /hstest hider", 512, 463,
            13, 0.65, 0.65, 0.65, 1, 1)
        return
    end

    if localHuman and localHuman.position.x < 100 and phase == "hide" then
        UI.rect(0, 0, 1024, 768, 0, 0, 0, 1)
        UI.textShadow("YOU ARE THE SEEKER", 512, 345, 28, 1, 1, 1, 1, 1)
        UI.textShadow(string.format("Hiders have %02d:%02d to hide", math.floor(seconds / 60),
            seconds % 60), 512, 390, 19, 0.8, 0.8, 0.8, 1, 1)
        return
    end
    if not localHuman then return end
    if phase == "hide" then
        topTimer(string.format("SEEKER SPAWNS IN %02d:%02d", math.floor(seconds / 60), seconds % 60))
    elseif phase == "hunt" then
        topTimer(string.format("HIDE AND SEEK  %02d:%02d", math.floor(seconds / 60), seconds % 60))
    end
end

Hook("DrawOverlay", drawHideAndSeek, "post")
-- END HIDE_AND_SEEK_HUD
