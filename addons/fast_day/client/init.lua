-- Fast day/night cycle: a full day passes in about 90 seconds.
-- Time sunAngle is the sun's angle in radians (0..2pi); the game advances it
-- itself, so adding a small step every frame speeds the whole cycle up.
local DAY_SECONDS = 90
local STEP = (math.pi * 2) / (DAY_SECONDS * 60) -- per frame at 60fps

-- The sun visual is driven by the server's raw 30-bit sunTime counter
-- (see networking.html, packet 0x05). Step it directly each frame.
local SUN_STEP = 20000

Hook("DrawHUD", function()
    Time:setSunTime(Time:getSunTime() + SUN_STEP)
end, "post")

local frames = 0
Hook("DrawHUD", function()
    frames = frames + 1
    if frames == 120 then
        print("fast day: valid=" .. tostring(Time.isValid()) ..
              ", sunAngle=" .. string.format("%.2f", Time:getSunAngle()))
    end
    if frames % 300 == 0 then
        Time.dumpDebug()
        print("fast day: sunTime=" .. Time:getSunTime())
    end
end, "post")
