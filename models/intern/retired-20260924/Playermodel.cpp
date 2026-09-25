#include "Playermodel.hpp"

#include "Addresses.hpp"
#include "api/Logging.hpp"
#include "ItemModels.hpp"

#include <glad/glad.h>
#include <subhook.h>

#include <cstdint>
#include <exception>
#include <stdexcept>

namespace playermodel
{
    namespace
    {
        constexpr int InternHead = 5;
        constexpr int InternHair = 15;
        constexpr int InternBody = 3;

        using DrawHumanBodyFn = double (*)(int humanID, int renderResourceID);
        using LoadCharacterFn = std::int64_t (*)(int slot, const char *name);

        DrawHumanBodyFn drawHumanBody = nullptr;
        subhook::Hook *drawHumanBodyHook = nullptr;
        bool assetsAttempted = false;
        bool assetsReady = false;
        bool selectionInitialized = false;
        int internEnabled = 0;
        int internTextureResource = -1;
        int characterTextureUnit = -1;

        int ResolveCharacterTextureUnit()
        {
            if (characterTextureUnit >= 0) return characterTextureUnit;
            const auto base = reinterpret_cast<std::uintptr_t>(addresses::Base.ptr);
            const GLuint program = *reinterpret_cast<GLuint *>(base + 0x115AD380);
            if (!program || !glIsProgram(program)) return 0;
            GLint count = 0;
            glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
            for (GLint index = 0; index < count; ++index)
            {
                char name[256]{};
                GLsizei length = 0;
                GLint size = 0;
                GLenum type = 0;
                glGetActiveUniform(program, static_cast<GLuint>(index), sizeof(name), &length, &size, &type, name);
                if (type != GL_SAMPLER_2D && type != GL_SAMPLER_2D_SHADOW) continue;
                const GLint location = glGetUniformLocation(program, name);
                if (location < 0) continue;
                glGetUniformiv(program, location, &characterTextureUnit);
                api::GetSandiumLogger()->Log("Intern native skin sampler {} uses texture unit {}",
                                             name, characterTextureUnit);
                return characterTextureUnit;
            }
            characterTextureUnit = 0;
            return characterTextureUnit;
        }

        int *ProfileGender()
        {
            return reinterpret_cast<int *>(reinterpret_cast<std::uintptr_t>(addresses::Base.ptr) + 0x11E461E8);
        }

        int *ProfileHead()
        {
            return reinterpret_cast<int *>(reinterpret_cast<std::uintptr_t>(addresses::Base.ptr) + 0x11E461EC);
        }

        int *ProfileHair()
        {
            return reinterpret_cast<int *>(reinterpret_cast<std::uintptr_t>(addresses::Base.ptr) + 0x11E461F8);
        }

        void ApplySelection()
        {
            if (internEnabled)
            {
                *ProfileGender() = 1;
                *ProfileHead() = InternHead;
                *ProfileHair() = InternHair;
            }
            else if (*ProfileHead() == InternHead)
            {
                *ProfileHead() = 0;
                *ProfileHair() = 0;
            }
        }

