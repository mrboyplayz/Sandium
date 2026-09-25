#include "Image.hpp"

#include "../Addon.hpp"
#include "../Addresses.hpp"
#include "../LuaManager.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <tuple>
#include <vector>

#if _WIN32
#include <Windows.h>
#include <wincodec.h>
#undef DrawText
#endif

namespace api
{
    namespace
    {
        thread_local bool loadingCoreAsset = false;
    }

    std::filesystem::path ResolveAddonMediaPath(const std::string &relativePath)
    {
        if (relativePath.empty())
            throw std::runtime_error("Media path is empty");
        const std::filesystem::path requested(relativePath);
        if (requested.is_absolute())
        {
            // Server-streamed media (ServerMedia) lives in sandium/cache -- the
            // one absolute location addons may load from.
            const auto cacheRoot = std::filesystem::weakly_canonical("sandium/cache");
            const auto resolvedAbs = std::filesystem::weakly_canonical(requested);
            auto rootIt = cacheRoot.begin(), pathIt = resolvedAbs.begin();
            for (; rootIt != cacheRoot.end() && pathIt != resolvedAbs.end(); ++rootIt, ++pathIt)
                if (*rootIt != *pathIt)
                    throw std::runtime_error("Media paths must be relative to the addon folder");
            if (rootIt != cacheRoot.end())
                throw std::runtime_error("Media paths must be relative to the addon folder");
            return resolvedAbs;
        }
        Addon *addon = loadingCoreAsset ? nullptr : GetMainLuaManager()->GetCurrentAddon();
        if (!addon)
        {
            // Built-in Sandium assets are loaded outside a Lua addon context.
            // Resolve them beside the running game instead of against the
            // process working directory. Launchers are free to inherit a
            // different working directory (Noxus does), while the asset tree
            // always lives beside subrosa.exe.
            std::filesystem::path gameRoot = std::filesystem::current_path();
#if _WIN32
            std::wstring executablePath(32768, L'\0');
            const DWORD executableLength = GetModuleFileNameW(
                nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
            if (executableLength > 0 && executableLength < executablePath.size())
            {
                executablePath.resize(executableLength);
                gameRoot = std::filesystem::path(executablePath).parent_path();
            }
#endif
            // Permit the game's own cursor atlas for Sandium's custom lobby.
            // Other core media remain confined to sandium/models.
            if (requested == std::filesystem::path("texture/mouse01.png"))
                return std::filesystem::weakly_canonical(gameRoot / requested);
            const auto modelRoot = std::filesystem::weakly_canonical(gameRoot / "sandium/models");
            const auto resolved = std::filesystem::weakly_canonical(gameRoot / requested);
            auto rootIt = modelRoot.begin(), pathIt = resolved.begin();
            for (; rootIt != modelRoot.end() && pathIt != resolved.end(); ++rootIt, ++pathIt)
                if (*rootIt != *pathIt)
                    throw std::runtime_error("Core media path escapes sandium/models");
            if (rootIt != modelRoot.end())
                throw std::runtime_error("Media.Load must be called by an addon");
            return resolved;
        }
        const auto root = std::filesystem::weakly_canonical(addon->GetFolderPath());
        const auto resolved = std::filesystem::weakly_canonical(root / requested);
        auto rootIt = root.begin(), pathIt = resolved.begin();
        for (; rootIt != root.end() && pathIt != resolved.end(); ++rootIt, ++pathIt)
            if (*rootIt != *pathIt)
                throw std::runtime_error("Media path escapes the addon folder");
        if (rootIt != root.end())
            throw std::runtime_error("Media path escapes the addon folder");
        return resolved;
    }

    namespace
    {
        thread_local bool luaDrawing = false;
        struct QueuedImage
        {
            unsigned int texture;
            float x, y, width, height, red, green, blue, alpha;
            int layer;
            float rotation;
        };
        thread_local std::vector<QueuedImage> imageQueue;
#if _WIN32
        IWICImagingFactory *wicFactory = nullptr;
        bool comInitialized = false;

        using GLProc = void (APIENTRY *)();
        using GetProcAddressFn = GLProc (*)(const char *);
        GetProcAddressFn getGLProcAddress = nullptr;

        using BindTextureFn = void (APIENTRY *)(unsigned int, unsigned int);

        constexpr unsigned int GL_TEXTURE_2D_VALUE = 0x0DE1;
        constexpr unsigned int GL_RGBA_VALUE = 0x1908;
        constexpr unsigned int GL_UNSIGNED_BYTE_VALUE = 0x1401;
        constexpr unsigned int GL_LINEAR_VALUE = 0x2601;
        constexpr unsigned int GL_TEXTURE_MIN_FILTER_VALUE = 0x2801;
        constexpr unsigned int GL_TEXTURE_MAG_FILTER_VALUE = 0x2800;
        constexpr unsigned int GL_TEXTURE_BINDING_2D_VALUE = 0x8069;

        // Fixed-function pipeline enums/entry points used only by rotated quads,
        // where Sub Rosa's own UI draw cannot help us.
        constexpr unsigned int GL_CURRENT_PROGRAM_VALUE = 0x8B8D;
        constexpr unsigned int GL_VIEWPORT_VALUE = 0x0BA2;
        constexpr unsigned int GL_PROJECTION_VALUE = 0x1701;
        constexpr unsigned int GL_MODELVIEW_VALUE = 0x1700;
        constexpr unsigned int GL_BLEND_VALUE = 0x0BE2;
        constexpr unsigned int GL_SRC_ALPHA_VALUE = 0x0302;
        constexpr unsigned int GL_ONE_MINUS_SRC_ALPHA_VALUE = 0x0303;
        constexpr unsigned int GL_QUADS_VALUE = 0x0007;

        template<typename T> T GL(const char *name)
        {
            if (!getGLProcAddress)
                throw std::runtime_error("OpenGL is not initialized");
            T function = reinterpret_cast<T>(getGLProcAddress(name));
            if (!function)
                throw std::runtime_error(std::string("OpenGL function unavailable: ") + name);
            return function;
        }

        // Sub Rosa draws its UI through a shader, so rotating one of its quads via
        // the fixed-function matrices does nothing. Rotated draws use a tiny shader
        // program of our own instead; any failure falls back to the unrotated quad.
        constexpr unsigned int GL_VERTEX_SHADER_VALUE = 0x8B31;
        constexpr unsigned int GL_FRAGMENT_SHADER_VALUE = 0x8B30;
        constexpr unsigned int GL_COMPILE_STATUS_VALUE = 0x8B81;
        constexpr unsigned int GL_LINK_STATUS_VALUE = 0x8B82;
        constexpr unsigned int GL_ARRAY_BUFFER_VALUE = 0x8892;
        constexpr unsigned int GL_STREAM_DRAW_VALUE = 0x88E0;
        constexpr unsigned int GL_TRIANGLES_VALUE = 0x0004;
        constexpr unsigned int GL_FLOAT_VALUE = 0x1406;
        constexpr unsigned int GL_VERTEX_ARRAY_BINDING_VALUE = 0x85B5;

        struct ShaderRotator
        {
            unsigned int program = 0;
            unsigned int buffer = 0;
            unsigned int vertexArray = 0;
            int resolutionUniform = -1;
            int textureUniform = -1;
            bool ok = false;
        };

        const ShaderRotator &GetShaderRotator()
        {
            static ShaderRotator rotator = []
            {
                ShaderRotator out;
                if (!getGLProcAddress) return out;
                auto createShader = reinterpret_cast<unsigned int (APIENTRY *)(unsigned int)>(getGLProcAddress("glCreateShader"));
                auto shaderSource = reinterpret_cast<void (APIENTRY *)(unsigned int, int, const char **, const int *)>(getGLProcAddress("glShaderSource"));
                auto compileShader = reinterpret_cast<void (APIENTRY *)(unsigned int)>(getGLProcAddress("glCompileShader"));
                auto getShaderiv = reinterpret_cast<void (APIENTRY *)(unsigned int, unsigned int, int *)>(getGLProcAddress("glGetShaderiv"));
                auto createProgram = reinterpret_cast<unsigned int (APIENTRY *)()>(getGLProcAddress("glCreateProgram"));
                auto attachShader = reinterpret_cast<void (APIENTRY *)(unsigned int, unsigned int)>(getGLProcAddress("glAttachShader"));
                auto linkProgram = reinterpret_cast<void (APIENTRY *)(unsigned int)>(getGLProcAddress("glLinkProgram"));
                auto getProgramiv = reinterpret_cast<void (APIENTRY *)(unsigned int, unsigned int, int *)>(getGLProcAddress("glGetProgramiv"));
                auto getUniformLocation = reinterpret_cast<int (APIENTRY *)(unsigned int, const char *)>(getGLProcAddress("glGetUniformLocation"));
                auto uniform2f = reinterpret_cast<void (APIENTRY *)(int, float, float)>(getGLProcAddress("glUniform2f"));
                auto uniform1i = reinterpret_cast<void (APIENTRY *)(int, int)>(getGLProcAddress("glUniform1i"));
                auto genBuffers = reinterpret_cast<void (APIENTRY *)(int, unsigned int *)>(getGLProcAddress("glGenBuffers"));
                auto bindBuffer = reinterpret_cast<void (APIENTRY *)(unsigned int, unsigned int)>(getGLProcAddress("glBindBuffer"));
                auto bufferData = reinterpret_cast<void (APIENTRY *)(unsigned int, ptrdiff_t, const void *, unsigned int)>(getGLProcAddress("glBufferData"));
                auto vertexAttribPointer = reinterpret_cast<void (APIENTRY *)(unsigned int, int, unsigned int, unsigned char, int, const void *)>(getGLProcAddress("glVertexAttribPointer"));
                auto enableVertexAttribArray = reinterpret_cast<void (APIENTRY *)(unsigned int)>(getGLProcAddress("glEnableVertexAttribArray"));
                auto genVertexArrays = reinterpret_cast<void (APIENTRY *)(int, unsigned int *)>(getGLProcAddress("glGenVertexArrays"));
                auto bindVertexArray = reinterpret_cast<void (APIENTRY *)(unsigned int)>(getGLProcAddress("glBindVertexArray"));
                if (!createShader || !shaderSource || !compileShader || !getShaderiv || !createProgram ||
                    !attachShader || !linkProgram || !getProgramiv || !getUniformLocation || !uniform2f ||
                    !uniform1i || !genBuffers || !bindBuffer || !bufferData || !vertexAttribPointer ||
                    !enableVertexAttribArray || !genVertexArrays || !bindVertexArray)
                    return out;

                const char *vertexSource =
                    "#version 330\n"
                    "layout(location = 0) in vec2 aPos;\n"
                    "layout(location = 1) in vec2 aUV;\n"
                    "layout(location = 2) in vec4 aColor;\n"
                    "uniform vec2 uResolution;\n"
                    "out vec2 vUV;\n"
                    "out vec4 vColor;\n"
                    "void main()\n"
                    "{\n"
                    "    gl_Position = vec4(aPos.x / uResolution.x * 2.0 - 1.0,\n"
                    "                       1.0 - aPos.y / uResolution.y * 2.0, 0.0, 1.0);\n"
                    "    vUV = aUV;\n"
                    "    vColor = aColor;\n"
                    "}\n";
                const char *fragmentSource =
                    "#version 330\n"
                    "uniform sampler2D uTex;\n"
                    "in vec2 vUV;\n"
                    "in vec4 vColor;\n"
                    "out vec4 fragColor;\n"
                    "void main()\n"
                    "{\n"
                    "    fragColor = texture(uTex, vUV) * vColor;\n"
                    "}\n";

                unsigned int vertex = createShader(GL_VERTEX_SHADER_VALUE);
                shaderSource(vertex, 1, &vertexSource, nullptr);
                compileShader(vertex);
                unsigned int fragment = createShader(GL_FRAGMENT_SHADER_VALUE);
                shaderSource(fragment, 1, &fragmentSource, nullptr);
                compileShader(fragment);
                int vertexCompiled = 0, fragmentCompiled = 0;
                getShaderiv(vertex, GL_COMPILE_STATUS_VALUE, &vertexCompiled);
                getShaderiv(fragment, GL_COMPILE_STATUS_VALUE, &fragmentCompiled);
                if (!vertexCompiled || !fragmentCompiled)
                    return out;

                out.program = createProgram();
                attachShader(out.program, vertex);
                attachShader(out.program, fragment);
                linkProgram(out.program);
                int linked = 0;
                getProgramiv(out.program, GL_LINK_STATUS_VALUE, &linked);
                if (!linked)
                    return out;

                genBuffers(1, &out.buffer);
                genVertexArrays(1, &out.vertexArray);
                out.resolutionUniform = getUniformLocation(out.program, "uResolution");
                out.textureUniform = getUniformLocation(out.program, "uTex");
                out.ok = out.buffer != 0 && out.vertexArray != 0 &&
                         out.resolutionUniform >= 0 && out.textureUniform >= 0;
                return out;
            }();
            return rotator;
        }

        void DrawQueuedItem(const QueuedImage &item)
        {
            using DrawUIQuad = void (*)(float, float, float, float, unsigned char,
                                        float, float, float, float);
            const auto base = reinterpret_cast<std::uintptr_t>(addresses::Base.ptr);
            const auto draw = reinterpret_cast<DrawUIQuad>(base + 0x6CFF0);
            GL<BindTextureFn>("glBindTexture")(GL_TEXTURE_2D_VALUE, item.texture);
            if (item.rotation != 0.0f)
            {
                const ShaderRotator &rotator = GetShaderRotator();
                if (rotator.ok)
                {
                    using GetIntegervFn = void (APIENTRY *)(unsigned int, int *);
                    using UseProgramFn = void (APIENTRY *)(unsigned int);
                    using BindVertexArrayFn = void (APIENTRY *)(unsigned int);
                    using BindBufferFn = void (APIENTRY *)(unsigned int, unsigned int);
                    using BufferDataFn = void (APIENTRY *)(unsigned int, ptrdiff_t, const void *, unsigned int);
                    using EnableVertexAttribArrayFn = void (APIENTRY *)(unsigned int);
                    using VertexAttribPointerFn = void (APIENTRY *)(unsigned int, int, unsigned int, unsigned char, int, const void *);
                    using DrawArraysFn = void (APIENTRY *)(unsigned int, int, int);
                    using Uniform2fFn = void (APIENTRY *)(int, float, float);
                    using Uniform1iFn = void (APIENTRY *)(int, int);
                    const auto getIntegerv = GL<GetIntegervFn>("glGetIntegerv");
                    const auto useProgram = GL<UseProgramFn>("glUseProgram");
                    const auto bindVertexArray = GL<BindVertexArrayFn>("glBindVertexArray");
                    const auto bindBuffer = GL<BindBufferFn>("glBindBuffer");
                    const auto bufferData = GL<BufferDataFn>("glBufferData");
                    const auto enableVertexAttribArray = GL<EnableVertexAttribArrayFn>("glEnableVertexAttribArray");
                    const auto vertexAttribPointer = GL<VertexAttribPointerFn>("glVertexAttribPointer");
                    const auto drawArrays = GL<DrawArraysFn>("glDrawArrays");
                    const auto uniform2f = GL<Uniform2fFn>("glUniform2f");
                    const auto uniform1i = GL<Uniform1iFn>("glUniform1i");

                    int previousProgram = 0, previousArray = 0, viewport[4] = {};
                    getIntegerv(GL_CURRENT_PROGRAM_VALUE, &previousProgram);
                    getIntegerv(GL_VERTEX_ARRAY_BINDING_VALUE, &previousArray);
                    getIntegerv(GL_VIEWPORT_VALUE, viewport);

                    // Rotate the quad corners on the CPU; y-down coordinates make
                    // positive angles turn clockwise. Textures are stored bottom-up,
                    // so the top edge samples t=1.
                    const float radians = item.rotation * 3.14159265f / 180.0f;
                    const float cosine = std::cos(radians), sine = std::sin(radians);
                    const float centerX = item.x + item.width * 0.5f;
                    const float centerY = item.y + item.height * 0.5f;
                    const float halfWidth = item.width * 0.5f;
                    const float halfHeight = item.height * 0.5f;
                    const float corners[4][2] = {{-halfWidth, -halfHeight}, {halfWidth, -halfHeight},
                                                 {halfWidth, halfHeight}, {-halfWidth, halfHeight}};
                    const float uvs[4][2] = {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}};
                    const int order[6] = {0, 1, 2, 0, 2, 3};
                    float vertices[6][8];
                    for (int i = 0; i < 6; ++i)
                    {
                        const float px = corners[order[i]][0], py = corners[order[i]][1];
                        vertices[i][0] = centerX + px * cosine - py * sine;
                        vertices[i][1] = centerY + px * sine + py * cosine;
                        vertices[i][2] = uvs[order[i]][0];
                        vertices[i][3] = uvs[order[i]][1];
                        vertices[i][4] = item.red;
                        vertices[i][5] = item.green;
                        vertices[i][6] = item.blue;
                        vertices[i][7] = item.alpha;
                    }

                    bindVertexArray(rotator.vertexArray);
                    bindBuffer(GL_ARRAY_BUFFER_VALUE, rotator.buffer);
                    bufferData(GL_ARRAY_BUFFER_VALUE, static_cast<ptrdiff_t>(sizeof(vertices)), vertices, GL_STREAM_DRAW_VALUE);
                    enableVertexAttribArray(0);
                    enableVertexAttribArray(1);
                    enableVertexAttribArray(2);
                    vertexAttribPointer(0, 2, GL_FLOAT_VALUE, 0, 32, reinterpret_cast<const void *>(0));
                    vertexAttribPointer(1, 2, GL_FLOAT_VALUE, 0, 32, reinterpret_cast<const void *>(8));
                    vertexAttribPointer(2, 4, GL_FLOAT_VALUE, 0, 32, reinterpret_cast<const void *>(16));
                    useProgram(rotator.program);
                    uniform2f(rotator.resolutionUniform, static_cast<float>(viewport[2]), static_cast<float>(viewport[3]));
                    uniform1i(rotator.textureUniform, 0);
                    drawArrays(GL_TRIANGLES_VALUE, 0, 6);
                    useProgram(static_cast<unsigned int>(previousProgram));
                    bindVertexArray(static_cast<unsigned int>(previousArray));
                    return;
                }
                // fall through: unrotated
            }
            draw(item.x, item.y, item.width, item.height, 0,
                 item.red, item.green, item.blue, item.alpha);
        }

        template<typename T> void Release(T *&object)
        {
            if (object) object->Release();
            object = nullptr;
        }
#endif
    }

