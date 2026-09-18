-- Media API demo. Set to true to draw a bouncing video overlay ("best of zach").
local ENABLE_VIDEO_DEMO = false

if ENABLE_VIDEO_DEMO then
    local video = Video.Load("client/test.mp4")
    video.layer = 0
    video:Play()
    print("Lua video loaded: " .. video.width .. "x" .. video.height .. ", " .. string.format("%.1f", video.duration) .. "s")

    -- The video sits centered and pulses its width and height (left/right and
    -- up/down) on slightly different speeds so it wobbles. UI space is 1024x768.
    local BASE_W, BASE_H = 320, 180
    local function PulsingRect()
        local t = video.position
        local w = BASE_W * (1.0 + 0.4 * math.sin(t * 2.0))
        local h = BASE_H * (1.0 + 0.4 * math.sin(t * 3.1))
        return (1024 - w) * 0.5, (768 - h) * 0.5, w, h
    end

    Hook("DrawHUD", function()
        if not video.isPlaying then video:Play() end -- loop the demo
        video:Draw(PulsingRect())
    end, "post")

    Hook("DrawMenu", function()
        if not video.isPlaying then video:Play() end -- loop the demo
        video:Draw(PulsingRect())
    end, "post")
end
