#include "Gate.h"

#include <windows.h>
#include <wininet.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GATE_HOST "185.227.111.150"

static char gateEntered[128] = "";
static HWND gateEdit = NULL;
static HWND gateWindow = NULL;
static int gateSubmitted = 0;

static int JsonString(const char *json, const char *key, char *out, size_t outSize)
{
    char pattern[80];
    _snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *start = strstr(json, pattern);
    if (!start) return 0;
    start += strlen(pattern);
    const char *end = strchr(start, '"');
    size_t length = end ? (size_t)(end - start) : 0;
    if (!end || length + 1 > outSize) return 0;
    memcpy(out, start, length);
    out[length] = 0;
    return 1;
}

static int JsonInteger(const char *json, const char *key, unsigned long long *value)
{
    char pattern[80];
    _snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *start = strstr(json, pattern);
    if (!start) return 0;
    start += strlen(pattern);
    char *end = NULL;
    *value = _strtoui64(start, &end, 10);
    return end && end != start;
}

static int HexToBytes(const char *hex, unsigned char *out, size_t outSize)
{
    if (strlen(hex) != outSize * 2) return 0;
    for (size_t i = 0; i < outSize; ++i)
    {
        char pair[3] = {hex[i * 2], hex[i * 2 + 1], 0};
        char *end = NULL;
        unsigned long value = strtoul(pair, &end, 16);
        if (!end || *end) return 0;
        out[i] = (unsigned char)value;
    }
    return 1;
}

static void BytesToHex(const unsigned char *bytes, size_t size, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i)
    {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 15];
    }
    out[size * 2] = 0;
}

static int HttpRequest(const char *method, const char *path, const char *requestBody,
                       char *response, size_t responseSize)
{
    HINTERNET internet = InternetOpenA("SandiumLauncher", INTERNET_OPEN_TYPE_PRECONFIG,
                                      NULL, NULL, 0);
    if (!internet) return 0;
    HINTERNET connection = InternetConnectA(internet, GATE_HOST,
                                            INTERNET_DEFAULT_HTTP_PORT, NULL, NULL,
                                            INTERNET_SERVICE_HTTP, 0, 0);
    HINTERNET request = connection ? HttpOpenRequestA(
        connection, method, path, NULL, NULL, NULL,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0) : NULL;
    int ok = 0;
    if (request)
    {
        const char *headers = requestBody ? "Content-Type: application/json\r\n" : NULL;
        DWORD bodyLength = requestBody ? (DWORD)strlen(requestBody) : 0;
        if (HttpSendRequestA(request, headers, headers ? (DWORD)-1L : 0,
                             (LPVOID)requestBody, bodyLength))
        {
            DWORD received = 0, total = 0;
            while (total < responseSize - 1 &&
                   InternetReadFile(request, response + total,
                                    (DWORD)(responseSize - 1 - total), &received) && received)
                total += received;
            response[total] = 0;
            ok = total > 0;
        }
    }
    if (request) InternetCloseHandle(request);
    if (connection) InternetCloseHandle(connection);
    InternetCloseHandle(internet);
    return ok;
}

static int MakeProof(const char *password, const unsigned char salt[32],
                     unsigned long long iterations, const unsigned char nonce[32],
                     char proof[65])
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char verifier[32] = {0};
    unsigned char digest[32] = {0};
    PUCHAR hashObject = NULL;
    DWORD objectSize = 0, resultSize = 0;
    NTSTATUS result = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (result >= 0)
        result = BCryptDeriveKeyPBKDF2(
            algorithm, (PUCHAR)password, (ULONG)strlen(password), (PUCHAR)salt, 32,
            iterations, verifier, sizeof(verifier), 0);
    if (result >= 0)
        result = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                   (PUCHAR)&objectSize, sizeof(objectSize), &resultSize, 0);
    if (result >= 0)
    {
        hashObject = (PUCHAR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, objectSize);
        if (!hashObject) result = (NTSTATUS)-1;
    }
    if (result >= 0)
        result = BCryptCreateHash(algorithm, &hash, hashObject, objectSize,
                                  verifier, sizeof(verifier), 0);
    if (result >= 0) result = BCryptHashData(hash, (PUCHAR)nonce, 32, 0);
    if (result >= 0) result = BCryptFinishHash(hash, digest, sizeof(digest), 0);

    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    SecureZeroMemory(verifier, sizeof(verifier));
    if (hashObject)
    {
        SecureZeroMemory(hashObject, objectSize);
        HeapFree(GetProcessHeap(), 0, hashObject);
    }
    if (result < 0) return 0;
    BytesToHex(digest, sizeof(digest), proof);
    return 1;
}

