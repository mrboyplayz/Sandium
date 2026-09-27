#include "ShaderLights.hpp"

#include "Addresses.hpp"
#include "Diagnostics.hpp"

#include <subhook.h>

#if _WIN32
#include <Windows.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

// The resolver stores every resolved GL pointer right after its
// `lea rcx,"name"; call loader` pair. Static verification on this exact exe:
// the store following the "glShaderSource" load lands at RVA 0x4042D8, and
// the one following "glCompileShader" at RVA 0x4042C8.
constexpr std::uintptr_t resolverRva = 0xE42E0;
constexpr std::uintptr_t shaderSourceSlotRva = 0x4042D8;
constexpr std::uintptr_t compileShaderSlotRva = 0x4042C8;

namespace
{
    using ShaderSourceFn = void (*)(unsigned int, int, const char **, const int *);
    using ResolverFn = void *(*)(void *(*)(const char *));

    ShaderSourceFn realShaderSource = nullptr;
    ResolverFn realResolver = nullptr;
    subhook::Hook resolverHook;

    using CompileShaderFn = void (*)(unsigned int);
    using GetShaderivFn = void (*)(unsigned int, unsigned int, int *);
    using GetShaderInfoLogFn = void (*)(unsigned int, int, int *, char *);
    CompileShaderFn realCompileShader = nullptr;
    GetShaderivFn getShaderiv = nullptr;
    GetShaderInfoLogFn getShaderInfoLog = nullptr;

    void FakeCompileShader(unsigned int shader)
    {
        if (!realCompileShader)
            return;
        realCompileShader(shader);
        if (getShaderiv && getShaderInfoLog)
        {
            int status = 0;
            getShaderiv(shader, 0x8B81 /* GL_COMPILE_STATUS */, &status);
            if (!status)
            {
                char log[2048] = {};
                int length = 0;
                getShaderInfoLog(shader, sizeof(log) - 1, &length, log);
                diag::Log("shaderlights", "SHADER %u FAILED TO COMPILE: %.1800s",
                          shader, log);
            }
        }
    }

    // Point-light GLSL added to every world fragment shader. Unused slots
    // keep radius (w) at 0, which makes the attenuation term vanish.
    // kMaxShaderLights must match the array sizes here AND the upload count
    // in Lighting.cpp.
    const char *declarations =
        "uniform vec4 lightpoints[32];\n"
        "uniform vec4 lightcolors[32];\n"
        "uniform vec4 lightspots[32];\n"
        "uniform vec4 bloodmarks[24];\n";

