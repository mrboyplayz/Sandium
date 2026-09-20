-- Server Stream: content published by the server. This file and its media
-- are downloaded automatically from the server -- clients need nothing but
-- Sandium itself. Edit this file on the server; clients get it next launch.

print("[Server Stream] init.lua executed, ServerFlags=" ..
    tostring(ServerFlags ~= nil) .. " UI=" .. tostring(UI ~= nil) ..
    " Billboards=" .. tostring(Billboards ~= nil))

-- ---- HUD layout (UI space is 1024x768, alignment 1 = centered) ----
local BLUE = { 0.13, 0.59, 0.95, 1.0 }
local MAGENTA = { 0.85, 0.15, 0.60, 1.0 }

-- The Homicide HUD is toggled live by the server's /hmcd <on|off> command
-- (admin/console): it rewrites the served flags file, clients pick it up
-- within ~2 seconds. Default ON until the server says otherwise.
local hudTicks = 0
local function DrawHUDLayout()
    hudTicks = hudTicks + 1
    if hudTicks == 60 then
        print("[Server Stream] DrawHUD alive after 60 frames, homicide_hud flag='" ..
            tostring(ServerFlags.Get("homicide_hud")) .. "'")
    end
    if ServerFlags.Get("homicide_hud") == "0" then return end
    -- semi-transparent black backdrop, like the original screenshot
    UI.rect(0, 0, 1024, 768, 0.0, 0.0, 0.0, 0.55)
    UI.textShadow("Homicide | State of Emergency", 512, 67, 30, BLUE[1], BLUE[2], BLUE[3], BLUE[4], 1)
    UI.textShadow("You are an Innocent", 512, 383, 27, BLUE[1], BLUE[2], BLUE[3], BLUE[4], 1)
    UI.textShadow("Occupation: Huntsman", 512, 427, 17, BLUE[1], BLUE[2], BLUE[3], BLUE[4], 1)
    UI.textShadow("You are an innocent with a hunting weapon. Find and neutralize the traitor before it's too late...",
        512, 700, 16, MAGENTA[1], MAGENTA[2], MAGENTA[3], MAGENTA[4], 1)
end

Hook("DrawHUD", function()
    DrawHUDLayout()
end, "post")

Hook("DrawMenu", function()
    Billboards.Clear()
end, "post")
