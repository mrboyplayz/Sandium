#include "DirectJoin.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Addresses.hpp"
#include "api/Text.hpp"

#if _WIN32
#include <Windows.h>
#endif

#if _WIN32
#undef DrawText
#endif

// Direct-join entry for the server browser.
//
// The vanilla Join Game browser only lists servers learned from the master.
// This draws a list-style row ("Sandium Server" + players/ping columns) at the
// top of the browser that performs the exact same memory writes as clicking a
// vanilla server entry, then forces the master/K gate open. The client's own
// state machine + the VPS master K-reply (7DFP + 'K' + u32) take it from there.

namespace directjoin
{
    namespace
    {
        // dword_140404A6C array indices (from client decomp).
        constexpr uint32_t IDX_GAME_IP = 456202714;
        constexpr uint32_t IDX_GAME_PORT = 456202715;
        constexpr uint32_t IDX_GAME_IP2 = 456202719;
        constexpr uint32_t IDX_GAME_PORT2 = 456202720;
        constexpr uint32_t IDX_MENU_STATE = 283831310; // 1 = server browser, 2 = connecting
        constexpr uint32_t IDX_PW_OK = 179662132; // 1 = password ok, 2 = master authorized join
        constexpr uint32_t IDX_SELECTED = 179662129;
        constexpr uint32_t IDX_AUTH_MISC = 284023066;
        constexpr uint32_t IDX_PW_FLAG = 179662130;
        constexpr uint32_t IDX_PW_BUF = 179662133; // 64 bytes zeroed by vanilla click
        constexpr uint32_t IDX_AUTH_STATE = 179668277; // 0 = steam, 1 = CS auth, 2 = done
        constexpr uint32_t IDX_K_GATE = 179662131; // 1 = K join-requests allowed
        constexpr uint32_t IDX_MASTER_IP = 280649181;
        constexpr uint32_t IDX_MASTER_PORT = 280649182;
        constexpr uint32_t IDX_SERVER_COUNT = 179662117;
        constexpr uint32_t IDX_TICKET_FLAG = 278384987;
        constexpr uint32_t IDX_TICKET_LEN = 278384730;

        constexpr uint32_t TEST_IP = (185u << 24) | (227u << 16) | (111u << 8) | 150u;
        constexpr uint16_t TEST_PORT = 27584;
        constexpr uint16_t MASTER_PORT = 28015;

        inline uint32_t *StatePtr(uint32_t index)
        {
            std::uintptr_t base = (std::uintptr_t)addresses::Base.ptr;
            if (!base)
                return nullptr;
            return reinterpret_cast<uint32_t *>(base + 0x404A6C + 4ULL * index);
        }

        inline uint32_t StateGet(uint32_t index, uint32_t fallback = 0)
        {
            uint32_t *p = StatePtr(index);
            return p ? *p : fallback;
        }

        bool IsBrowserOpen()
        {
            if (!addresses::IsInGame.ptr)
                return false;
            const bool inGame = *addresses::IsInGame;
            if (inGame)
                return false;
            return StateGet(IDX_MENU_STATE, 99) == 1;
        }

        void JoinServer(uint32_t ip, uint16_t port);

        void JoinTestingServer()
        {
            JoinServer(TEST_IP, TEST_PORT);
        }

        void JoinServer(uint32_t ip, uint16_t port)
        {
            uint32_t *gameIP = StatePtr(IDX_GAME_IP);
            uint32_t *gamePort = StatePtr(IDX_GAME_PORT);
            uint32_t *gameIP2 = StatePtr(IDX_GAME_IP2);
            uint32_t *gamePort2 = StatePtr(IDX_GAME_PORT2);
            uint32_t *masterIP = StatePtr(IDX_MASTER_IP);
            uint32_t *masterPort = StatePtr(IDX_MASTER_PORT);
            uint32_t *kGate = StatePtr(IDX_K_GATE);
            uint32_t *pwFlag = StatePtr(IDX_PW_FLAG);
            uint32_t *pwBuf = StatePtr(IDX_PW_BUF);
            uint32_t *pwOk = StatePtr(IDX_PW_OK);
            uint32_t *selected = StatePtr(IDX_SELECTED);
            uint32_t *authMisc = StatePtr(IDX_AUTH_MISC);
            uint32_t *menuState = StatePtr(IDX_MENU_STATE);
            if (!gameIP || !gamePort || !gameIP2 || !gamePort2 || !pwOk || !menuState)
                return;

            *gameIP = ip;
            *reinterpret_cast<uint16_t *>(gamePort) = port;
            *gameIP2 = ip;
            *reinterpret_cast<uint16_t *>(gamePort2) = port;
            // Force the master/K gate open regardless of H/I history.
            if (masterIP)
                *masterIP = TEST_IP;
            if (masterPort)
                *reinterpret_cast<uint16_t *>(masterPort) = MASTER_PORT;
            if (kGate)
                *kGate = 1;
            if (pwFlag)
                *pwFlag = 0;
            if (pwBuf)
                std::memset(pwBuf, 0, 64);
            if (selected)
                *selected = 0;
            if (authMisc)
                *authMisc = 0;
            *pwOk = 1;
            *menuState = 2;
        }

        void DrawJoinMenuImpl()
        {
        }

        void OnMainMenuImpl()
        {
            // Join IP panel removed 2026-09-18: the join now works end-to-end
            // through the server browser entry below.
        }

        void InitImpl(std::uintptr_t /*baseAddress*/)
        {
        }
    } // namespace
} // namespace directjoin

namespace directjoin
{
    void DrawMenu()
    {
        // Custom row removed 2026-09-18: the vanilla browser entry (fed by
        // the master list + our server's own ping reply) handles joining.
    }

    void OnMainMenu()
    {
        OnMainMenuImpl();
    }

    void Init(std::uintptr_t baseAddress)
    {
        InitImpl(baseAddress);
    }
}