    std::string BuildInjectedSource(const std::string &source)
    {
        // Output variable name: "out vec4 <name>;"
        const std::size_t outPos = source.find("out vec4 ");
        if (outPos == std::string::npos)
            return source;
        if (source.find("viewvec") == std::string::npos ||
            source.find("normalvec") == std::string::npos)
            return source;
        std::size_t nameStart = outPos + 9;
        while (nameStart < source.size() && source[nameStart] == ' ')
            ++nameStart;
        std::size_t nameEnd = nameStart;
        while (nameEnd < source.size() &&
               (std::isalnum(static_cast<unsigned char>(source[nameEnd])) || source[nameEnd] == '_'))
            ++nameEnd;
        if (nameEnd == nameStart)
            return source;
        const std::string outName = source.substr(nameStart, nameEnd - nameStart);

        // World position: newer world shaders pass it directly; for the rest
        // recover it from viewvec, whose sign differs on the landscape pass.
        // The same sign difference decides the view direction.
        bool landscape = source.find("land_usemask") != std::string::npos ||
                         source.find("tex_mask") != std::string::npos;
        std::string worldPosition;
        std::string viewDirection;
        if (source.find("in vec3 wsposition") != std::string::npos)
        {
            worldPosition = "wsposition";
            viewDirection = "normalize(viewvec)";
        }
        else if (landscape)
        {
            worldPosition = "(viewposition + viewvec)";
            viewDirection = "normalize(-viewvec)";
        }
        else
        {
            worldPosition = "(viewposition - viewvec)";
            viewDirection = "normalize(viewvec)";
        }

        std::string apply;
        apply += "{\n";
        apply += "  vec3 splposition = " + worldPosition + ";\n";
        apply += "  vec3 splnormal = normalize(normalvec);\n";
        apply += "  vec3 splviewdir = " + viewDirection + ";\n";
        apply += "  vec3 splillumination = vec3(0.0);\n";
        apply += "  for (int spli = 0; spli < 32; spli++)\n";
        apply += "  {\n";
        apply += "    vec3 spldelta = lightpoints[spli].xyz - splposition;\n";
        apply += "    float spldist = length(spldelta);\n";
        apply += "    float splrange = max(lightpoints[spli].w, 0.001);\n";
        apply += "    if (spldist >= splrange) continue;\n";
        apply += "    float splx = spldist / splrange;\n";
        apply += "    // Broad, smooth falloff keeps the beam useful away from its center.\n";
        apply += "    float splwindow = max(1.0 - splx * splx, 0.0);\n";
        apply += "    float splatt = splwindow * splwindow / (1.0 + 2.0 * splx * splx);\n";
        apply += "    vec3 spldir = spldelta / max(spldist, 0.0001);\n";
        apply += "    // spot cone: lightspots[spli] = normalized dir + cos(halfAngle);\n";
        apply += "    // w = -2 means omnidirectional (plain point light)\n";
        apply += "    float splcone = lightspots[spli].w;\n";
        apply += "    if (splcone > -1.5)\n";
        apply += "    {\n";
        // spldir points from the fragment toward the light. The cone axis
        // points outward from the light, toward the fragment.
        apply += "      float splaxis = dot(-spldir, lightspots[spli].xyz);\n";
        apply += "      if (splaxis <= splcone) continue;\n";
        apply += "      float splinner = mix(splcone, 1.0, 0.75);\n";
        apply += "      splatt *= smoothstep(splcone, splinner, splaxis);\n";
        apply += "      // The focused beam covers a smaller patch up close.\n";
        apply += "      // Increase nearby illumination without an unbounded hotspot.\n";
        apply += "      splatt *= 1.0 + 3.0 / (1.0 + 0.8 * spldist * spldist);\n";
        apply += "    }\n";
        apply += "    // wrap diffuse: surfaces facing away keep a soft fill\n";
        apply += "    float splndl = clamp(dot(splnormal, spldir) * 0.7 + 0.3, 0.0, 1.0);\n";
        apply += "    // Keep highlights subtle; a white additive glint erases surface detail.\n";
        apply += "    vec3 splhalf = normalize(spldir + splviewdir + vec3(0.00001));\n";
        apply += "    float splspec = pow(max(dot(splnormal, splhalf), 0.0), 48.0) * 0.08;\n";
        apply += "    vec3 spllight = lightcolors[spli].rgb * (lightcolors[spli].w * splatt * 2.0);\n";
        apply += "    splillumination += spllight * (splndl + splspec);\n";
        apply += "  }\n";
        apply += "  // Ground blood is evaluated on the actual world surface, so\n";
        apply += "  // walls and geometry occlude it without a screen-space sprite.\n";
        apply += "  if (splnormal.y > 0.45)\n";
        apply += "  {\n";
        apply += "    float splblood = 0.0;\n";
        apply += "    for (int splm = 0; splm < 24; ++splm)\n";
        apply += "    {\n";
        apply += "      vec4 splmark = bloodmarks[splm];\n";
        apply += "      if (splmark.w <= 0.0) continue;\n";
        apply += "      vec2 sploffset = (splposition.xz - splmark.xz) / splmark.w;\n";
        apply += "      float spldist = length(sploffset);\n";
        apply += "      if (spldist > 1.2) continue;\n";
        apply += "      float splrotation = fract(sin(dot(splmark.xyz, vec3(12.9898, 78.233, 39.425))) * 43758.5453) * 6.2831853;\n";
        apply += "      float splc = cos(splrotation), spls = sin(splrotation);\n";
        apply += "      vec2 splrotated = vec2(splc * sploffset.x - spls * sploffset.y, spls * sploffset.x + splc * sploffset.y);\n";
        apply += "      float splangle = atan(splrotated.y, splrotated.x);\n";
        apply += "      float spledge = 0.78 + 0.10 * sin(splangle * 5.0 + 0.6) + 0.06 * sin(splangle * 9.0 + 1.7);\n";
        apply += "      float splshape = 1.0 - smoothstep(spledge - 0.09, spledge + 0.04, spldist);\n";
        apply += "      float spldrop = 1.0 - smoothstep(0.10, 0.23, length(splrotated - vec2(0.91, 0.18)));\n";
        apply += "      splshape = max(splshape, spldrop * 0.72);\n";
        apply += "      float splheight = 1.0 - smoothstep(0.04, 0.15, abs(splposition.y - splmark.y));\n";
        apply += "      splblood = max(splblood, splshape * splheight);\n";
        apply += "    }\n";
        apply += "    // Blood is a surface pigment. Apply it before illumination so\n";
        apply += "    // the same light and shadow response affects floor and stain.\n";
        apply += "    " + outName + ".rgb = mix(" + outName + ".rgb, " + outName + ".rgb * vec3(0.36, 0.045, 0.045) + vec3(0.08, 0.0, 0.0), splblood * 0.9);\n";
        apply += "  }\n";
        apply += "  // Apply light to the shaded material color. Compress stacked lights\n";
        apply += "  // so nearby surfaces brighten without becoming solid white.\n";
        apply += "  if (dot(splillumination, vec3(1.0)) > 0.0001)\n";
        apply += "  {\n";
        apply += "  vec3 splgain = 1.5 * splillumination / (vec3(2.0) + splillumination);\n";
        apply += "  vec3 spllit = " + outName + ".rgb * (vec3(1.0) + splgain);\n";
        apply += "  float splpeak = max(spllit.r, max(spllit.g, spllit.b));\n";
        apply += "  " + outName + ".rgb = spllit * min(1.0, 0.98 / max(splpeak, 0.001));\n";
        apply += "  }\n";
        // The shadow NPC texture is opaque black with an alpha marker of
        // 254/255. Force its final color to black after native PBR, cubemap
        // reflections and our point lights have all run. Other materials keep
        // their ordinary lighting, and the NPC still writes normal depth.
        if (source.find("uniform sampler2DArray tex_color") != std::string::npos &&
            source.find("flat in int texlevel") != std::string::npos)
        {
            apply += "  vec4 splshadow = texture(tex_color, vec3(texcoord.xy, texlevel));\n";
        }
        else if (source.find("uniform sampler2D tex_color") != std::string::npos &&
                 source.find("texcoord") != std::string::npos)
        {
            apply += "  vec4 splshadow = texture(tex_color, texcoord.xy);\n";
        }
        else
        {
            apply += "  vec4 splshadow = vec4(1.0);\n";
        }
        apply += "  if (all(lessThan(splshadow.rgb, vec3(0.0001))) && "
                 "splshadow.a > 0.994 && splshadow.a < 0.999) " + outName + ".rgb = vec3(0.0);\n";
        if (source.find("uniform int flags;") != std::string::npos)
            apply += "  if ((flags & 268435456) != 0) " + outName + ".rgb = vec3(0.0);\n";
        apply += "}\n";

        // Declarations go right after the #version line. The lighting block
        // goes at the end of main(), located by brace counting — the game's
        // runtime sources indent every closing brace, so searching for a
        // column-zero "\n}" never matches.
        const std::size_t lineEnd = source.find('\n');
        if (lineEnd == std::string::npos)
            return source;
        const char *viewDeclaration = source.find("uniform vec3 viewposition") == std::string::npos
            ? "uniform vec3 viewposition;\n" : "";
        std::string result = source.substr(0, lineEnd + 1) + viewDeclaration +
                             declarations + source.substr(lineEnd + 1);

        const std::size_t mainPos = result.find("void main");
        if (mainPos == std::string::npos)
            return source;
        const std::size_t mainOpen = result.find('{', mainPos);
        if (mainOpen == std::string::npos)
            return source;
        int depth = 0;
        std::size_t mainClose = std::string::npos;
        for (std::size_t i = mainOpen; i < result.size(); ++i)
        {
            if (result[i] == '{') ++depth;
            else if (result[i] == '}')
            {
                --depth;
                if (depth == 0)
                {
                    mainClose = i;
                    break;
                }
            }
        }
        if (mainClose == std::string::npos)
            return source;
        result.insert(mainClose, apply);
        return result;
    }

