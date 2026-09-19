#include "GLUniforms.hpp"

#include <subhook.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#if _WIN32
#include <Windows.h>
#undef DrawText

#include "../Addresses.hpp"
#endif

namespace api
{
    namespace glcap
    {
#if _WIN32
        namespace
        {
            using GetProcAddressFn = void *(*)(const char *);
            using GetUniformLocationFn = int (*)(unsigned int, const char *);
            using UniformMatrix4fvFn = void (*)(int, int, unsigned char, const float *);
            using Uniform3fvFn = void (*)(int, int, const float *);
            using Uniform3fFn = void (*)(int, float, float, float);
            using GetIntegervFn = void (*)(unsigned int, int *);

            constexpr unsigned int GL_CURRENT_PROGRAM_VALUE = 0x8B8D;

            enum Semantic
            {
                SEM_NONE = -1,
                SEM_MODELVIEWPROJECTION = 0,
                SEM_VIEWPOSITION = 1,
                SEM_VIEWMATRIX = 2
            };

            std::mutex stateMutex;

            // Per-program capture: the same uniform names are uploaded by several
            // shaders (main scene, water reflection, shadow maps...), each with a
            // different camera, so a single global value picks the wrong pass.
            struct ProgramCapture
            {
                float mvp[16] = {};
                bool mvpTransposed = false;
                bool hasMvp = false;
                unsigned int mvpUploads = 0;
                float viewPosition[3] = {};
                bool hasViewPosition = false;
                unsigned int viewPosUploads = 0;
            };
            std::unordered_map<unsigned int, ProgramCapture> programCaptures;

            std::unordered_map<std::uint64_t, Semantic> &SemanticMap()
            {
                static std::unordered_map<std::uint64_t, Semantic> map;
                return map;
            }

            std::atomic<bool> prepared{false};

            // real driver entry points, captured when the wrappers are handed out
            // or resolved through SDL at Prepare time
            GetProcAddressFn sdlGLProcAddress = nullptr;
            GetUniformLocationFn realGetUniformLocation = nullptr;
            UniformMatrix4fvFn realUniformMatrix4fv = nullptr;
            Uniform3fvFn realUniform3fv = nullptr;
            Uniform3fFn realUniform3f = nullptr;
            GetIntegervFn realGetIntegerv = nullptr;

            std::uint64_t Key(unsigned int program, int location)
            {
                return (static_cast<std::uint64_t>(program) << 32) | static_cast<std::uint32_t>(location);
            }

            Semantic SemanticFor(unsigned int program, int location)
            {
                {
                    const std::lock_guard<std::mutex> lock(stateMutex);
                    const auto it = SemanticMap().find(Key(program, location));
                    if (it != SemanticMap().end())
                        return it->second;
                }
                // The game resolved its uniform locations before we were injected,
                // so we cannot rely on intercepting glGetUniformLocation. Probe
                // the program ourselves and cache the answer (including "none").
                Semantic semantic = SEM_NONE;
                if (realGetUniformLocation)
                {
                    if (realGetUniformLocation(program, "modelviewprojectionmatrix") == location)
                        semantic = SEM_MODELVIEWPROJECTION;
                    else if (realGetUniformLocation(program, "viewposition") == location)
                        semantic = SEM_VIEWPOSITION;
                    else if (realGetUniformLocation(program, "viewmatrix") == location)
                        semantic = SEM_VIEWMATRIX;
                }
                {
                    const std::lock_guard<std::mutex> lock(stateMutex);
                    SemanticMap()[Key(program, location)] = semantic;
                }
                return semantic;
            }

            float lastViewMatrix[16] = {};
            bool lastViewTransposed = false;
            bool hasLastView = false;
            float lastViewPosition[3] = {};
            bool hasLastViewPosition = false;

            unsigned int CurrentProgram()
            {
                int program = 0;
                if (realGetIntegerv)
                    realGetIntegerv(GL_CURRENT_PROGRAM_VALUE, &program);
                return static_cast<unsigned int>(program);
            }

