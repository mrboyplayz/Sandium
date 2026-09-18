#include "Billboards.hpp"

#include "Image.hpp"
#include "Video.hpp"
#include "GLUniforms.hpp"

#include "../Addresses.hpp"

#include <cmath>
#include <vector>

#if _WIN32
#include <Windows.h>
#undef DrawText
#endif

namespace api
{
    namespace
    {
        std::vector<std::shared_ptr<Billboard>> &Registry()
        {
            static std::vector<std::shared_ptr<Billboard>> registry;
            return registry;
        }

        // Sub Rosa's image/video layer UI space (matches the Lua Image API).
        constexpr float UI_WIDTH = 1024.0f;
        constexpr float UI_HEIGHT = 768.0f;
        // anything farther than this from the doll/camera anchor is treated
        // as unprojectable rather than wildly stretched
        constexpr float MAX_STRETCH_PX = 4096.0f;

        // clip = matrix * point, honoring the shader's transpose flag
        bool ProjectPoint(const float *m, bool transposed, const float point[3], float ndcOut[2])
        {
            float clip[4];
            for (int row = 0; row < 4; ++row)
            {
                clip[row] = transposed
                    ? m[row * 4 + 0] * point[0] + m[row * 4 + 1] * point[1] + m[row * 4 + 2] * point[2] + m[row * 4 + 3]
                    : m[0 * 4 + row] * point[0] + m[1 * 4 + row] * point[1] + m[2 * 4 + row] * point[2] + m[3 * 4 + row];
            }
            if (clip[3] < 0.05f)
                return false;
            ndcOut[0] = clip[0] / clip[3];
            ndcOut[1] = clip[1] / clip[3];
            return true;
        }

        // screen-space size of a world-space extent at a world position
        bool ExtentToPixels(const float *m, bool transposed, const float origin[3],
                            float dxWorld, float dyWorld, float &pixels)
        {
            const float end[3] = {origin[0] + dxWorld, origin[1] + dyWorld, origin[2]};
            float a[2], b[2];
            if (!ProjectPoint(m, transposed, origin, a) || !ProjectPoint(m, transposed, end, b))
                return false;
            const float ax = (a[0] * 0.5f + 0.5f) * UI_WIDTH;
            const float ay = (1.0f - (a[1] * 0.5f + 0.5f)) * UI_HEIGHT;
            const float bx = (b[0] * 0.5f + 0.5f) * UI_WIDTH;
            const float by = (1.0f - (b[1] * 0.5f + 0.5f)) * UI_HEIGHT;
            const float ex = bx - ax, ey = by - ay;
            pixels = std::sqrt(ex * ex + ey * ey);
            return pixels > 0.5f && pixels < MAX_STRETCH_PX;
        }
    }

    Billboard::~Billboard() = default;

    void Billboard::Destroy()
    {
        billboards::Destroy(this);
    }

    namespace billboards
    {
        void Register(const std::shared_ptr<Billboard> &billboard)
        {
            Registry().push_back(billboard);
        }

        void Destroy(Billboard *billboard)
        {
            auto &registry = Registry();
            for (auto it = registry.begin(); it != registry.end(); ++it)
            {
                if (it->get() == billboard)
                {
                    registry.erase(it);
                    return;
                }
            }
        }

        void Clear()
        {
            Registry().clear();
        }

        void DrawFrame()
        {
            if (!glcap::HasViewProjection())
                return;
            const float *matrix = glcap::ViewProjectionMatrix();
            const bool transposed = glcap::MatrixTransposed();
            const float *camera = glcap::ViewPosition();

            for (const auto &billboard : Registry())
            {
                if (!billboard || !billboard->visible)
                    continue;
                if (!billboard->image && !billboard->video)
                    continue;
                if (billboard->width <= 0.0f || billboard->height <= 0.0f)
                    continue;

                const float position[3] = {billboard->x, billboard->y, billboard->z};
                const float dx = position[0] - camera[0];
                const float dy = position[1] - camera[1];
                const float dz = position[2] - camera[2];
                const float distanceSquared = dx * dx + dy * dy + dz * dz;
                if (billboard->maxDistance > 0.0f && distanceSquared > billboard->maxDistance * billboard->maxDistance)
                    continue;

                float centerNdc[2];
                if (!ProjectPoint(matrix, transposed, position, centerNdc))
                    continue;

                float widthPx = 0.0f, heightPx = 0.0f;
                if (!ExtentToPixels(matrix, transposed, position, billboard->width, 0.0f, widthPx))
                    continue;
                if (!ExtentToPixels(matrix, transposed, position, 0.0f, billboard->height, heightPx))
                    continue;

                const float px = (centerNdc[0] * 0.5f + 0.5f) * UI_WIDTH - widthPx * 0.5f;
                const float py = (1.0f - (centerNdc[1] * 0.5f + 0.5f)) * UI_HEIGHT - heightPx * 0.5f;
                const float opacity = billboard->opacity;

                if (billboard->video)
                {
                    if (billboard->video->IsValid() && !billboard->video->IsPlaying())
                        billboard->video->Play(); // loop
                    if (billboard->video->IsValid())
                        billboard->video->Draw(px, py, widthPx, heightPx, 1.0f, 1.0f, 1.0f, opacity);
                }
                else if (billboard->image && billboard->image->IsValid())
                {
                    billboard->image->Draw(px, py, widthPx, heightPx, 1.0f, 1.0f, 1.0f, opacity);
                }
            }
        }
    }
}