static int Authenticate(const char *password)
{
    char challenge[2048], id[33], saltHex[65], nonceHex[65];
    unsigned long long iterations = 0;
    unsigned char salt[32], nonce[32];
    if (!HttpRequest("GET", "/gate/challenge", NULL, challenge, sizeof(challenge)) ||
        !JsonString(challenge, "id", id, sizeof(id)) ||
        !JsonString(challenge, "salt", saltHex, sizeof(saltHex)) ||
        !JsonString(challenge, "nonce", nonceHex, sizeof(nonceHex)) ||
        !JsonInteger(challenge, "iterations", &iterations) ||
        !HexToBytes(saltHex, salt, sizeof(salt)) ||
        !HexToBytes(nonceHex, nonce, sizeof(nonce)))
        return 0;

    char proof[65], requestBody[180], response[512];
    if (!MakeProof(password, salt, iterations, nonce, proof)) return 0;
    _snprintf(requestBody, sizeof(requestBody),
              "{\"id\":\"%s\",\"proof\":\"%s\"}", id, proof);
    return HttpRequest("POST", "/gate/login", requestBody, response, sizeof(response)) &&
           strstr(response, "\"ok\":true") != NULL;
}

static LRESULT CALLBACK GateProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
        case WM_CREATE:
            CreateWindowA("STATIC", "Please input a secret password in order to play.",
                          WS_CHILD | WS_VISIBLE, 14, 12, 350, 20,
                          window, NULL, NULL, NULL);
            gateEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                       WS_CHILD | WS_VISIBLE | ES_PASSWORD | ES_AUTOHSCROLL,
                                       14, 40, 260, 24, window, (HMENU)1, NULL, NULL);
            CreateWindowA("BUTTON", "Submit", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                          282, 40, 76, 24, window, (HMENU)2, NULL, NULL);
            CreateWindowA("STATIC", "Enter the password to unlock Sandium.",
                          WS_CHILD | WS_VISIBLE, 14, 72, 350, 18,
                          window, (HMENU)3, NULL, NULL);
            SetFocus(gateEdit);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == 2)
            {
                GetWindowTextA(gateEdit, gateEntered, sizeof(gateEntered));
                gateSubmitted = 1;
            }
            return 0;
        case WM_CLOSE:
            gateSubmitted = -1;
            return 0;
    }
    return DefWindowProcA(window, message, wParam, lParam);
}

int RunPasswordGate(void)
{
    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = GateProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "SandiumGate";
    RegisterClassA(&wc);

    gateWindow = CreateWindowExA(WS_EX_TOPMOST, "SandiumGate", "Sandium Password",
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 390, 140,
                                 NULL, NULL, wc.hInstance, NULL);
    if (!gateWindow) return 0;
    ShowWindow(gateWindow, SW_SHOW);
    SetForegroundWindow(gateWindow);

    for (;;)
    {
        MSG message;
        while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN)
            {
                GetWindowTextA(gateEdit, gateEntered, sizeof(gateEntered));
                gateSubmitted = 1;
            }
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        if (gateSubmitted == -1) return 0;
        if (gateSubmitted == 1)
        {
            gateSubmitted = 0;
            EnableWindow(GetDlgItem(gateWindow, 2), FALSE);
            SetWindowTextA(GetDlgItem(gateWindow, 3), "Checking password...");
            if (Authenticate(gateEntered))
            {
                SecureZeroMemory(gateEntered, sizeof(gateEntered));
                DestroyWindow(gateWindow);
                return 1;
            }
            SecureZeroMemory(gateEntered, sizeof(gateEntered));
            SetWindowTextA(gateEdit, "");
            SetWindowTextA(GetDlgItem(gateWindow, 3),
                           "Wrong password, service unavailable, or rate limited.");
            EnableWindow(GetDlgItem(gateWindow, 2), TRUE);
            SetFocus(gateEdit);
        }
        Sleep(15);
    }
}

