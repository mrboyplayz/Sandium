#include "RadioPlayer.hpp"

#include "Addresses.hpp"
#include "Flags.hpp"
#include "api/Http.hpp"
#include "api/Sound.hpp"
#include "api/GLUniforms.hpp"
#include "structs/Vehicle.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
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
        constexpr float AUDIBLE_RADIUS = 90.0f;

        long long lastSerial = -1;
        std::shared_ptr<api::Sound> current;
        unsigned int currentVehicle = 0xFFFFFFFFu;
        std::atomic<bool> downloadPending{false};
        std::atomic<bool> downloadDone{false};
        std::string downloadName;

        void DownloadThread(std::string file)
        {
            const std::string body = http::Get(MASTER_HOST, MASTER_HTTP_PORT,
                                               "/stream/" + file, 120000,
                                               "SANDIUM_MASTER");
            if (!body.empty())
            {
                std::ofstream out(std::string("sandium/cache/") + file,
                                  std::ios::binary | std::ios::trunc);
                if (out)
                {
                    out.write(body.data(), (std::streamsize)body.size());
                    out.close();
                    if (out)
                        downloadDone = true;
                }
            }
            downloadPending = false;
        }
    }

    void Update()
    {
#if _WIN32
        // new radio state from the server flags?
        const std::string value = flags::Get("radio");
        if (!value.empty())
        {
            long long serial = 0;
            char file[128] = {};
            unsigned vid = 0xFFFFFFFFu;
            if (std::sscanf(value.c_str(), "%lld %127s %u", &serial, file, &vid) == 3 &&
                serial > lastSerial)
            {
                lastSerial = serial;
                if (current)
                {
                    current->Stop();
                    current.reset();
                }
                currentVehicle = vid;
                downloadName = file;
                downloadDone = false;
                if (!downloadPending.exchange(true))
                    std::thread(DownloadThread, file).detach();
            }
        }

        // start once the file landed
        if (downloadDone && !current)
        {
            downloadDone = false;
            const std::string path = std::string("sandium/cache/") + downloadName;
            try
            {
                current = api::Sound::Load(path);
                current->Play(1.0f);
            }
            catch (...)
            {
                current.reset();
            }
        }

        // positional gains from the car
        if (!current)
            return;
        if (currentVehicle >= structs::Vehicle::VanillaCount ||
            !addresses::Vehicles[currentVehicle].isActive.b1)
        {
            current->Stop();
            current.reset();
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
        float atten = 1.0f - dist / AUDIBLE_RADIUS;
        if (atten <= 0.0f)
        {
            current->SetGainLR(0.0f, 0.0f);
            return;
        }
        atten *= atten;

        float pan = 0.0f;
        if (api::glcap::HasLastView())
        {
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
            const float len = dist > 0.001f ? dist : 1.0f;
            pan = (dx * right[0] + dy * right[1] + dz * right[2]) / len;
        }
        current->SetGainLR(atten * (0.5f - 0.45f * pan) + 0.001f,
                           atten * (0.5f + 0.45f * pan) + 0.001f);
#endif
    }
}
