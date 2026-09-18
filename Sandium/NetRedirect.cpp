#include "NetRedirect.hpp"

#include <subhook.h>

#if _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

// The real master's IP is hardcoded in the exe (inet_addr("66.226.72.227"),
// also the A record of www.crypticsea.com) and it is ALIVE — the vanilla
// browser asks it for the community list next to ours, so real internet
// servers show up in the browser. Redirect every connect/sendto aimed at it
// to our own master instead, inside the process, so every Sandium client
// only ever sees our list.

namespace netredirect
{
    namespace
    {
        subhook::Hook sendtoHook;
        subhook::Hook connectHook;

        in_addr realMaster{};
        in_addr ourMaster{};

        bool RedirectDest(const sockaddr *&to, sockaddr_in &patched)
        {
            if (!to || to->sa_family != AF_INET)
                return false;
            const auto *in = reinterpret_cast<const sockaddr_in *>(to);
            if (in->sin_addr.s_addr != realMaster.s_addr)
                return false;
            patched = *in;
            patched.sin_addr = ourMaster;
            // The real master's UDP ports (27590/27592/...) are not ours;
            // everything non-HTTP goes to our master protocol port 28015.
            const u_short port = ntohs(patched.sin_port);
            if (port != 80 && port != 28015 && port != 28017)
                patched.sin_port = htons(28015);
            to = reinterpret_cast<const sockaddr *>(&patched);
            return true;
        }

        int WSAAPI SendtoHook(SOCKET s, const char *buf, int len, int flags,
                              const sockaddr *to, int tolen)
        {
            sockaddr_in patched{};
            RedirectDest(to, patched);
            subhook::ScopedHookRemove remove(&sendtoHook);
            return sendto(s, buf, len, flags, to, tolen);
        }

        int WSAAPI ConnectHook(SOCKET s, const sockaddr *name, int namelen)
        {
            sockaddr_in patched{};
            RedirectDest(name, patched);
            subhook::ScopedHookRemove remove(&connectHook);
            return connect(s, name, namelen);
        }
    }

    void Install()
    {
        realMaster.s_addr = inet_addr("66.226.72.227");
        ourMaster.s_addr = inet_addr("185.227.111.150");
        if (realMaster.s_addr == INADDR_NONE || ourMaster.s_addr == INADDR_NONE)
            return;

        HMODULE wsock = GetModuleHandleA("ws2_32.dll");
        if (!wsock)
            wsock = LoadLibraryA("ws2_32.dll");
        if (!wsock)
            return;

        if (auto p = GetProcAddress(wsock, "sendto"))
        {
            sendtoHook.Install(p, &SendtoHook, subhook::HookFlag64BitOffset);
            // Keep the trampoline alive so the original stays callable.
        }
        if (auto p = GetProcAddress(wsock, "connect"))
            connectHook.Install(p, &ConnectHook, subhook::HookFlag64BitOffset);
    }
}
#endif
