#include "BloodMarks.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <mutex>
#include <vector>

namespace api::bloodmarks
{
    namespace
    {
        struct Mark
        {
            float x, y, z, radius;
            std::chrono::steady_clock::time_point created;
        };
        std::vector<Mark> marks;
        std::mutex marksMutex;
        constexpr std::size_t maxMarks = 24;
        constexpr auto lifetime = std::chrono::seconds(180);
    }

    void Add(float x, float y, float z, float normalY)
    {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
            normalY < 0.45f)
            return;
        std::lock_guard<std::mutex> lock(marksMutex);
        const auto now = std::chrono::steady_clock::now();
        for (auto &mark : marks)
        {
            const float dx = mark.x - x, dz = mark.z - z;
            if (dx * dx + dz * dz < 0.10f && std::abs(mark.y - y) < 0.12f)
            {
                mark.radius = std::min(0.82f, mark.radius + 0.03f);
                mark.created = now;
                return;
            }
        }
        if (marks.size() >= maxMarks)
            marks.erase(marks.begin());
        const float radius = 0.27f + static_cast<float>(marks.size() % 5) * 0.055f;
        marks.push_back({x, y, z, radius, now});
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lock(marksMutex);
        marks.clear();
    }

    void FillUniforms(float *values, int slots)
    {
        std::fill(values, values + slots * 4, 0.0f);
        std::lock_guard<std::mutex> lock(marksMutex);
        const auto now = std::chrono::steady_clock::now();
        marks.erase(std::remove_if(marks.begin(), marks.end(), [&](const Mark &mark) {
            return now - mark.created > lifetime;
        }), marks.end());
        const int count = std::min(slots, static_cast<int>(marks.size()));
        for (int i = 0; i < count; ++i)
        {
            const auto &mark = marks[marks.size() - count + i];
            values[i * 4 + 0] = mark.x;
            values[i * 4 + 1] = mark.y;
            values[i * 4 + 2] = mark.z;
            values[i * 4 + 3] = mark.radius;
        }
    }
}