            int HookGetUniformLocation(unsigned int program, const char *name)
            {
                const int location = realGetUniformLocation(program, name);
                if (location >= 0 && name)
                {
                    Semantic semantic = SEM_NONE;
                    if (std::strcmp(name, "modelviewprojectionmatrix") == 0)
                        semantic = SEM_MODELVIEWPROJECTION;
                    else if (std::strcmp(name, "viewposition") == 0)
                        semantic = SEM_VIEWPOSITION;
                    else if (std::strcmp(name, "viewmatrix") == 0)
                        semantic = SEM_VIEWMATRIX;
                    if (semantic != SEM_NONE)
                    {
                        const std::lock_guard<std::mutex> lock(stateMutex);
                        SemanticMap()[Key(program, location)] = semantic;
                    }
                }
                return location;
            }

            void HookUniformMatrix4fv(int location, int count, unsigned char transpose, const float *value)
            {
                if (count == 1 && value &&
                    SemanticFor(CurrentProgram(), location) == SEM_VIEWMATRIX)
                {
                    const std::lock_guard<std::mutex> lock(stateMutex);
                    std::memcpy(lastViewMatrix, value, sizeof(lastViewMatrix));
                    lastViewTransposed = transpose != 0;
                    hasLastView = true;
                }
                realUniformMatrix4fv(location, count, transpose, value);
                if (count == 1 && value)
                {
                    const unsigned int program = CurrentProgram();
                    if (SemanticFor(program, location) == SEM_MODELVIEWPROJECTION)
                    {
                        const std::lock_guard<std::mutex> lock(stateMutex);
                        ProgramCapture &capture = programCaptures[program];
                        std::memcpy(capture.mvp, value, sizeof(capture.mvp));
                        capture.mvpTransposed = transpose != 0;
                        capture.hasMvp = true;
                        ++capture.mvpUploads;
                    }
                }
            }

            void HookUniform3fv(int location, int count, const float *value)
            {
                realUniform3fv(location, count, value);
                const unsigned int program = CurrentProgram();
                if (count == 1 && value)
                {
                    const unsigned int program = CurrentProgram();
                    if (SemanticFor(program, location) == SEM_VIEWPOSITION)
                    {
                        const std::lock_guard<std::mutex> lock(stateMutex);
                        ProgramCapture &capture = programCaptures[program];
                        std::memcpy(capture.viewPosition, value, sizeof(capture.viewPosition));
                        capture.hasViewPosition = true;
                        ++capture.viewPosUploads;
                        std::memcpy(lastViewPosition, value, sizeof(lastViewPosition));
                        hasLastViewPosition = true;
                    }
                }
            }

            void HookUniform3f(int location, float x, float y, float z)
            {
                realUniform3f(location, x, y, z);
                const unsigned int program = CurrentProgram();
                if (SemanticFor(program, location) == SEM_VIEWPOSITION)
                {
                    const std::lock_guard<std::mutex> lock(stateMutex);
                    ProgramCapture &capture = programCaptures[program];
                    capture.viewPosition[0] = x;
                    capture.viewPosition[1] = y;
                    capture.viewPosition[2] = z;
                    capture.hasViewPosition = true;
                    ++capture.viewPosUploads;
                }
            }

            // ---- memory pointer patching -------------------------------------
            // The game resolves GL functions before this DLL is injected and keeps
            // the pointers, so proc-address hooking never sees them. Scan the
            // game's memory for the stored driver pointers and swap them for the
            // wrappers above.
            std::uintptr_t ourModuleBase = 0;

            bool RegionAllowed(const MEMORY_BASIC_INFORMATION &mbi)
            {
                const std::uintptr_t allocBase = reinterpret_cast<std::uintptr_t>(mbi.AllocationBase);
                if (allocBase == ourModuleBase)
                    return false; // never patch ourselves
                if (mbi.Type == MEM_IMAGE)
                    return allocBase == reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
                return mbi.Type == MEM_PRIVATE;
            }

