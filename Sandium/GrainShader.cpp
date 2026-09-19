#include "GrainShader.hpp"

#include "PainShader.hpp"

#include <glad/glad.h>

#include <cmath>
#include <cstdio>
#include <chrono>

// Fullscreen post pass: red pain vignette + two-pass film grain (the
// decompiled Source zb_grain/zb_grainwhite logic: per-frame seeded noise,
// pixel-locked hash, edge weighting). Everything is computed in real
// viewport space so the vignette center is always the true screen center,
// and the vignette is oversized + slowly wobbling at high pain (Homigrad
// style) so its edge can never be seen.

namespace grainshader
{
    namespace
    {
        bool ready = false;
        bool failed = false;
        bool reported = false;
        GLuint program = 0;
        GLuint vao = 0;
        GLint timeLocation = -1, amountLocation = -1, modeLocation = -1,
              aspectLocation = -1, painLocation = -1;
        const auto started = std::chrono::steady_clock::now();

        GLuint Compile(GLenum type, const char *source)
        {
            GLuint shader = glCreateShader(type);
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);
            GLint okay = 0;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &okay);
            if (!okay)
            {
                glDeleteShader(shader);
                return 0;
            }
            return shader;
        }

        bool EnsureProgram()
        {
            if (ready)
                return true;
            if (failed)
                return false;

            const char *vs = "#version 330 core\n"
                             "out vec2 vUv;\n"
                             "void main(){\n"
                             "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
                             "  vUv = p;\n"
                             "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
                             "}\n";
            const char *fs = "#version 330 core\n"
                             "in vec2 vUv;\n"
                             "uniform vec2 uTime;\n"
                             "uniform float uAmount;  // grain strength\n"
                             "uniform float uMode;    // 0 vignette, 1 white grain, 2 dark grain\n"
                             "uniform float uAspect;  // width / height\n"
                             "uniform float uPain;    // pain 0..1\n"
                             "out vec4 color;\n"
                             "float hash(vec2 p){\n"
                             "  p = fract(p * vec2(123.34, 456.21));\n"
                             "  p += dot(p, p + 45.32);\n"
                             "  return fract(p.x * p.y);\n"
                             "}\n"
                             "void main(){\n"
                             "  if (uMode < 0.5){\n"
                             "    vec2 uv = (vUv - 0.5) * 1.45 + 0.5;\n"
                             "    float t = uTime.x;\n"
                             "    float wob = uPain * uPain;\n"
                             "    float ang = (sin(t * 0.7) + sin(t * 0.43 + 1.7)) * 0.035 * wob;\n"
                             "    float ca = cos(ang), sa = sin(ang);\n"
                             "    uv -= 0.5;\n"
                             "    uv = vec2(uv.x * ca - uv.y * sa, uv.x * sa + uv.y * ca);\n"
                             "    uv += 0.5;\n"
                             "    uv += vec2(sin(t * 0.9), cos(t * 0.61)) * 0.012 * wob;\n"
                             "    vec2 d = (uv - 0.5) * vec2(uAspect, 1.0);\n"
                             "    float dist = length(d);\n"
                             "    float inner = mix(0.85, 0.08, uPain);\n"
                             "    float a = smoothstep(inner, inner + 0.55, dist) * uPain;\n"
                             "    a += smoothstep(0.95, 1.25, dist) * uPain * 0.4;\n"
                             "    color = vec4(vec3(0.52, 0.01, 0.01), clamp(a, 0.0, 0.92));\n"
                             "    return;\n"
                             "  }\n"
                             "  float j = hash(vec2(uTime.x * 2.72154951, uTime.y)) * 13.0;\n"
                             "  vec2 coord = floor(vUv * 1024.0) + j;\n"
                             "  float n = hash(coord);\n"
                             "  vec2 d = vUv - 0.5;\n"
                             "  float r2 = dot(d, d);\n"
                             "  float weight = mix(0.6, 1.6, smoothstep(0.2, 0.75, sqrt(r2) * 2.0));\n"
                             "  float g = (n - 0.5) * uAmount * weight;\n"
                             "  if (uMode > 1.5)\n"
                             "    color = vec4(1.0) - vec4(max(-g, 0.0) * 0.8);\n"
                             "  else\n"
                             "    color = vec4(max(g, 0.0) * 0.8);\n"
                             "}\n";

            GLuint v = Compile(GL_VERTEX_SHADER, vs);
            GLuint f = Compile(GL_FRAGMENT_SHADER, fs);
            if (!v || !f)
            {
                failed = true;
                return false;
            }
            program = glCreateProgram();
            glAttachShader(program, v);
            glAttachShader(program, f);
            glLinkProgram(program);
            glDeleteShader(v);
            glDeleteShader(f);
            GLint linked = 0;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            if (!linked)
            {
                failed = true;
                return false;
            }
            timeLocation = glGetUniformLocation(program, "uTime");
            amountLocation = glGetUniformLocation(program, "uAmount");
            modeLocation = glGetUniformLocation(program, "uMode");
            aspectLocation = glGetUniformLocation(program, "uAspect");
            painLocation = glGetUniformLocation(program, "uPain");
            glGenVertexArrays(1, &vao);
            ready = vao != 0;
            return ready;
        }

        void DrawPass(float mode, float seconds, float amount, float aspect, float pain)
        {
            if (modeLocation >= 0)
                glUniform1f(modeLocation, mode);
            if (amountLocation >= 0)
                glUniform1f(amountLocation, amount);
            if (aspectLocation >= 0)
                glUniform1f(aspectLocation, aspect);
            if (painLocation >= 0)
                glUniform1f(painLocation, pain);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }

    void Draw()
    {
#if _WIN32
        if (!EnsureProgram())
            return;

        const float pain01 = painshader::Intensity(); // 0..1
        const float grainAmount = pain01 * 0.55f;
        const float seconds = std::chrono::duration<float>(
                                  std::chrono::steady_clock::now() - started).count();

        if (!reported)
        {
            reported = true;
            if (std::FILE *f = std::fopen("sandium_grain.txt", "w"))
            {
                std::fprintf(f, "ready=%d program=%u vao=%u pain=%.2f\n",
                             ready ? 1 : 0, program, vao, pain01);
                std::fclose(f);
            }
        }

        GLint viewport[4] = {};
        glGetIntegerv(GL_VIEWPORT, viewport);
        const float aspect = viewport[3] > 0 ? static_cast<float>(viewport[2]) / static_cast<float>(viewport[3]) : 1.0f;

        GLint previousProgram = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
        GLint previousVao = 0, previousVbo = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousVbo);
        GLboolean previousBlend = glIsEnabled(GL_BLEND);
        GLint previousSrc = 0, previousDst = 0;
        glGetIntegerv(GL_BLEND_SRC_RGB, &previousSrc);
        glGetIntegerv(GL_BLEND_DST_RGB, &previousDst);
        GLint previousActive = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActive);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glEnable(GL_BLEND);
        glUseProgram(program);
        if (timeLocation >= 0)
            glUniform2f(timeLocation, seconds, seconds - std::floor(seconds));
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        if (pain01 > 0.02f)
        {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            DrawPass(0.0f, seconds, 0.0f, aspect, pain01);
        }
        if (grainAmount > 0.03f)
        {
            glBlendFunc(GL_ONE, GL_ONE);
            DrawPass(1.0f, seconds, grainAmount, aspect, pain01);
            glBlendFunc(GL_ZERO, GL_SRC_COLOR);
            DrawPass(2.0f, seconds, grainAmount, aspect, pain01);
        }

        glUseProgram(static_cast<GLuint>(previousProgram));
        glBindVertexArray(static_cast<GLuint>(previousVao));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousVbo));
        if (previousBlend)
            glEnable(GL_BLEND);
        else
            glDisable(GL_BLEND);
        glBlendFunc(static_cast<GLenum>(previousSrc), static_cast<GLenum>(previousDst));
        glActiveTexture(static_cast<GLenum>(previousActive));
#endif
    }
}