        double DrawHumanBodyHook(int humanID, int renderResourceID)
        {
            subhook::ScopedHookRemove remove(drawHumanBodyHook);
            if (humanID < 0 || humanID >= static_cast<int>(structs::Human::VanillaCount))
                return drawHumanBody(humanID, renderResourceID);

            auto &human = addresses::Humans[humanID];
            const int originalModel = human.modelID;
            if (human.headModelID != InternHead)
                return drawHumanBody(humanID, renderResourceID);

            human.modelID = InternBody;
            GLint oldActiveTexture = 0, oldTexture = 0;
            glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
            glActiveTexture(GL_TEXTURE0 + ResolveCharacterTextureUnit());
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
            if (internTextureResource >= 0)
                glBindTexture(GL_TEXTURE_2D, addresses::CSTextures[internTextureResource].glTexture);
            const double result = drawHumanBody(humanID, renderResourceID);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture));
            glActiveTexture(static_cast<GLenum>(oldActiveTexture));
            human.modelID = originalModel;
            return result;
        }
    }

    void Install()
    {
#if _WIN32
        drawHumanBody = reinterpret_cast<DrawHumanBodyFn>(
            reinterpret_cast<std::uintptr_t>(addresses::Base.ptr) + 0x22DF0);
        drawHumanBodyHook = new subhook::Hook(
            reinterpret_cast<void *>(drawHumanBody), reinterpret_cast<void *>(&DrawHumanBodyHook),
            subhook::HookFlag64BitOffset);
        drawHumanBodyHook->Install();
#endif
    }

    void InitializeAssets()
    {
#if _WIN32
        if (assetsAttempted)
            return;
        assetsAttempted = true;
        try
        {
            const auto base = reinterpret_cast<std::uintptr_t>(addresses::Base.ptr);
            const auto loadCharacter = reinterpret_cast<LoadCharacterFn>(base + 0xE8580);
            auto *headResources = reinterpret_cast<int *>(base + 0x43EBC244);
            auto *hairResources = reinterpret_cast<int *>(base + 0x43EBC2C4);

            if (!addresses::ModelResourceCount.ptr || !addresses::ModelTextureResources.ptr)
                throw std::runtime_error("Native texture resource pool is unavailable");
            const int textureModelResource = (*addresses::ModelResourceCount)++;
            if (textureModelResource < 0 || textureModelResource >= itemmodels::TextureMappedModelLimit)
                throw std::runtime_error("Native texture resource pool is full");
            internTextureResource = addresses::ModelTextureResources[textureModelResource];
            itemmodels::LoadSkin(internTextureResource, "sandium/models/intern/intern.png");

            if (!loadCharacter(InternBody, "intern_body") ||
                !loadCharacter(8 + InternBody, "intern_body"))
                throw std::runtime_error("Sub Rosa rejected intern_body.cmc");
            if (!addresses::LoadCMOFunc(headResources[InternHead], -1, "intern_head") ||
                !addresses::LoadCMOFunc(headResources[16 + InternHead], -1, "intern_head"))
                throw std::runtime_error("Sub Rosa rejected intern_head.cmo");
            if (!addresses::LoadCMOFunc(hairResources[InternHair], -1, "intern_empty_hair") ||
                !addresses::LoadCMOFunc(hairResources[16 + InternHair], -1, "intern_empty_hair"))
                throw std::runtime_error("Sub Rosa rejected intern_empty_hair.cmo");

            assetsReady = true;
            api::GetSandiumLogger()->Log(
                "Loaded Intern playermodel into body slots 3/11 and head slots 5/21");
        }
        catch (const std::exception &error)
        {
            api::GetSandiumLogger()->Log("<red>Intern playermodel disabled: {}", error.what());
        }
#endif
    }

    void DrawAppearanceOption()
    {
#if _WIN32
        if (!selectionInitialized)
        {
            internEnabled = (*ProfileHead() == InternHead) ? 1 : 0;
            selectionInitialized = true;
        }
        if (!assetsReady || *addresses::IsInGame || *addresses::MenuTypeID != 5)
            return;

        *addresses::NextMenuButtonPositionX = 640.0f;
        *addresses::NextMenuButtonPositionY = 528.0f;
        *addresses::NextMenuButtonSizeX = 240.0f;
        *addresses::NextMenuButtonSizeY = 32.0f;
        *addresses::NextMenuButtonKey = static_cast<SDL_Scancode>(-1);
        if (addresses::DrawMenuToggleFunc("Intern Playermodel", &internEnabled))
            ApplySelection();
#endif
    }

    void AfterMenuDraw()
    {
#if _WIN32
        // The stock Head/Hair sliders only know their vanilla maxima and clamp
        // the reserved marker values while the appearance page is open.
        if (internEnabled)
            ApplySelection();
#endif
    }
}
