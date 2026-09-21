#pragma once

#include <cstdint>
#include <string>

namespace http
{
    // Minimal blocking HTTP/1.1 GET. Returns the response body, or "" on any
    // failure. The host can be overridden per machine with the given env var
    // (falls back to the VPS master).
    std::string Get(const std::string &host, uint16_t port, const std::string &path,
                    int timeoutMs = 3000, const char *envOverride = nullptr);

    // Minimal blocking JSON POST. Returns the response body, or "" on failure.
    std::string PostJson(const std::string &host, uint16_t port, const std::string &path,
                         const std::string &jsonBody, int timeoutMs = 3000,
                         const char *envOverride = nullptr);
}
