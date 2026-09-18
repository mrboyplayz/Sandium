# Lua images

Sandium addons can load common image files and draw them over the HUD or menus. On Windows, the decoder accepts PNG, JPEG, BMP, GIF, TIFF, and ICO files.

Place the image inside your addon folder. Paths passed to `Image.Load` are relative to that folder and cannot leave it.

```lua
local logo = Image.Load("client/logo.png")

local hud = Hook("DrawHUD", function()
    -- Native size at (32, 32)
    logo:Draw(32, 32)

    -- Or x, y, width, height, alpha
    logo:Draw(400, 24, 128, 64, 0.8)

    -- Or x, y, width, height, red, green, blue, alpha
    logo:Draw(560, 24, 128, 64, 1.0, 0.5, 0.5, 1.0)
end, "post")
```

Use `Hook("DrawMenu", function() ... end, "post")` for menu overlays. `Image:Draw` is restricted to `DrawHUD` and `DrawMenu` hooks because OpenGL drawing is only safe during those callbacks.

Properties:

- `image.width` / `image.sizeX` and `image.height` / `image.sizeY` contain the decoded size.
- `image.isValid` becomes false after `image:Delete()`.
- `image.layer` controls draw order (see below).
- `image.rotation` rotates the drawn quad, in degrees clockwise. Uses its own shader, so it works wherever `Draw` works; if that is unavailable it falls back to an unrotated draw.
- `image:Delete()` releases GPU memory early. Otherwise Lua garbage collection releases it.

## Videos

`Video.Load` plays MP4 and other common video files (anything Media Foundation decodes: H.264/MP4, WMV, etc.), with audio. Video playback advances while the video is drawn, so keep it inside a `DrawHUD` or `DrawMenu` hook like images.

```lua
local clip = Video.Load("client/clip.mp4")
clip.layer = 0
clip.rotation = 45
clip:Play()

Hook("DrawHUD", function()
    clip:Draw(24, 232, 320, 180)
end, "post")
```

Methods and properties:

- `video:Play()` starts (or resumes) playback; `video:Pause()` and `video:Stop()` do what you expect. Calling `Play()` after the end restarts from the beginning.
- `video:Draw` mirrors `Image:Draw`, including `layer` and `rotation`.
- `video.width` / `video.height` (also `sizeX` / `sizeY`) are the decoded frame size.
- `video.duration` and `video.position` are seconds; assign `position` to seek.
- `video.isPlaying` reports whether playback is running.

## Layers

Every image has a `layer` number, default `0`. Queued images are drawn sorted by layer from low to high, so higher layers appear on top of lower ones.

The frame is split into two passes around Sub Rosa's own UI drawing:

- Layers `0` and above are drawn **after** the game's UI in the same pass, so they appear on top of menus, the HUD, and the mouse cursor.
- Negative layers are drawn **before** it, behind the game's UI. Opaque fullscreen menus can hide them entirely, so they are mainly useful in-game, where they land on top of the 3D world but underneath the HUD.

```lua
local logo = Image.Load("client/logo.png")
logo.layer = 0 -- default: on top of everything
```

DrawHUD and DrawMenu are separate passes, each flushing its own queue around the game's drawing.
