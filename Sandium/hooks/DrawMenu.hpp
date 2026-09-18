#pragma once
#include "../DirectJoin.hpp"

#include <subhook.h>

extern subhook::Hook *drawMenuHook;
extern int drawMenuOptionsSection;

int DrawMenuHookFunc(int unk);

// I need stuff!!!!
#if _VSCODE
#define IMPLEMENT_HOOKS 1
#endif

#if IMPLEMENT_HOOKS

#include <fmt/format.h>
#include <glm/glm.hpp>

#if _WIN32
#include <Windows.h>
#elif __linux__
#include "../Hotfixes.hpp"
#endif

#include "../Addon.hpp"
#include "../Addresses.hpp"
#include "../api/Text.hpp"
#include "../structs/CSKeyboard.hpp"
#include "../Version.hpp"
#include "../Paperdoll.hpp"
#include "../LuaManager.hpp"
#include "../api/Image.hpp"
#include "../api/GLUniforms.hpp"
#include "../Phone.hpp"
#include "../ServerMedia.hpp"

subhook::Hook *drawMenuHook;

int DrawMenuHookFunc(int unk)
{
    subhook::ScopedHookRemove scopedRemove(drawMenuHook);

    static bool directJoinInited = false;
    if (!directJoinInited)
    {
        directjoin::Init((std::uintptr_t)addresses::Base.ptr);
        directJoinInited = true;
    }

    api::glcap::Prepare(); // first call happens with the GL context current

    api::BeginLuaDrawing();
    GetMainLuaManager()->CallHooks("DrawMenu", "post");
    api::EndLuaDrawing();
    api::FlushImageLayer(false);

    const auto finishMenu = [](int result)
    {
        api::FlushImageLayer(true);
        api::FlushQueuedTexts();
        return result;
    };

    servermedia::Tick();

    if (!*addresses::IsInGame && *addresses::MenuTypeID == 0)
    {
        // The main menu is being drawn!
        phone::Assign();
        api::DrawText(fmt::format("Sub Rosa 0.{}{}", SANDIUM_GAME_VERSION_NUMBER, SANDIUM_GAME_VERSION_PATCH), 512.0f, 128.0f, 24.0f, glm::vec4(1.0f));    
        api::DrawText(fmt::format("Sandium {}", SANDIUM_VERSION), 512.0f, 128.0f + 22.0f, 16.0f, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));    
        
        *addresses::NextMenuButtonPositionX = 16.0f;
        *addresses::NextMenuButtonPositionY = 464.0f + 26.0f;
        *addresses::NextMenuButtonSizeX = 192.0f;
        *addresses::NextMenuButtonSizeY = 24.0f;
        *addresses::NextMenuButtonKey = (SDL_Scancode)-1;
        if (addresses::DrawMenuButtonFunc("Addons"))
            *addresses::MenuTypeID = 100 + 0;

        // After the vanilla buttons: the Join IP text entries disturb the
        // game's shared widget-layout state, so anything drawn after them in
        // the same frame would be misplaced.
        directjoin::OnMainMenu();
    }
    else if (*addresses::MenuTypeID == 3)
    {
        // The options menu is being drawn!

        *addresses::NextMenuButtonPositionX = 4.0f;
        *addresses::NextMenuButtonPositionY = 48.0f;
        *addresses::NextMenuButtonSizeX = 85.0f;
        *addresses::NextMenuButtonSizeY = 24.0f;
        addresses::DrawMenuButtonSelectableFunc("General", addresses::MenuOptionsSectionID.ptr, 0);
        *addresses::NextMenuButtonPositionX = 4.0f + (85.0f * 1.0f) + 2.0f;
        *addresses::NextMenuButtonPositionY = 48.0f;
        *addresses::NextMenuButtonSizeX = 85.0f;
        *addresses::NextMenuButtonSizeY = 24.0f;
        addresses::DrawMenuButtonSelectableFunc("Video", addresses::MenuOptionsSectionID.ptr, 1);
        *addresses::NextMenuButtonPositionX = 4.0f + (85.0f * 2.0f) + 4.0f;
        *addresses::NextMenuButtonPositionY = 48.0f;
        *addresses::NextMenuButtonSizeX = 85.0f;
        *addresses::NextMenuButtonSizeY = 24.0f;
        addresses::DrawMenuButtonSelectableFunc("Audio", addresses::MenuOptionsSectionID.ptr, 2);
        *addresses::NextMenuButtonPositionX = 4.0f + (85.0f * 3.0f) + 6.0f;
        *addresses::NextMenuButtonPositionY = 48.0f;
        *addresses::NextMenuButtonSizeX = 85.0f;
        *addresses::NextMenuButtonSizeY = 24.0f;
        addresses::DrawMenuButtonSelectableFunc("Input", addresses::MenuOptionsSectionID.ptr, 3);

        *addresses::NextMenuButtonPositionX = 352.0f;
        *addresses::NextMenuButtonPositionY = 48.0f;
        *addresses::NextMenuButtonSizeX = 85.0f;
        *addresses::NextMenuButtonSizeY = 24.0f;
        *addresses::NextMenuButtonKey = static_cast<SDL_Scancode>(-1);
        addresses::DrawMenuButtonSelectableFunc("HUD", addresses::MenuOptionsSectionID.ptr, 4);
        if (*addresses::MenuOptionsSectionID == 4)
            paperdoll::DrawSettings();

        if (*addresses::MenuOptionsSectionID == 1)
        {
            static int enableHDWater = 1;
            *addresses::NextMenuButtonPositionX = 4.0f;
            *addresses::NextMenuButtonPositionY = 120.0f + (32.0f * 3.5f) - 4.0f;
            *addresses::NextMenuButtonSizeX = 240.0f;
            *addresses::NextMenuButtonSizeY = 32.0f;
            *addresses::NextMenuButtonKey = (SDL_Scancode)-1;
            if (addresses::DrawMenuToggleFunc("Enable HD water", &enableHDWater))
            {
#if _WIN32
                std::uintptr_t baseAddress = (std::uintptr_t)addresses::Base.ptr;
                DWORD oldProtect;
                VirtualProtect((LPVOID)(baseAddress + 0x957BF), 5, PAGE_EXECUTE_READWRITE, &oldProtect);
                if (enableHDWater)
                {
                    *(std::uint8_t *)(baseAddress + 0x957BF + 0) = 0xE8;
                    *(std::uint8_t *)(baseAddress + 0x957BF + 1) = 0x4C;
                    *(std::uint8_t *)(baseAddress + 0x957BF + 2) = 0x8A;
                    *(std::uint8_t *)(baseAddress + 0x957BF + 3) = 0x08;
                    *(std::uint8_t *)(baseAddress + 0x957BF + 4) = 0x00;
                }
                else
                {
                    *(std::uint8_t *)(baseAddress + 0x957BF + 0) = 0x90;
                    *(std::uint8_t *)(baseAddress + 0x957BF + 1) = 0x90;
                    *(std::uint8_t *)(baseAddress + 0x957BF + 2) = 0x90;
                    *(std::uint8_t *)(baseAddress + 0x957BF + 3) = 0x90;
                    *(std::uint8_t *)(baseAddress + 0x957BF + 4) = 0x90;
                }
                VirtualProtect((LPVOID)(baseAddress + 0x957BF), 5, oldProtect, &oldProtect);
#elif __linux__
                std::uintptr_t baseAddress = (std::uintptr_t)addresses::Base.ptr;
                int oldProtect;
                VirtualProtect((void *)(baseAddress + 0xC2B59), 5, PAGE_EXECUTE_READWRITE, &oldProtect);
                if (enableHDWater)
                {
                    *(std::uint8_t *)(baseAddress + 0xC2B59 + 0) = 0xE8;
                    *(std::uint8_t *)(baseAddress + 0xC2B59 + 1) = 0x46;
                    *(std::uint8_t *)(baseAddress + 0xC2B59 + 2) = 0xF2;
                    *(std::uint8_t *)(baseAddress + 0xC2B59 + 3) = 0xFF;
                    *(std::uint8_t *)(baseAddress + 0xC2B59 + 4) = 0xFF;
                }
                else
                {
                    *(std::uint8_t *)(baseAddress + 0xC2B59 + 0) = 0x90;
                    *(std::uint8_t *)(baseAddress + 0xC2B59 + 1) = 0x90;
                    *(std::uint8_t *)(baseAddress + 0xC2B59 + 2) = 0x90;
                    *(std::uint8_t *)(baseAddress + 0xC2B59 + 3) = 0x90;
                    *(std::uint8_t *)(baseAddress + 0xC2B59 + 4) = 0x90;
                }
                VirtualProtect((void *)(baseAddress + 0xC2B59), 5, oldProtect, &oldProtect);
#endif
            }
        }
    }
    else if (*addresses::MenuTypeID >= 100) // 100 is the base ID for sandium menus
    {
        int customMenuTypeID = *addresses::MenuTypeID - 100;
        if (customMenuTypeID == 0)
        {
            static int selectedAddonID = -1;

            *addresses::NextMenuButtonPositionX = 4.0f;
            *addresses::NextMenuButtonPositionY = 4.0f;
            *addresses::NextMenuButtonSizeX = 160.0f;
            *addresses::NextMenuButtonSizeY = 20.0f;
            *addresses::NextMenuButtonKey = SDL_SCANCODE_ESCAPE;
            if (addresses::DrawMenuButtonFunc("Back"))
            {
                *addresses::MenuTypeID = 0;
                selectedAddonID = -1;
            }
            
            {
                std::size_t addonCount = 0;
                for (auto it = GetAddons().begin(); it != GetAddons().end(); it++)
                {
                    *addresses::NextMenuButtonPositionX = 8.0f;
                    *addresses::NextMenuButtonPositionY = 128.0f + ((float)addonCount * (*addresses::NextMenuButtonSizeY + 2.0f));
                    *addresses::NextMenuButtonSizeX = 192.0f;
                    *addresses::NextMenuButtonSizeY = 48.0f;
                    *addresses::NextMenuButtonKey = (SDL_Scancode)-1;
                    addresses::DrawMenuButtonSelectableFunc("", &selectedAddonID, (int)addonCount);

                    addonCount++;
                }
            }

            // buttons must be registered before DrawMenuFunc processes them
            if (selectedAddonID >= 0 && !GetAddons()[selectedAddonID]->IsBuiltin())
            {
                const auto &selectedAddon = GetAddons()[selectedAddonID];
                *addresses::NextMenuButtonPositionX = 256.0f;
                *addresses::NextMenuButtonPositionY = 128.0f + 72.0f;
                *addresses::NextMenuButtonSizeX = 120.0f;
                *addresses::NextMenuButtonSizeY = 24.0f;
                *addresses::NextMenuButtonKey = (SDL_Scancode)-1;
                if (addresses::DrawMenuButtonFunc(selectedAddon->IsDisabled() ? "Enable" : "Disable"))
                    selectedAddon->SetDisabled(!selectedAddon->IsDisabled());
            }

            int returnValue = addresses::DrawMenuFunc(unk);

            {
                api::DrawText("Addons:", 10.0f, 128.0f - 24.0f, 22.0f, glm::vec4(1.0f), api::TextAlignment::Right);

                std::size_t addonCount = 0;
                for (auto it = GetAddons().begin(); it != GetAddons().end(); it++)
                {
                    const std::size_t maxNumberOfDisplayChars = 15; // must be bigger than 3
                    std::string name = (*it)->GetName();
                    if (name.size() > maxNumberOfDisplayChars)
                    {
                        name.erase(name.begin() + (maxNumberOfDisplayChars - 3), name.end());
                        name.push_back('.');
                        name.push_back('.');
                        name.push_back('.');
                    }

                    *addresses::NextMenuButtonPositionX = 8.0f;
                    *addresses::NextMenuButtonPositionY = 128.0f + ((float)addonCount * (*addresses::NextMenuButtonSizeY + 2.0f));
                    *addresses::NextMenuButtonSizeX = 192.0f;
                    *addresses::NextMenuButtonSizeY = 48.0f;
                    *addresses::NextMenuButtonKey = (SDL_Scancode)-1;
                    api::DrawText(name.c_str(), *addresses::NextMenuButtonPositionX + (*addresses::NextMenuButtonSizeX * 0.35f), *addresses::NextMenuButtonPositionY + ((*addresses::NextMenuButtonSizeY * 0.5f) - 8.0f), 16.0f, glm::vec4(1.0f), api::TextAlignment::Right);

                    addonCount++;
                }
            }

            if (selectedAddonID >= 0)
            {
                const auto &selectedAddon = GetAddons()[selectedAddonID];

                api::DrawText(fmt::format("{} ({})", selectedAddon->GetName(), selectedAddon->ID()), 256.0f, 128.0f, 22.0f, glm::vec4(1.0f), api::TextAlignment::Right);
                api::DrawText(selectedAddon->GetDescription(), 256.0f, 128.0f + 24.0f, 22.0f, glm::vec4(1.0f), api::TextAlignment::Right);
            }

            return finishMenu(returnValue);
        }
    }

    directjoin::DrawMenu();
    return finishMenu(addresses::DrawMenuFunc(unk));
}

#endif
