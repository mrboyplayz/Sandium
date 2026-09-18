#include "../Sandium/PaperdollGeometry.hpp"
#include <cassert>
#include <limits>
#include <cstdio>

int main()
{
    using namespace paperdoll;
    for (float angle : {0.0f, 0.37f, 1.5707963f, 3.0f})
    {
        const Point axis{std::cos(angle), std::sin(angle)};
        const Point cross{axis.y, -axis.x};
        const Point center{400, 520};
        const auto point = [&](float x, float y) {
            return Point{center.x + cross.x * x + axis.x * y,
                         center.y + cross.y * x + axis.y * y};
        };
        // Capsule: 12-pixel body + 4-pixel radius, rotated and translated.
        for (auto shape : {Shape::Circles, Shape::Squares, Shape::Triangles, Shape::Octagons})
        {
            const auto polygon = MakePolygon(shape, point(4, 6), point(0, 10), point(0, -10));
            const int expected = shape == Shape::Circles ? 0 : shape == Shape::Squares ? 4 : shape == Shape::Triangles ? 3 : 8;
            assert(polygon.count == expected);
            if (!expected) continue;
            assert(std::hypot(polygon.center.x - center.x, polygon.center.y - center.y) < 0.001f);
            float area = 0;
            for (int i = 0; i < polygon.count; ++i)
            {
                const Point p{polygon.points[i].x - center.x, polygon.points[i].y - center.y};
                const Point q{polygon.points[(i + 1) % polygon.count].x - center.x,
                              polygon.points[(i + 1) % polygon.count].y - center.y};
                assert(std::abs(p.x * cross.x + p.y * cross.y) <= 4.001f);
                assert(std::abs(p.x * axis.x + p.y * axis.y) <= 10.001f);
                const float triangleArea = p.x * q.y - p.y * q.x;
                assert(triangleArea > 0); // Non-degenerate, consistently wound injury-fill triangles.
                area += triangleArea * 0.5f;
            }
            const float expectedArea = shape == Shape::Squares ? 160.0f : shape == Shape::Triangles ? 80.0f : 132.54834f;
            assert(std::abs(area - expectedArea) < 0.02f);
        }
    }
    assert(MakePolygon(Shape::Squares, {0, 0}, {0, 0}, {0, 0}).count == 0);
    assert(MakePolygon(Shape::Squares, {0, 0}, {0, 1}, {0, -1}).count == 0);
    assert(MakePolygon(Shape::Triangles, {1, 0}, {0, std::numeric_limits<float>::quiet_NaN()}, {0, 0}).count == 0);
    std::puts("Paperdoll geometry: all shapes, rotations, bounds, fill winding and degenerate inputs passed.");
}
