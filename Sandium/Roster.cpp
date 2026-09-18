#include "Roster.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "api/Http.hpp"

// Account roster: name -> phone, fetched from the master's
// /stream/roster.txt (generated from the master's phone assignments).
// Used by the name renderer to highlight special numbers (256-0001 gold).

namespace roster
{
    namespace
    {
        constexpr const char *MASTER_HOST = "185.227.111.150";
        constexpr uint16_t MASTER_HTTP_PORT = 80;
        constexpr int REFETCH_SECONDS = 20;

        std::atomic<bool> started{false};
        std::mutex mapMutex;
        std::map<std::string, std::uint32_t> phoneByName;

        bool LooksLikeIpKey(const std::string &key)
        {
            // IP-fallback phone keys ("94.99.165.6") and internal udp:<hex>
            // keys are not account names.
            int dots = 0;
            bool allDigitOrDot = !key.empty();
            for (char c : key)
            {
                if (c == '.')
                    ++dots;
                else if (c < '0' || c > '9')
                    allDigitOrDot = false;
            }
            if (allDigitOrDot && dots == 3)
                return true;
            return key.rfind("udp:", 0) == 0;
        }

        // "256-0004" -> 2560004; 0 on failure
        std::uint32_t PhoneStringToInt(const std::string &s)
        {
            unsigned prefix = 0, suffix = 0;
            if (std::sscanf(s.c_str(), "%u-%u", &prefix, &suffix) != 2)
                return 0;
            if (prefix > 999 || suffix > 9999)
                return 0;
            return prefix * 10000u + suffix;
        }

        void ApplyRoster(const std::string &body)
        {
            std::map<std::string, std::uint32_t> parsed;
            std::string line;
            auto take = [&](std::string &l)
            {
                const size_t tab = l.find('\t');
                if (tab != std::string::npos)
                {
                    const std::string name = l.substr(0, tab);
                    const std::string phone = l.substr(tab + 1);
                    if (!LooksLikeIpKey(name) && !name.empty())
                    {
                        if (const std::uint32_t number = PhoneStringToInt(phone))
                            parsed[name] = number;
                    }
                }
                l.clear();
            };
            for (char c : body)
            {
                if (c == '\n')
                    take(line);
                else
                    line += c;
            }
            if (!line.empty())
                take(line);

            if (parsed.empty())
                return; // keep the previous roster on a bad fetch
            std::lock_guard<std::mutex> lock(mapMutex);
            phoneByName = std::move(parsed);
        }

        void ThreadProc()
        {
            for (;;)
            {
                const std::string body = http::Get(MASTER_HOST, MASTER_HTTP_PORT,
                                                   "/stream/roster.txt", 4000,
                                                   "SANDIUM_MASTER");
                if (!body.empty())
                    ApplyRoster(body);
                std::this_thread::sleep_for(std::chrono::seconds(REFETCH_SECONDS));
            }
        }
    }

    void Start()
    {
        if (started.exchange(true))
            return;
        std::thread(ThreadProc).detach();
    }

    std::uint32_t PhoneForName(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(mapMutex);
        const auto it = phoneByName.find(name);
        return it == phoneByName.end() ? 0 : it->second;
    }
}
