#include "Http.hpp"

#if _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#endif

namespace http
{
    namespace
    {
    std::string Request(const char *method, const std::string &hostIn, uint16_t port,
                        const std::string &path, const std::string &requestBody,
                        int timeoutMs, const char *envOverride)
    {
    #if _WIN32
        std::string host = hostIn;
        if (envOverride)
        {
            char envHost[256];
            if (GetEnvironmentVariableA(envOverride, envHost, sizeof(envHost)) > 0)
                host = envHost;
        }

        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return "";

        struct addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = nullptr;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
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
        DWORD timeout = timeoutMs;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));

        std::string body;
        if (connect(sock, res->ai_addr, (int)res->ai_addrlen) == 0)
        {
            std::string req = std::string(method) + " " + path + " HTTP/1.1\r\nHost: " + host +
                              "\r\nConnection: close\r\n";
            if (!requestBody.empty())
                req += "Content-Type: application/json\r\nContent-Length: " +
                       std::to_string(requestBody.size()) + "\r\n";
            req += "\r\n" + requestBody;
            send(sock, req.c_str(), (int)req.size(), 0);

            char buf[8192];
            int n;
            std::string raw;
            while ((n = recv(sock, buf, sizeof(buf), 0)) > 0)
                raw.append(buf, n);

            const size_t sep = raw.find("\r\n\r\n");
            if (sep != std::string::npos)
                body = raw.substr(sep + 4);
        }
        closesocket(sock);
        freeaddrinfo(res);
        WSACleanup();
        return body;
    #else
        (void)method; (void)hostIn; (void)port; (void)path; (void)requestBody;
        (void)timeoutMs; (void)envOverride;
        return "";
    #endif
    }
    }

    std::string Get(const std::string &host, uint16_t port, const std::string &path,
                    int timeoutMs, const char *envOverride)
    {
        return Request("GET", host, port, path, "", timeoutMs, envOverride);
    }

    std::string PostJson(const std::string &host, uint16_t port, const std::string &path,
                         const std::string &jsonBody, int timeoutMs,
                         const char *envOverride)
    {
        return Request("POST", host, port, path, jsonBody, timeoutMs, envOverride);
    }
}
