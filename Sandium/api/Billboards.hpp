#pragma once

#include <memory>

namespace api
{
    class Image;
    class Video;

    // A world-anchored quad that renders an Image or Video every frame.
    // Position/size are in world units; the projected rect is stretched to
    // width x height, faded by opacity, and hidden beyond maxDistance
    // (0 = unlimited). Drawn from the HUD hook's Lua-drawing window.
    class Billboard
    {
    public:
        Billboard() = default;
        ~Billboard();

        float x = 0.0f, y = 0.0f, z = 0.0f;
        float width = 2.0f;
        float height = 1.5f;
        float maxDistance = 0.0f; // 0 = unlimited
        float opacity = 1.0f;
        bool visible = true;

        std::shared_ptr<Image> image;
        std::shared_ptr<Video> video;

        void Destroy();
    };

    namespace billboards
    {
        // Registry ops (Lua-facing wrappers live in LuaManager).
        void Register(const std::shared_ptr<Billboard> &billboard);
        void Destroy(Billboard *billboard);
        void Clear();

        // Per-frame projection + draw. Call inside the Lua drawing window.
        void DrawFrame();
    }
}
