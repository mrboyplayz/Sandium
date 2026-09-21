#pragma once

#include <cstdint>
#include <subhook.h>

#if _WIN32
extern subhook::Hook *drawHUDHook;
#include "../api/Billboards.hpp"
#include "../BrokenBones.hpp"
#include "../PainShader.hpp"
#include "../CameraFX.hpp"
#include "../GrainShader.hpp"
#include "../Footsteps.hpp"
#include "../RadioPlayer.hpp"
#include "../CarRadioUI.hpp"

void DrawHUDHookFunc(std::int64_t a, std::int64_t b, int c, int d);
#endif

#if _VSCODE
#define IMPLEMENT_HOOKS 1
#endif

#if IMPLEMENT_HOOKS && _WIN32
#include "../Addresses.hpp"
#include "../BetterCrashes.hpp"
#include "../CustomModels.hpp"
#include "../LuaManager.hpp"
#include "../api/Image.hpp"
#include "../api/ItemBillboard.hpp"

subhook::Hook *drawHUDHook;

void DrawHUDHookFunc(std::int64_t a, std::int64_t b, int c, int d)
{
    subhook::ScopedHookRemove scopedRemove(drawHUDHook);
    painshader::Update();
    // Pain camera effects, shaders, and debug stats are disabled for now.
    radioplayer::Update();
    carradio::Update();
    bettercrashes::Update();
    brokenbones::Update();
    footsteps::Update();
    custommodels::UpdateRevolver();
    api::BeginLuaDrawing();
    GetMainLuaManager()->CallHooks("DrawHUD", "post");
    api::billboards::DrawFrame();
    carradio::Draw();
    api::EndLuaDrawing();
    api::FlushImageLayer(false);
    addresses::DrawHUDFunc(a, b, c, d);
    api::DrawItemBillboards();
    api::FlushImageLayer(true);
    api::FlushQueuedTexts(); // Lua UI text on top of the game's HUD
}
#endif
