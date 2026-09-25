#include "ServerMedia.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "Addon.hpp"
#include "LuaManager.hpp"
#include "NetRedirect.hpp"
#include "api/Http.hpp"

// Server media streaming.
//
// The game server publishes content for clients via the master's HTTP endpoint:
//   GET /stream/manifest.txt  -> one file name per line
//   GET /stream/<name>        -> the file bytes
// After joining the Noxus game server, the client fetches the manifest and downloads every file
// into sandium/cache/, skipping ones already present. Lua addons pick the
// files up through the ServerMedia table (Ready/Path) and feed them into the
// existing Image/Video APIs.
//
// If the manifest contains init.lua, the server is publishing a whole client
// addon: it is materialized as sandium/addons/server_stream (a real addon
// folder) and loaded from the main thread via Tick() only while connected to
// that game server. A cached copy is never discovered as a local addon.

namespace servermedia
{
    namespace
    {
        constexpr const char *MASTER_HOST = "185.227.111.150";
        constexpr uint16_t MASTER_HTTP_PORT = 80;

        std::atomic<bool> started{false};
        std::mutex filesMutex;
        std::map<std::string, std::string> readyFiles; // name -> local path
        std::atomic<bool> pendingAddon{false};

        std::filesystem::path CacheDir()
        {
            return std::filesystem::path("sandium") / "cache";
        }

        bool SafeName(const std::string &name)
        {
            if (name.empty() || name.size() > 128)
                return false;
            for (char c : name)
                if (c == '/' || c == '\\' || c == ':' || c == '<' || c == '>' ||
                    c == '"' || c == '|' || c == '?' || c == '*')
                    return false;
            return name != "." && name != "..";
        }

        void WriteFile(const std::filesystem::path &dest, const std::string &data)
        {
            std::error_code ec;
            std::filesystem::create_directories(dest.parent_path(), ec);
            std::ofstream out(dest, std::ios::binary | std::ios::trunc);
            if (!out)
                return;
            out.write(data.data(), (std::streamsize)data.size());
        }

        // Turn the streamed init.lua into a loadable addon folder.
        void MaterializeAddon(const std::string &initLua)
        {
            const auto addonDir = std::filesystem::path("sandium") / "addons" / "server_stream";
            const auto manifestPath = addonDir / "addon.json";
            if (!std::filesystem::exists(manifestPath))
                WriteFile(manifestPath,
                          "{\n"
                          "  \"id\": \"server_stream\",\n"
                          "  \"name\": \"Server Stream\",\n"
                          "  \"description\": \"Content published by the server (streamed automatically).\",\n"
                          "  \"requires\": [],\n"
                          "  \"conflicts\": []\n"
                          "}\n");
            WriteFile(addonDir / "client" / "init.lua", initLua);
        }

        void SyncThread()
        {
            if (!netredirect::IsNoxusGame()) return;
            const std::string manifest = http::Get(MASTER_HOST, MASTER_HTTP_PORT,
                                                   "/stream/manifest.txt", 4000,
                                                   "SANDIUM_MASTER");
            if (manifest.empty() || !netredirect::IsNoxusGame())
                return;

            std::error_code ec;
            std::filesystem::create_directories(CacheDir(), ec);

            std::set<std::string> names;
            std::string line;
            auto take = [&](std::string &l)
            {
                std::string trimmed;
                for (char t : l)
                    if (t != '\r' && t != ' ' && t != '\t')
                        trimmed += t;
                if (SafeName(trimmed))
                    names.insert(trimmed);
                l.clear();
            };
            for (char c : manifest)
            {
                if (c == '\n')
                    take(line);
                else
                    line += c;
            }
            if (!line.empty())
                take(line);

            std::string initLua;
            bool hasInit = false;

            for (const auto &name : names)
            {
                if (!netredirect::IsNoxusGame()) return;
                const auto dest = CacheDir() / name;

                // init.lua also materializes the streamed addon (re-read every
                // launch so server-side script updates apply on relaunch).
                if (name == "init.lua")
                {
                    const std::string data = http::Get(MASTER_HOST, MASTER_HTTP_PORT,
                                                       "/stream/" + name, 10000,
                                                       "SANDIUM_MASTER");
                    if (data.empty())
                        continue;
                    WriteFile(dest, data);
                    initLua = data;
                    hasInit = true;
                    std::lock_guard<std::mutex> lock(filesMutex);
                    readyFiles[name] = std::filesystem::absolute(dest).string();
                    continue;
                }

                if (std::filesystem::exists(dest, ec))
                {
                    std::lock_guard<std::mutex> lock(filesMutex);
                    readyFiles[name] = std::filesystem::absolute(dest).string();
                    continue; // already downloaded on an earlier launch
                }

                const std::string data = http::Get(MASTER_HOST, MASTER_HTTP_PORT,
                                                   "/stream/" + name, 30000,
                                                   "SANDIUM_MASTER");
                if (data.empty())
                    continue;

                WriteFile(dest, data);
                std::ifstream verify(dest, std::ios::binary);
                if (!verify)
                    continue;
                verify.close();

                std::lock_guard<std::mutex> lock(filesMutex);
                readyFiles[name] = std::filesystem::absolute(dest).string();
            }

            if (hasInit && netredirect::IsNoxusGame())
            {
                MaterializeAddon(initLua);
                pendingAddon = true; // loaded on the main thread by Tick()
            }
        }
    }

    void Sync()
    {
        if (!netredirect::IsNoxusGame()) return;
        if (started.exchange(true))
            return;
        std::thread(SyncThread).detach();
    }

    bool Ready(const std::string &name)
    {
        if (!netredirect::IsNoxusGame()) return false;
        std::lock_guard<std::mutex> lock(filesMutex);
        return readyFiles.count(name) != 0;
    }

    std::string Path(const std::string &name)
    {
        if (!netredirect::IsNoxusGame()) return {};
        std::lock_guard<std::mutex> lock(filesMutex);
        const auto it = readyFiles.find(name);
        return it == readyFiles.end() ? std::string() : it->second;
    }

    void Tick()
    {
        if (!netredirect::IsNoxusGame())
            return;
        Sync();
        if (!GetMainLuaManager())
            return;

        // Keep the Lua state alive across disconnects. Its hooks are gated by
        // IsNoxusGame; unloading it here can invalidate callbacks during reset.
        for (const auto &existing : GetAddons())
            if (existing->ID() == "server_stream")
            {
                if (!existing->IsLoaded() && Ready("init.lua"))
                {
                    existing->Load();
                    if (existing->IsLoaded() && !existing->IsDisabled() &&
                        existing->CheckDependencies())
                        existing->PrepareLua(GetMainLuaManager(), false);
                }
                return;
            }

        if (!pendingAddon.exchange(false)) return;

        auto addon = std::make_unique<Addon>("sandium/addons/server_stream");
        addon->Load();
        if (addon->IsLoaded() && !addon->IsDisabled() && addon->CheckDependencies())
        {
            addon->PrepareLua(GetMainLuaManager(), false);
            RegisterAddon(std::move(addon));
        }
    }
}
