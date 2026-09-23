#include "RadioPlayer.hpp"

#include "Addresses.hpp"
#include "Flags.hpp"
#include "api/Http.hpp"
#include "api/Sound.hpp"
#include "api/GLUniforms.hpp"
#include "api/Logging.hpp"
#include "structs/Human.hpp"
#include "structs/Vehicle.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>

#if _WIN32
#include <Windows.h>
#endif

namespace radioplayer
{
    namespace
    {
        constexpr const char *MASTER_HOST = "185.227.111.150";
        constexpr uint16_t MASTER_HTTP_PORT = 80;
        // Keep radio decoding on Sound's streaming audio thread.  Loading an
        // MP3 as a native 3D sample decodes the entire song on DrawHUD, which
        // stalls rendering during joins.  Stereo gain/pan below provides a
        // stable car-relative source without touching native source internals.
        constexpr float AUDIBLE_RADIUS = 28.0f;

        constexpr int DOWNLOAD_RETRY_FRAMES = 300;

        std::string lastValue;
        std::shared_ptr<api::Sound> current;
        unsigned int currentVehicle = 0xFFFFFFFFu;
        std::atomic<bool> downloadPending{false};
        std::atomic<bool> downloadDone{false};
        std::atomic<bool> downloadFailed{false};
        std::string downloadName;
        std::mutex completedNameMutex;
        std::string completedName;
        int downloadRetryFrames = 0;
        std::atomic<bool> decodePending{false};
        std::mutex decodedSoundMutex;
        std::shared_ptr<api::Sound> decodedSound;
        std::string decodedName;

        void StopCurrentSource()
        {
            if (current)
            {
                current->Stop();
                current.reset();
            }
            currentVehicle = 0xFFFFFFFFu;
            downloadName.clear();
            downloadDone = false;
            downloadFailed = false;
            downloadRetryFrames = 0;
            decodePending = false;
            const std::lock_guard<std::mutex> lock(decodedSoundMutex);
            decodedSound.reset();
            decodedName.clear();
        }

        void DownloadThread(std::string file)
        {
            const std::string body = http::Get(MASTER_HOST, MASTER_HTTP_PORT,
                                               "/stream/" + file, 120000,
                                               "SANDIUM_MASTER");
            bool saved = false;
            // A real MP3 will be much larger than an nginx 404/error page.
            if (body.size() >= 1024)
            {
                std::error_code ec;
                std::filesystem::create_directories("sandium/cache", ec);
                const std::string path = std::string("sandium/cache/") + file;
                const std::string partPath = path + ".part";
                std::ofstream out(partPath,
                                  std::ios::binary | std::ios::trunc);
                if (out)
                {
                    out.write(body.data(), (std::streamsize)body.size());
                    out.close();
                    if (out)
                    {
                        std::filesystem::remove(path, ec);
                        ec.clear();
                        std::filesystem::rename(partPath, path, ec);
                        saved = !ec;
                    }
                }
            }
            if (saved)
            {
                {
                    const std::lock_guard<std::mutex> lock(completedNameMutex);
                    completedName = file;
                }
                api::GetSandiumLogger()->Log("Radio downloaded {} ({} bytes)", file, body.size());
                downloadDone = true;
            }
            else
            {
                api::GetSandiumLogger()->Log("<yellow>Radio download failed for {}; retrying", file);
                downloadFailed = true;
            }
            downloadPending = false;
        }

        void StartDownload()
        {
            if (downloadName.empty() || downloadPending.exchange(true))
                return;
            downloadDone = false;
            downloadFailed = false;
            api::GetSandiumLogger()->Log("Radio downloading {}", downloadName);
            std::thread(DownloadThread, downloadName).detach();
        }

        void DecodeThread(std::string file)
        {
            try
            {
                auto sound = api::Sound::Load(std::string("sandium/cache/") + file);
                const std::lock_guard<std::mutex> lock(decodedSoundMutex);
                decodedName = file;
                decodedSound = std::move(sound);
            }
            catch (const std::exception &error)
            {
                api::GetSandiumLogger()->Log("<red>Radio decode failed: {}", error.what());
            }
            decodePending = false;
        }

        void StartDecode()
        {
            if (downloadName.empty() || decodePending.exchange(true))
                return;
            std::thread(DecodeThread, downloadName).detach();
        }
    }

