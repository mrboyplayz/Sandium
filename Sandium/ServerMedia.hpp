#pragma once

#include <string>

namespace servermedia
{
    // Start the manifest+files sync from the server's /stream/ endpoint into
    // sandium/cache/ (once per process, background thread). Safe to call
    // repeatedly from Lua.
    void Sync();

    // True once <name> has been downloaded and is available locally.
    bool Ready(const std::string &name);

    // Local path of the downloaded <name> ("" while not ready).
    std::string Path(const std::string &name);

    // Main-thread pump: loads the streamed addon once its files arrived.
    void Tick();
}