            int PatchPointer(std::uintptr_t needle, std::uintptr_t replacement)
            {
                int patched = 0;
                SYSTEM_INFO info{};
                GetSystemInfo(&info);
                std::uintptr_t address = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
                const std::uintptr_t maxAddress = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);
                while (address < maxAddress)
                {
                    MEMORY_BASIC_INFORMATION mbi{};
                    if (!VirtualQuery(reinterpret_cast<void *>(address), &mbi, sizeof(mbi)))
                        break;
                    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
                    const std::uintptr_t size = mbi.RegionSize;
                    // only plain writable data pages: never touch executable
                    // regions (moonjit's JIT code lives in private RWX memory)
                    if (mbi.State == MEM_COMMIT && RegionAllowed(mbi) &&
                        (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_WRITECOPY))
                    {
                        DWORD oldProtect = 0;
                        if (VirtualProtect(mbi.BaseAddress, size, PAGE_READWRITE, &oldProtect))
                        {
                            for (std::uintptr_t offset = 0; offset + sizeof(void *) <= size; offset += sizeof(void *))
                            {
                                void *at = reinterpret_cast<void *>(base + offset);
                                if (*reinterpret_cast<std::uintptr_t *>(at) == needle)
                                {
                                    *reinterpret_cast<std::uintptr_t *>(at) = replacement;
                                    ++patched;
                                }
                            }
                            VirtualProtect(mbi.BaseAddress, size, oldProtect, &oldProtect);
                        }
                    }
                    address = base + size;
                }
                return patched;
            }
        }
#endif

        void Install()
        {
#if _WIN32
            // intentionally empty: the game resolved its GL entry points before this
            // DLL was injected, so interception happens via the pointer patching in
            // ItemBillboard's setup instead of proc-address hooking
#endif
        }

