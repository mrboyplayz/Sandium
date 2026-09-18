-- Server video: plays the video the server publishes (sandium/cache/server.mp4,
-- downloaded from the server's /stream/ endpoint automatically). Replaces the
-- old bundled test.mp4 demo -- the file now lives on the server, so every
-- client gets whatever video the server hosts.

local video

local BASE_W, BASE_H = 320, 180
local function PulsingRect(t)
    local w = BASE_W * (1.0 + 0.4 * math.sin(t * 2.0))
    local h = BASE_H * (1.0 + 0.4 * math.sin(t * 3.1))
    return (1024 - w) * 0.5, (768 - h) * 0.5, w, h
end

local function Load()
    if video then return end
    local path = ServerMedia.Path("server.mp4")
    if path == "" then return end
    video = Video.Load(path)
    if not video or not video.isValid then
        video = nil
        return
    end
    video.layer = 0
    video:Play()
    print("server video: " .. video.width .. "x" .. video.height ..
        ", " .. string.format("%.1f", video.duration) .. "s")
end

Hook("DrawHUD", function()
    if not video then Load() end
    if not video then return end
    if not video.isPlaying then video:Play() end -- loop
    video:Draw(PulsingRect(video.position))
end, "post")

-- Leaving the server (any menu is up): stop and rewind so the audio does
-- not keep playing over the menu; rejoining restarts it from the top.
Hook("DrawMenu", function()
    if video and video.isPlaying then video:Stop() end
end, "post")
