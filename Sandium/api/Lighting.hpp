#pragma once

#include <memory>

// Lua-controllable light sources. The engine's shaders only support a single
// sun direction (lightvec/lightambient/lightdiffuse) — there is no point-light
// machinery to feed — so these are rendered by Sandium itself: additive glow
// billboards plus optional ground pools, depth-tested against the world so
// walls occlude them. DrawFrame is called from the DrawHUD hook.
namespace api
{
    class Light
    {
    public:
        ~Light();
        void Destroy();
        void SetPosition(float x, float y, float z);
        void SetColor(float r, float g, float b);
        void SetRadius(float radius);
        void SetIntensity(float intensity);
        // Follow a human's position each frame with a fixed offset.
        void SetFollow(int humanIndex, float offsetX, float offsetY, float offsetZ);
        void SetGroundPool(bool enable);
        void SetSourceVisible(bool enable);
        // Spot light: beam direction (world, normalized internally) and cone
        // half-angle in degrees. angle 0 = omnidirectional point light.
        void SetDirection(float dx, float dy, float dz);
        void SetCone(float angleDegrees);

        float x = 0.0f, y = 1.0f, z = 0.0f;
        float red = 1.0f, green = 1.0f, blue = 1.0f;
        float radius = 2.0f;
        float intensity = 1.0f;
        // Draw a small glowing bulb at the light's exact position (visible
        // through walls — it is a position marker, not an occluding object).
        bool source = true;
        bool groundPool = false;
        bool follow = false;
        int humanIndex = -1;
        float offsetX = 0.0f, offsetY = 1.2f, offsetZ = 0.0f;
        // Spot parameters (cone >= 180 means omni).
        float dirX = 0.0f, dirY = 0.0f, dirZ = 1.0f;
        float coneDegrees = 180.0f;
    };

    namespace lighting
    {
        void Register(const std::shared_ptr<Light> &light);
        void Destroy(Light *light);
        void Clear();
        // Master brightness multiplier applied to every light (default 1).
        // Hotkeys [ and ] adjust it live in-game; Lua can set it too.
        void SetBrightness(float brightness);
        float GetBrightness();
        void DrawFrame();
        // Diagnostic: sample the presented frame's brightness grid and log
        // the hottest cells vs the nearest light's screen projection. Runs
        // only while sandium_lightprobe.txt exists (create/delete to toggle).
        void ProbeFrame();
        void Shutdown();
    }
}
