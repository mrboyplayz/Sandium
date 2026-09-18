#include "Hooks.hpp"

#include <subhook.h>
#include <utility>
#include <vector>

#include "Addresses.hpp"
#include "Paperdoll.hpp"
#include "api/GLUniforms.hpp"

#define IMPLEMENT_HOOKS 1
#include "hooks/CSDrawAtlasEntry.hpp"
#include "hooks/CSDrawText.hpp"
#include "hooks/ClientMain.hpp"
#include "hooks/ConnectMasterServer.hpp"
#include "hooks/CreateItem.hpp"
#include "hooks/CreateVehicle.hpp"
#include "hooks/DrawMenu.hpp"
#include "hooks/DrawHUD.hpp"
#include "hooks/DrawMenuButton.hpp"
#include "hooks/DrawMenuButtonSelectable.hpp"
#include "hooks/DrawMenuList.hpp"
#include "hooks/DrawMenuSlider.hpp"
#include "hooks/DrawMenuTextBox.hpp"
#include "hooks/DrawMenuToggle.hpp"
#include "hooks/Printf.hpp"
#include "hooks/ResetGame.hpp"
#include "hooks/ServerMain.hpp"
#include "hooks/SetupItemTypes.hpp"
#include "hooks/SetupVehicleTypes.hpp"
#include "NetRedirect.hpp"

// FuncAddress, hook function and hook object
struct HookEntry 
{
    void** sourceFunction;
    void* hookedFunction;
    subhook::Hook** hook;
};

void InstallHooks()
{
    // Must run before the renderer resolves its GL entry points, so the
    // camera-uniform wrappers get handed out in time.
    api::glcap::Install();

    static std::vector<HookEntry> hookEntries =
    {
        {(void **)&addresses::ConnectMasterServerFunc.ptr, (void *)&ConnectMasterServerHookFunc, &connectMasterServerHook},
        {(void **)&addresses::CreateItemFunc.ptr, (void *)&CreateItemHookFunc, &createItemHook},
        {(void **)&addresses::CreateVehicleFunc.ptr, (void *)&CreateVehicleHookFunc, &createVehicleHook},
        {(void **)&addresses::CSDrawAtlasEntryFunc.ptr, (void *)&CSDrawAtlasEntryHookFunc, &csDrawAtlasEntryHook},
        {(void **)&addresses::CSDrawTextFunc.ptr, (void *)&CSDrawTextHookFunc, &csDrawTextHook},
        {(void **)&addresses::ClientMainFunc.ptr, (void *)&ClientMainHookFunc, &clientMainHook},
#if _WIN32
        {(void **)&addresses::DrawHUDFunc.ptr, (void *)&DrawHUDHookFunc, &drawHUDHook},
#endif
        {(void **)&addresses::DrawMenuFunc.ptr, (void *)&DrawMenuHookFunc, &drawMenuHook},
        {(void **)&addresses::DrawMenuButtonFunc.ptr, (void *)&DrawMenuButtonHookFunc, &drawMenuButtonHook},
        {(void **)&addresses::DrawMenuButtonSelectableFunc.ptr, (void *)&DrawMenuButtonSelectableHookFunc, &drawMenuButtonSelectableHook},
        {(void **)&addresses::DrawMenuListFunc.ptr, (void *)&DrawMenuListHookFunc, &drawMenuListHook},
        {(void **)&addresses::DrawMenuSliderFunc.ptr, (void *)&DrawMenuSliderHookFunc, &drawMenuSliderHook},
        {(void **)&addresses::DrawMenuTextBoxFunc.ptr, (void *)&DrawMenuTextBoxHookFunc, &drawMenuTextBoxHook},
        {(void **)&addresses::DrawMenuToggleFunc.ptr, (void *)&DrawMenuToggleHookFunc, &drawMenuToggleHook},
        {(void **)&addresses::ResetGameFunc.ptr, (void *)&ResetGameHookFunc, &resetGameHook},
        {(void **)&addresses::PrintfFunc.ptr, (void *)&PrintfHookFunc, &printfHook},
        {(void **)&addresses::ServerMainFunc.ptr, (void *)&ServerMainHookFunc, &serverMainHook},
        {(void **)&addresses::SetupItemTypesFunc.ptr, (void *)&SetupItemTypesHookFunc, &setupItemTypesHook},
        {(void **)&addresses::SetupVehicleTypesFunc.ptr, (void *)&SetupVehicleTypesHookFunc, &setupVehicleTypesHook}
    };

    for (HookEntry entry : hookEntries) 
    {
        if (!*entry.sourceFunction) // Some versions might not contain one or two hooks
          continue ;

        subhook::Hook *hook = new subhook::Hook(*entry.sourceFunction, entry.hookedFunction, subhook::HookFlag64BitOffset);
        hook->Install();

        *entry.hook = hook;
    }

#if _WIN32
    // Route any traffic aimed at the (alive) real master IP to our master
    // so the browser only ever lists our servers.
    netredirect::Install();
#endif
    paperdoll::Install();
}
