#include "Gate.hpp"

#include "Addresses.hpp"
#include "api/Http.hpp"
#include "api/Logging.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// Main-menu password gate: while locked, the master-server connect is
// suppressed and the menu shows a password prompt instead of "Connecting...".
// Authentication uses a slow PBKDF2 verifier and an HMAC over a one-time
// challenge. The password and verifier are never downloaded from the VPS.

namespace gate
{
    namespace
    {
        constexpr const char *MASTER_HOST = "185.227.111.150";
        constexpr uint16_t MASTER_PORT = 80;
        constexpr const char *CHALLENGE_PATH = "/gate/challenge";
        constexpr const char *LOGIN_PATH = "/gate/login";

        enum class State
        {
            Typing,
            Checking,
            Open,
        };

        State state = State::Typing;
        std::string entered;
        std::string status = "Enter the password to unlock the game.";
        int statusFrames = 0;
        int attempts = 0;
        bool rememberChecked = false;
        std::string masterSession;

        std::string ExeDirectory()
        {
            char path[MAX_PATH] = {};
            const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
            if (length == 0 || length >= MAX_PATH)
                return ".";
            std::string result(path, length);
            const std::size_t slash = result.find_last_of("\\/");
            return slash == std::string::npos ? "." : result.substr(0, slash);
        }

        std::string LocalAppDataRememberPath()
        {
            const char *localAppData = std::getenv("LOCALAPPDATA");
            std::string base = localAppData && *localAppData ? localAppData : ".";
            std::string dir = base + "\\Sandium";
            CreateDirectoryA(dir.c_str(), nullptr);
            return dir + "\\gate_remember.txt";
        }

        std::string ExeRememberPath()
        {
            return ExeDirectory() + "\\sandium_gate_remember.txt";
        }

        std::array<std::string, 2> RememberPaths()
        {
            return {ExeRememberPath(), LocalAppDataRememberPath()};
        }

        std::time_t NowSeconds()
        {
            return std::time(nullptr);
        }

        void ForgetRememberedUnlock()
        {
            for (const auto &path : RememberPaths())
                DeleteFileA(path.c_str());
        }

        std::string BytesToHexString(const unsigned char *bytes, std::size_t size)
        {
            static const char digits[] = "0123456789abcdef";
            std::string result(size * 2, '0');
            for (std::size_t i = 0; i < size; ++i)
            {
                result[i * 2] = digits[bytes[i] >> 4];
                result[i * 2 + 1] = digits[bytes[i] & 15];
            }
            return result;
        }

        bool HexStringToBytes(const std::string &hex, std::vector<unsigned char> &bytes)
        {
            if (hex.size() % 2 != 0)
                return false;
            bytes.clear();
            bytes.reserve(hex.size() / 2);
            for (std::size_t i = 0; i < hex.size(); i += 2)
            {
                char pair[3] = {hex[i], hex[i + 1], 0};
                char *end = nullptr;
                unsigned long value = strtoul(pair, &end, 16);
                if (!end || *end != '\0')
                    return false;
                bytes.push_back(static_cast<unsigned char>(value));
            }
            return true;
        }

        std::string ProtectRememberPayload(long long expiry, const std::string &session)
        {
            const std::string payload = "sandium-gate-v3 " + std::to_string(expiry) + " " + session;
            DATA_BLOB input{};
            input.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(payload.data()));
            input.cbData = static_cast<DWORD>(payload.size());
            DATA_BLOB output{};
            if (!CryptProtectData(&input, L"Sandium gate remember", nullptr, nullptr, nullptr,
                                  CRYPTPROTECT_UI_FORBIDDEN, &output))
                return "";
            std::string hex = BytesToHexString(output.pbData, output.cbData);
            LocalFree(output.pbData);
            return hex;
        }

