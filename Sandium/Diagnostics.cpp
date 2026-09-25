#include "Diagnostics.hpp"

#include <chrono>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <mutex>

namespace diag
{
    namespace
    {
        std::mutex logMutex;
        bool cleared = false;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        char lastEntry[1100] = "";

        void Write(bool truncate, const char *line)
        {
            std::FILE *f = std::fopen("sandium_log.txt", truncate ? "w" : "a");
            if (!f)
                return;
            std::fputs(line, f);
            std::fputc('\n', f);
            std::fclose(f);
        }
    }

    void Log(const char *tag, const char *fmt, ...)
    {
        char message[1024];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(message, sizeof(message), fmt, args);
        va_end(args);

        char entry[1100];
        std::snprintf(entry, sizeof(entry), "%s\x01%s", tag, message);

        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();

        char line[1152];
        std::snprintf(line, sizeof(line), "[+%07.3f][%s] %s", seconds, tag, message);

        std::lock_guard<std::mutex> lock(logMutex);
        if (cleared && std::strcmp(entry, lastEntry) == 0)
            return; // periodic status dumps repeat every few frames; log changes only
        std::snprintf(lastEntry, sizeof(lastEntry), "%s", entry);
        Write(!cleared, line);
        cleared = true;
    }
}