    Image::Image(unsigned int texture, int width, int height)
        : _texture(texture), _width(width), _height(height) {}

    Image::~Image() { Delete(); }

    std::shared_ptr<Image> Image::Load(const std::string &path)
    {
#if _WIN32
        if (!wicFactory)
            throw std::runtime_error("Image API is not initialized");
        const auto resolved = ResolveAddonMediaPath(path);
        if (!std::filesystem::is_regular_file(resolved))
            throw std::runtime_error("Image file does not exist: " + path);

        IWICBitmapDecoder *decoder = nullptr;
        IWICBitmapFrameDecode *frame = nullptr;
        IWICFormatConverter *converter = nullptr;
        HRESULT result = wicFactory->CreateDecoderFromFilename(resolved.c_str(), nullptr, GENERIC_READ,
                                                               WICDecodeMetadataCacheOnLoad, &decoder);
        if (SUCCEEDED(result)) result = decoder->GetFrame(0, &frame);
        if (SUCCEEDED(result)) result = wicFactory->CreateFormatConverter(&converter);
        if (SUCCEEDED(result)) result = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                                               WICBitmapDitherTypeNone, nullptr, 0.0,
                                                               WICBitmapPaletteTypeCustom);
        UINT width = 0, height = 0;
        if (SUCCEEDED(result)) result = converter->GetSize(&width, &height);
        if (SUCCEEDED(result) && (width == 0 || height == 0 || width > 16384 || height > 16384))
            result = E_INVALIDARG;
        std::vector<unsigned char> pixels;
        if (SUCCEEDED(result))
        {
            const unsigned long long byteCount = static_cast<unsigned long long>(width) * height * 4;
            if (byteCount > 512ull * 1024ull * 1024ull) result = E_OUTOFMEMORY;
            else pixels.resize(static_cast<std::size_t>(byteCount));
        }
        if (SUCCEEDED(result))
            result = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data());
        Release(converter); Release(frame); Release(decoder);
        if (FAILED(result))
            throw std::runtime_error("Could not decode image: " + path);

        // WIC returns rows from top to bottom, while Sub Rosa's UI shader samples
        // OpenGL textures from the bottom row. Flip once during loading so Lua uses
        // normal image coordinates and Draw displays files upright.
        const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
        std::vector<unsigned char> swapRow(rowBytes);
        for (UINT top = 0, bottom = height - 1; top < bottom; ++top, --bottom)
        {
            unsigned char *topRow = pixels.data() + static_cast<std::size_t>(top) * rowBytes;
            unsigned char *bottomRow = pixels.data() + static_cast<std::size_t>(bottom) * rowBytes;
            std::copy_n(topRow, rowBytes, swapRow.data());
            std::copy_n(bottomRow, rowBytes, topRow);
            std::copy_n(swapRow.data(), rowBytes, bottomRow);
        }

        using GenTextures = void (APIENTRY *)(int, unsigned int *);
        using TexParameteri = void (APIENTRY *)(unsigned int, unsigned int, int);
        using TexImage2D = void (APIENTRY *)(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void *);
        unsigned int texture = 0;
        GL<GenTextures>("glGenTextures")(1, &texture);
        GL<BindTextureFn>("glBindTexture")(GL_TEXTURE_2D_VALUE, texture);
        GL<TexParameteri>("glTexParameteri")(GL_TEXTURE_2D_VALUE, GL_TEXTURE_MIN_FILTER_VALUE, GL_LINEAR_VALUE);
        GL<TexParameteri>("glTexParameteri")(GL_TEXTURE_2D_VALUE, GL_TEXTURE_MAG_FILTER_VALUE, GL_LINEAR_VALUE);
        GL<TexImage2D>("glTexImage2D")(GL_TEXTURE_2D_VALUE, 0, GL_RGBA_VALUE, static_cast<int>(width),
                                       static_cast<int>(height), 0, GL_RGBA_VALUE, GL_UNSIGNED_BYTE_VALUE, pixels.data());
        return std::shared_ptr<Image>(new Image(texture, static_cast<int>(width), static_cast<int>(height)));
