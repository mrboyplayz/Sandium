#pragma once

#include <array>
#include <cmath>

namespace paperdoll
{
    enum class Shape { Circles, Squares, Triangles, Octagons };
    struct Point { float x, y; };
    struct Polygon { std::array<Point, 8> points{}; int count = 0; Point center{}; };

    // Samples 0, 16, 32 and 48 of the game's capsule outline, in order.
    // Derive the bone's local axes so every shape follows the original pose.
    inline Polygon MakePolygon(Shape shape, Point side, Point tip, Point oppositeTip)
    {
        Polygon result;
        if (shape == Shape::Circles)
            return result;
        result.center = {(tip.x + oppositeTip.x) * 0.5f, (tip.y + oppositeTip.y) * 0.5f};
        const float dx = tip.x - oppositeTip.x, dy = tip.y - oppositeTip.y;
        const float length = std::hypot(dx, dy);
        if (!std::isfinite(length) || length < 0.001f)
            return result;
        const Point along{dx / length, dy / length};
        const Point across{along.y, -along.x};
        const float radius = std::abs((side.x - result.center.x) * across.x +
                                     (side.y - result.center.y) * across.y);
        if (!std::isfinite(radius) || radius < 0.001f)
            return result;
        std::array<Point, 8> local{};
        if (shape == Shape::Squares)
        {
            local = {{{1, 1}, {-1, 1}, {-1, -1}, {1, -1}}};
            result.count = 4;
        }
        else if (shape == Shape::Triangles)
        {
            local = {{{0, 1}, {-1, -1}, {1, -1}}};
            result.count = 3;
        }
        else if (shape == Shape::Octagons)
        {
            constexpr float corner = 0.41421356237f;
            local = {{{corner, 1}, {-corner, 1}, {-1, corner}, {-1, -corner},
                      {-corner, -1}, {corner, -1}, {1, -corner}, {1, corner}}};
            result.count = 8;
        }
        for (int i = 0; i < result.count; ++i)
        {
            result.points[i] = {
                result.center.x + across.x * radius * local[i].x + along.x * length * 0.5f * local[i].y,
                result.center.y + across.y * radius * local[i].x + along.y * length * 0.5f * local[i].y
            };
        }
        return result;
    }
}
