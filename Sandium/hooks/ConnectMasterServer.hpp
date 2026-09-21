#pragma once

#include <subhook.h>

extern subhook::Hook *connectMasterServerHook;

void ConnectMasterServerHookFunc();

// I need stuff!!!!
#if _VSCODE
#define IMPLEMENT_HOOKS 1
#endif

#if IMPLEMENT_HOOKS

#include "../Addresses.hpp"
#include "../Gate.hpp"

subhook::Hook *connectMasterServerHook;

void ConnectMasterServerHookFunc()
{
    subhook::ScopedHookRemove scopedRemove(connectMasterServerHook);

    // main-menu password gate: no master connection until the gate opens
    if (gate::ShouldBlockConnect())
        return;

    addresses::ConnectMasterServerFunc(); // This might be used in the future
}

#endif