    void FakeShaderSource(unsigned int shader, int count, const char **strings, const int *lengths)
    {
        std::string combined;
        for (int i = 0; i < count; ++i)
        {
            if (!strings || !strings[i])
                continue;
            combined.append(strings[i], lengths && lengths[i] >= 0
                ? strings[i] + lengths[i]
                : strings[i] + std::strlen(strings[i]));
        }

        // Fragment shaders of the lit world passes only (vertex shaders carry
        // gl_Position; the sky/particle passes have no lightvec).
        if (combined.find("lightvec") != std::string::npos &&
            combined.find("gl_Position") == std::string::npos &&
            combined.find("normalvec") != std::string::npos &&
            combined.find("lightpoints") == std::string::npos)
        {
            const std::string injected = BuildInjectedSource(combined);
            const char *pointer = injected.c_str();
            const int length = static_cast<int>(injected.size());
            if (realShaderSource)
                realShaderSource(shader, 1, &pointer, &length);
            static bool dumped = false;
            if (!dumped)
            {
                dumped = true;
                if (std::FILE *dump = std::fopen("sandium_injected.txt", "w"))
                {
                    std::fwrite(injected.data(), 1, injected.size(), dump);
                    std::fclose(dump);
                }
            }
            diag::Log("shaderlights", "injected point lights into shader %u (%d chars)", shader, length);
            return;
        }

        if (realShaderSource)
            realShaderSource(shader, count, strings, lengths);
    }

