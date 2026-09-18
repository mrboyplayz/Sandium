#include "Flags.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "api/Http.hpp"

// Server-controlled flags: the server publishes name=value pairs in
// /opt/subrosa/stream/flags.txt (served by the master at /stream/flags.txt);
// game-server commands (e.g. /hmcd) rewrite the file, clients pick it up
// within seconds. Lets scripted server content react live.

namespace flags
{
    namespace
    {
        constexpr const char *MASTER_HOST = "185.227.111.150";
        constexpr uint16_t MASTER_HTTP_PORT = 80;
        constexpr int POLL_SECONDS = 2;

        std::atomic<bool> started{false};
        std::mutex mapMutex;
        std::map<std::string, std::string> flagValues;

        void Apply(const std::string &body)
        {
            std::map<std::string, std::string> parsed;
            std::string line;
            auto take = [&](std::string &l)
            {
                const size_t eq = l.find('=');
                if (eq != std::string::npos && !l.empty())
                    parsed[l.substr(0, eq)] = l.substr(eq + 1);
                l.clear();
            };
            for (char c : body)
            {
                if (c == '\n' || c == '\r')
                {
                    if (!line.empty())
                        take(line);
                }
                else
                    line += c;
            }
            if (!line.empty())
                take(line);
            if (parsed.empty())
                return; // keep previous flags on a bad fetch

            std::lock_guard<std::mutex> lock(mapMutex);
            flagValues = std::move(parsed);
        }

        void ThreadProc()
        {
            for (;;)
            {
                const std::string body = http::Get(MASTER_HOST, MASTER_HTTP_PORT,
                                                   "/stream/flags.txt", 3000,
                                                   "SANDIUM_MASTER");
                if (!body.empty())
                    Apply(body);
                std::this_thread::sleep_for(std::chrono::seconds(POLL_SECONDS));
            }
        }
    }

    void Start()
    {
        if (started.exchange(true))
            return;
        std::thread(ThreadProc).detach();
    }

    std::string Get(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(mapMutex);
        const auto it = flagValues.find(name);
        return it == flagValues.end() ? std::string() : it->second;
    }
}
