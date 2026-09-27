#pragma once

namespace api::bloodmarks
{
    void Add(float x, float y, float z, float normalY);
    void Clear();
    void FillUniforms(float *values, int slots);
}