    void Update()
    {
#if _WIN32
        if (!addresses::IsInGame.ptr || !*addresses::IsInGame)
        {
            StopSession();
            return;
        }
        // new radio state from the server flags?
        const std::string value = flags::Get("radio");
        if (!value.empty())
        {
            long long serial = 0;
            char file[128] = {};
            unsigned vid = 0xFFFFFFFFu;
            if (std::sscanf(value.c_str(), "%lld %127s %u", &serial, file, &vid) == 3 &&
                value != lastValue)
            {
                lastValue = value;
                if (current)
                {
                    current->Stop();
                    current.reset();
                }
                currentVehicle = vid;
                downloadName = file;
                downloadRetryFrames = 0;
                decodePending = false;
                {
                    const std::lock_guard<std::mutex> lock(decodedSoundMutex);
                    decodedSound.reset();
                    decodedName.clear();
                }
                StartDownload();
            }
        }

        if (downloadFailed.exchange(false))
            downloadRetryFrames = DOWNLOAD_RETRY_FRAMES;

        if (!current && !downloadPending && !downloadDone && !downloadName.empty())
        {
            if (downloadRetryFrames > 0)
                --downloadRetryFrames;
            else
                StartDownload();
        }

        // Decode/initialize audio away from DrawHUD. Constructing a Media
        // Foundation reader or opening an endpoint can stall a rendered frame.
        if (downloadDone && !current)
        {
            downloadDone = false;
            std::string landedName;
            {
                const std::lock_guard<std::mutex> lock(completedNameMutex);
                landedName = completedName;
            }
            if (landedName != downloadName)
            {
                downloadRetryFrames = 0;
                return;
            }
            StartDecode();
        }

        // Start the fully prepared streaming player on the game thread.
        if (!current && !decodePending)
        {
            std::shared_ptr<api::Sound> ready;
            {
                const std::lock_guard<std::mutex> lock(decodedSoundMutex);
                if (decodedName == downloadName)
                {
                    ready = decodedSound;
                    decodedSound.reset();
                    decodedName.clear();
                }
            }
            if (ready)
            {
            try
            {
                if (currentVehicle >= structs::Vehicle::VanillaCount || !addresses::Vehicles.ptr ||
                    !addresses::Vehicles[currentVehicle].isActive.b1)
                    throw std::runtime_error("radio vehicle is no longer active");
                current = std::move(ready);
                current->Play(1.0f);
                api::GetSandiumLogger()->Log("Radio playing {} on vehicle {}", downloadName, currentVehicle);
                downloadName.clear();
            }
            catch (const std::exception &error)
            {
                api::GetSandiumLogger()->Log("<red>Radio playback failed: {}", error.what());
                current.reset();
                downloadName.clear();
            }
            }
        }

        // positional gains from the car
        if (!current)
            return;
        if (currentVehicle >= structs::Vehicle::VanillaCount || !addresses::Vehicles.ptr ||
            !addresses::Vehicles[currentVehicle].isActive.b1)
        {
            current->Stop();
            current.reset();
            return;
        }
        // View-position uniforms may belong to a reflection/shadow pass.
        // The player's replicated human position is the stable listener
        // location to use for the radio's distance cutoff.
        const structs::Human *listener = nullptr;
        for (std::size_t humanID = 0; humanID < structs::Human::VanillaCount; ++humanID)
            if (addresses::Humans[humanID].isActive.b1)
            {
                listener = &addresses::Humans[humanID];
                break;
            }
        if (!listener || !api::glcap::HasLastView())
            return;

        const structs::Vehicle &car = addresses::Vehicles[currentVehicle];
        const float dx = car.position.x - listener->position.x;
        const float dy = car.position.y - listener->position.y;
        const float dz = car.position.z - listener->position.z;
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        float atten = 1.0f - dist / AUDIBLE_RADIUS;
        if (atten <= 0.0f)
        {
            current->SetGainLR(0.0f, 0.0f);
            return;
        }
        atten *= atten;

        // Pan against the camera's right vector so the radio tracks the car
        // as it moves around the listener rather than sounding screen-fixed.
        const float *view = api::glcap::LastViewMatrix();
        float right[3];
        if (api::glcap::LastViewTransposed())
        {
            right[0] = view[0]; right[1] = view[4]; right[2] = view[8];
        }
        else
        {
            right[0] = view[0]; right[1] = view[1]; right[2] = view[2];
        }
        const float length = dist > 0.001f ? dist : 1.0f;
        const float pan = (dx * right[0] + dy * right[1] + dz * right[2]) / length;
        current->SetGainLR(atten * (0.5f - 0.50f * pan),
                           atten * (0.5f + 0.50f * pan));
#endif
    }

    void StopSession()
    {
#if _WIN32
        StopCurrentSource();
#endif
    }
}