    void *__fastcall ResolverHook(void *(*loader)(const char *))
    {
        void *result = realResolver(loader);
        void **slot = reinterpret_cast<void **>(
            reinterpret_cast<std::uintptr_t>(addresses::Base.ptr) + shaderSourceSlotRva);
        if (*slot && *slot != reinterpret_cast<void *>(&FakeShaderSource))
        {
            realShaderSource = reinterpret_cast<ShaderSourceFn>(*slot);
            *slot = reinterpret_cast<void *>(&FakeShaderSource);
            diag::Log("shaderlights", "glShaderSource hooked (real=%p)", realShaderSource);
        }
        // Also wrap glCompileShader to catch injected-GLSL compile errors, and
        // resolve the status/log helpers through the same loader the game used.
        void **compileSlot = reinterpret_cast<void **>(
            reinterpret_cast<std::uintptr_t>(addresses::Base.ptr) + compileShaderSlotRva);
        if (*compileSlot && *compileSlot != reinterpret_cast<void *>(&FakeCompileShader))
        {
            realCompileShader = reinterpret_cast<CompileShaderFn>(*compileSlot);
            *compileSlot = reinterpret_cast<void *>(&FakeCompileShader);
            if (loader)
            {
                getShaderiv = reinterpret_cast<GetShaderivFn>(loader("glGetShaderiv"));
                getShaderInfoLog = reinterpret_cast<GetShaderInfoLogFn>(loader("glGetShaderInfoLog"));
            }
            diag::Log("shaderlights", "glCompileShader hooked (real=%p status=%p log=%p)",
                      realCompileShader, getShaderiv, getShaderInfoLog);
        }
        return result;
    }
}

namespace shaderlights
{
    void Install()
    {
        if (!addresses::Base.ptr)
            return;
        void *resolver = reinterpret_cast<void *>(
            reinterpret_cast<std::uintptr_t>(addresses::Base.ptr) + resolverRva);
        resolverHook.Install(resolver, reinterpret_cast<void *>(&ResolverHook),
            subhook::HookFlag64BitOffset | subhook::HookFlagTrampoline |
            subhook::HookFlagTrampolineAllocNearby);
        void *trampoline = resolverHook.GetTrampoline();
        if (trampoline)
            realResolver = reinterpret_cast<ResolverFn>(trampoline);
        else
            resolverHook.Remove();
        diag::Log("shaderlights", "resolver hook installed=%d", trampoline ? 1 : 0);
    }
}
#else
namespace shaderlights
{
    void Install() {}
}
#endif