        // Called once with the GL context current on the main thread; resolves the
        // real entry points used by the wrappers (and the pointer patching done by
        // ItemBillboard's setup).
        void Prepare()
        {
#if _WIN32
            if (prepared.exchange(true))
                return;
            const HMODULE sdl = GetModuleHandleA("SDL2.dll");
            if (!sdl)
                return;
            const auto sdlGetProcAddress = reinterpret_cast<void *(*)(const char *)>(
                GetProcAddress(sdl, "SDL_GL_GetProcAddress"));
            if (!sdlGetProcAddress)
                return;
            realUniformMatrix4fv = reinterpret_cast<UniformMatrix4fvFn>(sdlGetProcAddress("glUniformMatrix4fv"));
            realGetUniformLocation = reinterpret_cast<GetUniformLocationFn>(sdlGetProcAddress("glGetUniformLocation"));
            realUniform3fv = reinterpret_cast<Uniform3fvFn>(sdlGetProcAddress("glUniform3fv"));
            realUniform3f = reinterpret_cast<Uniform3fFn>(sdlGetProcAddress("glUniform3f"));
            realGetIntegerv = reinterpret_cast<GetIntegervFn>(sdlGetProcAddress("glGetIntegerv"));

            // The game stores the driver pointers it resolved before we were
            // injected; swap every stored copy for our wrappers so uploads pass
            // through the capture above.
            std::thread([] {
                for (int attempt = 0; attempt < 120; ++attempt)
                {
                    {
                        const std::lock_guard<std::mutex> lock(stateMutex);
                        bool complete = false;
                        for (const auto &entry : programCaptures)
                            if (entry.second.hasMvp && entry.second.hasViewPosition)
                                complete = true;
                        if (complete)
                            return;
                    }
                    PatchPointer(reinterpret_cast<std::uintptr_t>(realUniformMatrix4fv),
                                 reinterpret_cast<std::uintptr_t>(&HookUniformMatrix4fv));
                    PatchPointer(reinterpret_cast<std::uintptr_t>(realGetUniformLocation),
                                 reinterpret_cast<std::uintptr_t>(&HookGetUniformLocation));
                    PatchPointer(reinterpret_cast<std::uintptr_t>(realUniform3fv),
                                 reinterpret_cast<std::uintptr_t>(&HookUniform3fv));
                    PatchPointer(reinterpret_cast<std::uintptr_t>(realUniform3f),
                                 reinterpret_cast<std::uintptr_t>(&HookUniform3f));
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
            }).detach();
#endif
        }

        bool HasLastView()
        {
#if _WIN32
            const std::lock_guard<std::mutex> lock(stateMutex);
            return hasLastView;
#else
            return false;
#endif
        }

        const float *LastViewMatrix()
        {
#if _WIN32
            return lastViewMatrix;
#else
            return nullptr;
#endif
        }

        bool LastViewTransposed()
        {
#if _WIN32
            return lastViewTransposed;
#else
            return false;
#endif
        }

        const float *LastViewPosition()
        {
#if _WIN32
            return lastViewPosition;
#else
            return nullptr;
#endif
        }

        bool LastViewPositionValid()
        {
#if _WIN32
            const std::lock_guard<std::mutex> lock(stateMutex);
            return hasLastViewPosition;
#else
            return false;
#endif
        }

        bool HasViewProjection()
        {
#if _WIN32
            const std::lock_guard<std::mutex> lock(stateMutex);
            for (const auto &entry : programCaptures)
                if (entry.second.hasMvp)
                    return true;
#endif
            return false;
        }

        void DumpDiagnostics()
        {
#if _WIN32
            const std::lock_guard<std::mutex> lock(stateMutex);
            if (std::FILE *status = std::fopen("sandium_glcap.txt", "w"))
            {
                std::fprintf(status, "programs=%zu\n", programCaptures.size());
                for (const auto &entry : programCaptures)
                {
                    const ProgramCapture &capture = entry.second;
                    std::fprintf(status,
                                 "program=%u mvpUploads=%u hasMvp=%d transpose=%d "
                                 "viewPosUploads=%u viewPos=(%.1f,%.1f,%.1f)\n",
                                 entry.first, capture.mvpUploads, capture.hasMvp ? 1 : 0,
                                 capture.mvpTransposed ? 1 : 0, capture.viewPosUploads,
                                 capture.hasViewPosition ? capture.viewPosition[0] : 0.0f,
                                 capture.hasViewPosition ? capture.viewPosition[1] : 0.0f,
                                 capture.hasViewPosition ? capture.viewPosition[2] : 0.0f);
                }
                std::fclose(status);
            }
#endif
        }

        namespace
        {
#if _WIN32
            // Picks the camera program: only programs that also upload
            // "viewposition" carry a pure view*projection matrix. Programs without
            // it (per-object transforms, shadow passes) must be ignored — picking
            // those made prompts project with some object's matrix instead of the
            // camera's.
            const ProgramCapture *BestProgram()
            {
                const ProgramCapture *best = nullptr;
                for (const auto &entry : programCaptures)
                {
                    const ProgramCapture &capture = entry.second;
                    if (!capture.hasMvp || !capture.hasViewPosition)
                        continue;
                    if (!best || capture.viewPosUploads > best->viewPosUploads)
                        best = &capture;
                }
                return best;
            }
#endif
        }

        const float *ViewProjectionMatrix()
        {
#if _WIN32
            const std::lock_guard<std::mutex> lock(stateMutex);
            const ProgramCapture *best = BestProgram();
            return best ? best->mvp : nullptr;
#else
            return nullptr;
#endif
        }

        bool MatrixTransposed()
        {
#if _WIN32
            const std::lock_guard<std::mutex> lock(stateMutex);
            const ProgramCapture *best = BestProgram();
            return best ? best->mvpTransposed : false;
#else
            return false;
#endif
        }

        bool HasViewPosition()
        {
#if _WIN32
            const std::lock_guard<std::mutex> lock(stateMutex);
            for (const auto &entry : programCaptures)
                if (entry.second.hasViewPosition)
                    return true;
#endif
            return false;
        }

        const float *ViewPosition()
        {
#if _WIN32
            const std::lock_guard<std::mutex> lock(stateMutex);
            const ProgramCapture *bestViewPos = nullptr;
            for (const auto &entry : programCaptures)
                if (entry.second.hasViewPosition && (!bestViewPos || entry.second.viewPosUploads > bestViewPos->viewPosUploads))
                    bestViewPos = &entry.second;
            return bestViewPos ? bestViewPos->viewPosition : nullptr;
#else
            return nullptr;
#endif
        }
    }
}
