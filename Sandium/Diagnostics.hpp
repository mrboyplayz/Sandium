#pragma once

// Single-file diagnostics. Every debug dump Sandium used to scatter into its
// own sandium_*.txt file goes through diag::Log into one sandium_log.txt in
// the working directory (truncated once per launch, appended afterwards).
namespace diag
{
    // Append "[+<seconds>][tag] message" to sandium_log.txt. fmt is a printf
    // format string; safe to call from any thread.
    void Log(const char *tag, const char *fmt, ...);
}
