#include "GrainShader.hpp"

#include "PainShader.hpp"

#include <glad/glad.h>

#include <cmath>
#include <chrono>

// Faithful GLSL port of the decompiled Source grain shaders. The original
// ps_2_x sequence: two frac(time*k+0.5) -> sincos chains build a per-frame
// random offset, pixel coords * 1024 + the offset feed the classic
// frac(sin(dot(co, consts)) * big) white-noise hash, and the result is
// weighted by 1/distance^2 from screen center (stronger grain at the edges).
// The white variant outputs pure grain; we draw it additively over the
// finished frame, intensity driven by the pain level.

namespace grainshader
{
    namespace
    {
        bool ready = false;
        bool failed = false;
        GLuint program = 0;
        GLint timeLocation = -1, amountLocation = -1;
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

            // fullscreen triangle from gl_VertexID, no buffers
            const char *vs = "#version 330 core\n"
                             "out vec2 vUv;\n"
                             "void main(){\n"
                             "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
                             "  vUv = p;\n"
                             "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
                             "}\n";
            const char *fs = "#version 330 core\n"
                             "in vec2 vUv;\n"
                             "uniform vec2 uTime;    // x: seconds, y: sub-second seed\n"
                             "uniform float uAmount; // grain strength 0..1\n"
                             "out vec4 color;\n"
                             "float rand(vec2 co){ return fract(sin(dot(co, vec2(12.9898, 78.233))) * 43758.5453); }\n"
                             "void main(){\n"
                             "  // per-frame jitter (the decompiled sincos seed chains)\n"
                             "  float j = rand(vec2(uTime.x * 2.72154951, uTime.y)) * 13.0;\n"
                             "  vec2 coord = vUv * 1024.0 + j;\n"
                             "  float n = rand(coord);\n"
                             "  // 1/d^2 weighting from screen center: grain lives at the edges\n"
                             "  vec2 d = vUv - 0.5;\n"
                             "  float r2 = dot(d, d);\n"
                             "  float weight = 0.25 + 1.0 / (0.08 + r2 * 4.0);\n"
                             "  float g = (n - 0.5) * uAmount * weight * 0.35 + 0.018 * uAmount * weight;\n"
                             "  color = vec4(max(g, 0.0), max(g, 0.0), max(g, 0.0), 1.0);\n"
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
            ready = true;
            return true;
        }
    }

    void Draw()
    {
#if _WIN32
        if (!EnsureProgram())
            return;

        const float pain = painshader::Intensity();
        // tiny cinematic base, heavy grain as pain rises
        const float amount = 0.02f + pain * 0.55f;
        if (amount < 0.021f)
            return;

        const float seconds = std::chrono::duration<float>(
                                  std::chrono::steady_clock::now() - started).count();

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
        glBlendFunc(GL_ONE, GL_ONE); // additive white speckle (grainwhite)
        glUseProgram(program);
        if (timeLocation >= 0)
            glUniform2f(timeLocation, seconds, seconds - std::floor(seconds));
        if (amountLocation >= 0)
            glUniform1f(amountLocation, amount);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);

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