#else
        (void)path;
        throw std::runtime_error("Image.Load is currently available on Windows only");
#endif
    }

    std::shared_ptr<Image> Image::LoadCore(const std::string &path)
    {
        const bool previous = loadingCoreAsset;
        loadingCoreAsset = true;
        try
        {
            auto image = Load(path);
            loadingCoreAsset = previous;
            return image;
        }
        catch (...)
        {
            loadingCoreAsset = previous;
            throw;
        }
    }

    void Image::Draw(float x, float y, float width, float height, float red, float green, float blue, float alpha)
    {
#if _WIN32
        if (!_texture) throw std::runtime_error("Cannot draw a deleted image");
        if (!luaDrawing) throw std::runtime_error("Image:Draw may only be used inside DrawHUD or DrawMenu hooks");
        if (width <= 0 || height <= 0) throw std::runtime_error("Image width and height must be positive");
        red = std::clamp(red, 0.0f, 1.0f); green = std::clamp(green, 0.0f, 1.0f);
        blue = std::clamp(blue, 0.0f, 1.0f); alpha = std::clamp(alpha, 0.0f, 1.0f);

        QueueDraw(_texture, x, y, width, height, red, green, blue, alpha, _layer, _rotation);
#else
        (void)x;(void)y;(void)width;(void)height;(void)red;(void)green;(void)blue;(void)alpha;
#endif
    }

    void QueueDraw(unsigned int texture, float x, float y, float width, float height,
                   float red, float green, float blue, float alpha, int layer, float rotation)
    {
#if _WIN32
        imageQueue.push_back({texture, x, y, width, height, red, green, blue, alpha, layer, rotation});
#else
        (void)texture;(void)x;(void)y;(void)width;(void)height;(void)red;(void)green;(void)blue;(void)alpha;(void)layer;(void)rotation;
#endif
    }

    void FlushImageLayer(bool foreground)
    {
#if _WIN32
        std::stable_sort(imageQueue.begin(), imageQueue.end(),
                         [](const QueuedImage &a, const QueuedImage &b) { return a.layer < b.layer; });
        int previousTexture = 0;
        using GetIntegerv = void (APIENTRY *)(unsigned int, int *);
        GL<GetIntegerv>("glGetIntegerv")(GL_TEXTURE_BINDING_2D_VALUE, &previousTexture);

        for (const auto &item : imageQueue)
        {
            if ((item.layer >= 0) != foreground) continue;
            DrawQueuedItem(item);
        }
        GL<BindTextureFn>("glBindTexture")(GL_TEXTURE_2D_VALUE,
                                           static_cast<unsigned int>(previousTexture));
#endif
    }

    void Image::Delete()
    {
#if _WIN32
        if (_texture && getGLProcAddress)
        {
            using DeleteTextures = void (APIENTRY *)(int, const unsigned int *);
            GL<DeleteTextures>("glDeleteTextures")(1, &_texture);
        }
#endif
        _texture = 0;
    }

    void InitializeImages()
    {
#if _WIN32
        HMODULE sdl = GetModuleHandleA("SDL2.dll");
        getGLProcAddress = reinterpret_cast<GetProcAddressFn>(GetProcAddress(sdl, "SDL_GL_GetProcAddress"));
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        comInitialized = SUCCEEDED(comResult);
        const HRESULT factoryResult = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                                       IID_PPV_ARGS(&wicFactory));
        if (FAILED(factoryResult))
            wicFactory = nullptr;
#endif
    }
    void ShutdownImages()
    {
#if _WIN32
        Release(wicFactory);
        if (comInitialized) CoUninitialize();
        comInitialized = false;
        getGLProcAddress = nullptr;
#endif
    }
    void BeginLuaDrawing() { imageQueue.clear(); luaDrawing = true; }
    void EndLuaDrawing() { luaDrawing = false; }
    bool IsLuaDrawing() { return luaDrawing; }
}
