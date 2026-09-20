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
        constexpr float FULL_VOLUME_RADIUS = 1.0f;
        constexpr float AUDIBLE_RADIUS = 4.0f;

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
        bool pausedForDistance = false;

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
            pausedForDistance = false;
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
                pausedForDistance = false;
                currentVehicle = vid;
                downloadName = file;
                downloadRetryFrames = 0;
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

        // start once the file landed
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
            const std::string path = std::string("sandium/cache/") + downloadName;
            try
            {
                if (currentVehicle >= structs::Vehicle::VanillaCount || !addresses::Vehicles.ptr ||
                    !addresses::Vehicles[currentVehicle].isActive.b1)
                    throw std::runtime_error("radio vehicle is no longer active");
                current = api::Sound::Load3D(path);
                const structs::Vehicle &car = addresses::Vehicles[currentVehicle];
                current->Play3D(car.position.x, car.position.y, car.position.z, 1.0f, true);
                pausedForDistance = false;
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

        // positional gains from the car
        if (!current)
            return;
        if (currentVehicle >= structs::Vehicle::VanillaCount || !addresses::Vehicles.ptr ||
            !addresses::Vehicles[currentVehicle].isActive.b1)
        {
            current->Stop();
            current.reset();
            pausedForDistance = false;
            return;
        }
        if (!api::glcap::HasLastView() || !api::glcap::LastViewPositionValid())
            return;

        const float *cam = api::glcap::LastViewPosition();
        const structs::Vehicle &car = addresses::Vehicles[currentVehicle];
        const float dx = car.position.x - cam[0];
        const float dy = car.position.y - cam[1];
        const float dz = car.position.z - cam[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist >= AUDIBLE_RADIUS)
        {
            if (!pausedForDistance)
            {
                current->SetVolume(0.0f);
                pausedForDistance = true;
            }
            return;
        }

        if (pausedForDistance)
        {
            current->SetVolume(1.0f);
            pausedForDistance = false;
        }

        const float distanceT = std::clamp((dist - FULL_VOLUME_RADIUS) /
                                           (AUDIBLE_RADIUS - FULL_VOLUME_RADIUS),
                                           0.0f, 1.0f);
        // Smooth inverse rolloff: loud beside the car, rapidly quieter as the
        // listener walks away, and exactly silent at the cutoff.
        const float smoothT = distanceT * distanceT * (3.0f - 2.0f * distanceT);
        const float atten = (1.0f - smoothT) * (1.0f - smoothT);

        current->SetPosition(car.position.x, car.position.y, car.position.z);
        // Sub Rosa applies its own inverse-distance attenuation before HRTF.
        // This envelope preserves the requested four-unit hard cutoff.
        current->SetVolume(atten);
#endif
    }

    void StopSession()
    {
#if _WIN32
        StopCurrentSource();
#endif
    }
}
