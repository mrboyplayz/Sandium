#pragma once

#include <cstdint>
#include <subhook.h>

#if _WIN32
extern subhook::Hook *drawHUDHook;
void DrawHUDHookFunc(std::int64_t a, std::int64_t b, int c, int d);
#endif

#if _VSCODE
#define IMPLEMENT_HOOKS 1
#endif

#if IMPLEMENT_HOOKS && _WIN32
#include "../Addresses.hpp"
#include "../BetterCrashes.hpp"
#include "../CustomModels.hpp"
#include "../api/Freecam.hpp"
#include "../LuaManager.hpp"
#include "../api/Image.hpp"
#include "../api/ItemBillboard.hpp"

subhook::Hook *drawHUDHook;

void DrawHUDHookFunc(std::int64_t a, std::int64_t b, int c, int d)
{
    subhook::ScopedHookRemove scopedRemove(drawHUDHook);
    api::freecam::Update();
    bettercrashes::Update();
    custommodels::UpdateAndDraw();
    api::BeginLuaDrawing();
    GetMainLuaManager()->CallHooks("DrawHUD", "post");
    api::EndLuaDrawing();
    api::FlushImageLayer(false);
    addresses::DrawHUDFunc(a, b, c, d);
    api::DrawItemBillboards();
    api::FlushImageLayer(true);
}
#endif
