#pragma once

#include <string>

namespace flags
{
    // Start the background poller for the server's /stream/flags.txt
    // (name=value lines, refreshed every 2s). Safe to call repeatedly.
    void Start();

    // Value of a flag, or "" if unknown. e.g. Get("homicide_hud") == "1"
    std::string Get(const std::string &name);
}
