#pragma once

#include <cstdint>

namespace phone
{
    // Fetch (once per process) a persistent phone number for this account
    // from the master's /phone endpoint and write it into the client state
    // so the main menu displays it. Call from the menu draw; safe to call
    // every frame -- it no-ops after the first attempt.
    void Assign();
}
