#include "Phone.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "Addresses.hpp"
#include "api/Http.hpp"
#include <cstdlib>

#if _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#pragma comment(lib, "ws2_32.lib")
#endif

// Phone number assignment for the main menu.
//
// The vanilla flow assigns phones via the master's 'H' auth exchange, which
// the Noxus repack client never sends (zero 0x48 packets observed). So we ask
// the master's /phone endpoint directly (same persistent numbering the master
// uses, keyed by the account name) and write the result into the client state
// the menu reads: dword[134050337] = phone as prefix*10000+suffix (2560001 ->
// "256-0001"), dword[134050338] = auth-accepted flag,
// dword[179668277] = 2 (authed -> menu shows the number).

namespace phone
{
    namespace
    {
        constexpr const char *MASTER_HOST = "185.227.111.150";
        constexpr const char *MASTER_HOST_ENV = "SANDIUM_MASTER";
        constexpr uint16_t MASTER_HTTP_PORT = 80;

        const char *httpHost()
        {
            static char envHost[64];
            if (GetEnvironmentVariableA(MASTER_HOST_ENV, envHost, sizeof(envHost)) > 0)
                return envHost;
            return MASTER_HOST;
        }
        uint16_t httpPort() { return MASTER_HTTP_PORT; }

        // Client state indices (from the client decomp).
        constexpr uint32_t IDX_TOKEN = 134050334; // join-packet identity token
        constexpr uint32_t IDX_PHONE = 134050337;
        constexpr uint32_t IDX_ACCEPT = 134050338;
        constexpr uint32_t IDX_AUTH_STATE = 179668277;
        constexpr uint32_t IDX_ACCOUNT_ID = 73991096; // 32-byte account name

        std::atomic<bool> started{false};
        std::atomic<bool> done{false};

        uint32_t *StatePtr(uint32_t index)
        {
            const auto base = reinterpret_cast<std::uintptr_t>(addresses::Base.ptr);
            if (!base)
                return nullptr;
            return reinterpret_cast<uint32_t *>(base + 0x404A6C + 4ULL * index);
        }

        // "256-0004" -> 2560004; 0 on parse failure
        uint32_t PhoneStringToInt(const std::string &s)
        {
            unsigned prefix = 0, suffix = 0;
            if (std::sscanf(s.c_str(), "%u-%u", &prefix, &suffix) != 2)
                return 0;
            if (prefix > 999 || suffix > 9999)
                return 0;
            return prefix * 10000u + suffix;
        }

        // Blocking HTTP GET /phone?account=<name>; returns the body or "".
        std::string FetchPhone(const std::string &account)
        {
        #if _WIN32
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
                return "";

            const char *host = httpHost();

            struct addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            struct addrinfo *res = nullptr;
            if (getaddrinfo(host, "80", &hints, &res) != 0 || !res)
            {
                WSACleanup();
                return "";
            }

            SOCKET sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            if (sock == INVALID_SOCKET)
            {
                freeaddrinfo(res);
                WSACleanup();
                return "";
            }
            // Don't hang the thread for more than 3s.
            DWORD timeout = 3000;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));

            std::string body;
            if (connect(sock, res->ai_addr, (int)res->ai_addrlen) == 0)
            {
                std::string req = "GET /phone?account=" + account + " HTTP/1.1\r\nHost: " +
                                  std::string(host) + "\r\nConnection: close\r\n\r\n";
                send(sock, req.c_str(), (int)req.size(), 0);

                char buf[2048];
                int n;
                std::string raw;
                while ((n = recv(sock, buf, sizeof(buf), 0)) > 0)
                    raw.append(buf, n);

                const size_t sep = raw.find("\r\n\r\n");
                if (sep != std::string::npos)
                    body = raw.substr(sep + 4);
                // Trim trailing whitespace/newlines.
                while (!body.empty() && (body.back() == '\r' || body.back() == '\n' ||
                                         body.back() == ' ' || body.back() == '\t'))
                    body.pop_back();
            }
            closesocket(sock);
            freeaddrinfo(res);
            WSACleanup();
            return body;
        #else
            (void)account;
            return "";
        #endif
        }

        void AssignThread()
        {
            // Account name: the 32-byte buffer the client puts in its join
            // packet ("CROW 2 367"). Use it as the persistent phone key.
            char name[33];
            name[0] = 0;
            const auto base = reinterpret_cast<std::uintptr_t>(addresses::Base.ptr);
            if (base)
            {
                const char *id = reinterpret_cast<const char *>(base + 0x404A6C + 4ULL * IDX_ACCOUNT_ID);
                std::memcpy(name, id, 32);
                name[32] = 0;
            }
            // URL-encode the name roughly (spaces etc.).
            std::string key;
            for (const char *p = name; *p; ++p)
            {
                const unsigned char c = (unsigned char)*p;
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                    key += (char)c;
                else
                {
                    char hex[4];
                    std::snprintf(hex, sizeof(hex), "%%%02X", c);
                    key += hex;
                }
            }
            if (key.empty())
                key = "default";

            const std::string phoneBody = FetchPhone(key);
            const uint32_t phoneInt = PhoneStringToInt(phoneBody);
            // Unique identity token: the server keys accounts on the
            // (token, session) pair of the join packet. Without this every
            // client sends token=-1 and the server merges all players.
            uint32_t tokenInt = 0;
            {
                const std::string body = http::Get(httpHost(), httpPort(),
                                                   "/token?account=" + key, 3000,
                                                   "SANDIUM_MASTER");
                tokenInt = (uint32_t)std::strtoull(body.c_str(), nullptr, 10);
                if (tokenInt == 0xFFFFFFFFu)
                    tokenInt = 0;
            }
            if (phoneInt || tokenInt)
            {
                uint32_t *phone = StatePtr(IDX_PHONE);
                uint32_t *token = StatePtr(IDX_TOKEN);
                uint32_t *accept = StatePtr(IDX_ACCEPT);
                uint32_t *state = StatePtr(IDX_AUTH_STATE);
                if (phone) *phone = phoneInt;
                if (token && tokenInt) *token = tokenInt;
                if (accept && state)
                {
                    *accept = 1;
                    *state = 2; // menu switches to the "%03d-%04d" display
                }
            }
            done = true;
        }
    }

    void Assign()
    {
        if (started.exchange(true))
            return;
        std::thread(AssignThread).detach();
    }
}