        bool UnprotectRememberPayload(const std::string &hex, long long &expiry,
                                      std::string &session)
        {
            std::vector<unsigned char> protectedBytes;
            if (!HexStringToBytes(hex, protectedBytes) || protectedBytes.empty())
                return false;
            DATA_BLOB input{};
            input.pbData = protectedBytes.data();
            input.cbData = static_cast<DWORD>(protectedBytes.size());
            DATA_BLOB output{};
            if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                                    CRYPTPROTECT_UI_FORBIDDEN, &output))
                return false;
            std::string payload(reinterpret_cast<char *>(output.pbData), output.cbData);
            LocalFree(output.pbData);
            std::string magic;
            std::istringstream stream(payload);
            return (stream >> magic >> expiry >> session) && magic == "sandium-gate-v3" &&
                   session.size() == 32;
        }

        bool LoadRememberedUnlock()
        {
            for (const auto &path : RememberPaths())
            {
                std::ifstream file(path, std::ios::in);
                std::string magic;
                long long expiry = 0;
                if (!(file >> magic))
                    continue;
                std::string rememberedSession;
                if (magic == "sandium-gate-v3")
                {
                    std::string protectedHex;
                    file >> protectedHex;
                    if (!UnprotectRememberPayload(protectedHex, expiry, rememberedSession))
                        continue;
                }
                else
                    continue;
                if (expiry <= static_cast<long long>(NowSeconds()))
                {
                    ForgetRememberedUnlock();
                    return false;
                }

                // A remember token is only valid while the same master process
                // is alive and its in-memory IP authorization is still active.
                // Any master restart produces a new session ID and forces the
                // password prompt on the client's next gate check.
                using Json = nlohmann::json;
                const std::string body = http::Get(MASTER_HOST, MASTER_PORT,
                                                   "/gate/status", 4000);
                Json remote = Json::parse(body, nullptr, false);
                if (remote.is_discarded() || !remote.value("authorized", false) ||
                    remote.value("session", "") != rememberedSession)
                {
                    ForgetRememberedUnlock();
                    api::GetSandiumLogger()->Log(
                        "Password gate discarded stale remembered unlock");
                    return false;
                }
                masterSession = rememberedSession;
                api::GetSandiumLogger()->Log("Password gate remembered unlock from {}", path);
                return true;
            }
            return false;
        }

        void SaveRememberedUnlock()
        {
            constexpr long long THREE_DAYS = 3LL * 24LL * 60LL * 60LL;
            const long long expiry = static_cast<long long>(NowSeconds()) + THREE_DAYS;
            const std::string protectedHex = ProtectRememberPayload(expiry, masterSession);
            bool wrote = false;
            for (const auto &path : RememberPaths())
            {
                std::ofstream file(path, std::ios::out | std::ios::trunc);
                if (file && !protectedHex.empty())
                {
                    file << "sandium-gate-v3 " << protectedHex << "\n";
                    wrote = true;
                    api::GetSandiumLogger()->Log("Password gate saved remembered unlock to {}", path);
                }
            }
            if (!wrote)
                api::GetSandiumLogger()->Log("<red>Password gate could not save remembered unlock");
        }

        void EnsureRememberChecked()
        {
            if (rememberChecked)
                return;
            rememberChecked = true;
            if (LoadRememberedUnlock())
            {
                state = State::Open;
                api::GetSandiumLogger()->Log("Password gate opened from remembered unlock");
            }
        }

        bool KeyPressed(int vk)
        {
            return (GetAsyncKeyState(vk) & 1) != 0;
        }

        std::string ClipboardText()
        {
            if (!OpenClipboard(nullptr))
                return "";
            HANDLE handle = GetClipboardData(CF_TEXT);
            std::string text;
            if (handle)
            {
                const char *data = (const char *)GlobalLock(handle);
                if (data)
                {
                    text = data;
                    GlobalUnlock(handle);
                }
            }
            CloseClipboard();
            return text;
        }

        void PollKeyboard()
        {
            // paste (Ctrl+V)
            if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && KeyPressed('V'))
            {
                std::string paste = ClipboardText();
                for (char c : paste)
                {
                    if (c >= 0x20 && c < 0x7F && entered.size() < 63)
                        entered.push_back(c);
                }
            }
            if (KeyPressed(VK_BACK))
            {
                if (!entered.empty())
                    entered.pop_back();
            }
            if (KeyPressed(VK_RETURN))
            {
                state = State::Checking;
                return;
            }
            if (KeyPressed(VK_ESCAPE))
            {
                entered.clear();
                return;
            }

            // printable ASCII via ToUnicode with the current keyboard state
            static BYTE lastKeys[256] = {};
            BYTE keys[256] = {};
            GetKeyboardState(keys);
            for (int vk = VK_SPACE; vk <= VK_OEM_8; ++vk)
            {
                bool downNow = (keys[vk] & 0x80) != 0;
                bool downBefore = (lastKeys[vk] & 0x80) != 0;
                if (!downNow || downBefore)
                    continue;
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
                    continue; // Ctrl+letter = shortcut, not typing
                WCHAR chars[4] = {};
                BYTE scan = (BYTE)MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
                int written = ToUnicode(vk, scan, keys, chars, 4, 0);
                for (int c = 0; c < written; ++c)
                {
                    if (chars[c] >= 0x20 && chars[c] < 0x7F && entered.size() < 63)
                        entered.push_back((char)chars[c]);
                }
            }
            memcpy(lastKeys, keys, sizeof(keys));
        }

        bool HexToBytes(const std::string &hex, std::vector<unsigned char> &bytes)
        {
            if (hex.size() % 2 != 0)
                return false;
            bytes.clear();
            bytes.reserve(hex.size() / 2);
            for (size_t i = 0; i < hex.size(); i += 2)
            {
                char *end = nullptr;
                unsigned long value = strtoul(hex.substr(i, 2).c_str(), &end, 16);
                if (!end || *end != '\0')
                    return false;
                bytes.push_back(static_cast<unsigned char>(value));
            }
            return true;
        }

        std::string BytesToHex(const unsigned char *bytes, size_t size)
        {
            static const char digits[] = "0123456789abcdef";
            std::string result(size * 2, '0');
            for (size_t i = 0; i < size; ++i)
            {
                result[i * 2] = digits[bytes[i] >> 4];
                result[i * 2 + 1] = digits[bytes[i] & 15];
            }
            return result;
        }

        bool MakeProof(const std::string &password, const std::vector<unsigned char> &salt,
                       unsigned long long iterations, const std::vector<unsigned char> &nonce,
                       std::string &proof)
        {
            BCRYPT_ALG_HANDLE algorithm = nullptr;
            BCRYPT_HASH_HANDLE hash = nullptr;
            std::array<unsigned char, 32> verifier{};
            std::array<unsigned char, 32> digest{};
            DWORD objectSize = 0, resultSize = 0;
            std::vector<unsigned char> hashObject;

            NTSTATUS result = BCryptOpenAlgorithmProvider(
                &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
            if (result >= 0)
                result = BCryptDeriveKeyPBKDF2(
                    algorithm,
                    reinterpret_cast<PUCHAR>(const_cast<char *>(password.data())),
                    static_cast<ULONG>(password.size()),
                    const_cast<PUCHAR>(salt.data()), static_cast<ULONG>(salt.size()),
                    iterations, verifier.data(), static_cast<ULONG>(verifier.size()), 0);
            if (result >= 0)
                result = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                           reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
                                           &resultSize, 0);
            if (result >= 0)
            {
                hashObject.resize(objectSize);
                result = BCryptCreateHash(algorithm, &hash, hashObject.data(), objectSize,
                                          verifier.data(), static_cast<ULONG>(verifier.size()), 0);
            }
            if (result >= 0)
                result = BCryptHashData(hash, const_cast<PUCHAR>(nonce.data()),
                                        static_cast<ULONG>(nonce.size()), 0);
            if (result >= 0)
                result = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);

            if (hash)
                BCryptDestroyHash(hash);
            if (algorithm)
                BCryptCloseAlgorithmProvider(algorithm, 0);
            SecureZeroMemory(verifier.data(), verifier.size());
            SecureZeroMemory(hashObject.data(), hashObject.size());

            if (result < 0)
                return false;
            proof = BytesToHex(digest.data(), digest.size());
            return true;
        }

        bool Verify(const std::string &password)
        {
            using Json = nlohmann::json;
            std::string body = http::Get(MASTER_HOST, MASTER_PORT, CHALLENGE_PATH, 4000);
            Json challenge = Json::parse(body, nullptr, false);
            if (challenge.is_discarded() || !challenge.contains("id") ||
                !challenge.contains("salt") || !challenge.contains("nonce") ||
                !challenge.contains("iterations") || !challenge.contains("session"))
            {
                status = "Cannot reach the password service. Try again.";
                statusFrames = 180;
                return false;
            }

            std::vector<unsigned char> salt, nonce;
            std::string id = challenge.value("id", "");
            const std::string challengeSession = challenge.value("session", "");
            if (id.size() != 32 || !HexToBytes(challenge.value("salt", ""), salt) ||
                !HexToBytes(challenge.value("nonce", ""), nonce) ||
                challengeSession.size() != 32)
            {
                status = "Password service returned an invalid challenge.";
                statusFrames = 180;
                return false;
            }

            std::string proof;
            if (!MakeProof(password, salt, challenge.value("iterations", 0ULL), nonce, proof))
            {
                status = "Could not calculate the password proof.";
                statusFrames = 180;
                return false;
            }

            Json request = {{"id", id}, {"proof", proof}};
            Json response = Json::parse(
                http::PostJson(MASTER_HOST, MASTER_PORT, LOGIN_PATH, request.dump(), 4000),
                nullptr, false);
            if (!response.is_discarded() && response.value("ok", false) &&
                response.value("session", "") == challengeSession)
            {
                masterSession = challengeSession;
                return true;
            }

            status = "Wrong password or too many attempts.";
            statusFrames = 180;
            return false;
        }

        void DrawTextCentered(const char *text, float y, float size, float r, float g, float b)
        {
            // Bit 0 is the game's actual horizontal-center flag. Bit 1 anchors
            // the text's right edge and pushed long lines to the screen edge.
            addresses::CSDrawTextFunc(text, 512.0f, y, size, 1, r, g, b, 1.0f);
        }
    }

    bool Locked()
    {
        EnsureRememberChecked();
        return state != State::Open;
    }

    bool ShouldBlockConnect()
    {
        EnsureRememberChecked();
        return state != State::Open;
    }

    void UpdateAndDraw()
    {
        EnsureRememberChecked();
        if (state == State::Open)
            return;

        if (state == State::Typing)
            PollKeyboard();

        if (state == State::Checking)
        {
            status = "Checking password...";
            if (Verify(entered))
            {
                state = State::Open;
                SaveRememberedUnlock();
                api::GetSandiumLogger()->Log("Password gate opened");
                return;
            }
            state = State::Typing;
            attempts++;
            entered.clear();
        }

        if (statusFrames > 0)
            statusFrames--;

        DrawTextCentered("Please input a secret password in order to play.", 250.0f, 28.0f, 1.0f, 1.0f, 1.0f);

        // masked input with a blinking cursor
        std::string masked;
        for (std::size_t i = 0; i < entered.size(); ++i)
            masked.push_back('*');
        masked.push_back(((GetTickCount() / 400) % 2) ? '_' : ' ');
        DrawTextCentered(masked.c_str(), 320.0f, 30.0f, 1.0f, 1.0f, 0.4f);

        float sr = 1.0f, sg = 1.0f, sb = 1.0f;
        if (attempts > 0)
        {
            sr = 1.0f; sg = 0.35f; sb = 0.35f;
        }
        DrawTextCentered(status.c_str(), 430.0f, 18.0f, sr, sg, sb);
    }
}
