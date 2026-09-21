#include "ItemModels.hpp"

#include "Addresses.hpp"
#include "api/Logging.hpp"
#include "structs/ItemType.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace itemmodels
{
    namespace
    {
        std::array<int, structs::ItemType::ExpandedCount> modelResources = []
        {
            std::array<int, structs::ItemType::ExpandedCount> result{};
            result.fill(-1);
            return result;
        }();

        std::filesystem::path ResolveExistingFile(const std::string &path, const char *kind)
        {
            if (path.empty())
                throw std::invalid_argument(std::string(kind) + " path cannot be empty");
            const auto resolved = std::filesystem::weakly_canonical(std::filesystem::path(path));
            if (!std::filesystem::is_regular_file(resolved))
                throw std::runtime_error(std::string(kind) + " file does not exist: " + path);
            return resolved;
        }

    }

    // The loader only builds and uploads the GL texture when the mipmap
    // argument is nonzero (0x1400EDED0 gates sub_14009A370 on it); with it
    // clear the texture resource never gets a GL name and the model renders
    // untextured. Vanilla item textures always pass 1 with repeat wrap and
    // linear/linear-mipmap-nearest filtering, so mirror that exactly.
    void LoadSkin(int textureResourceID, const std::string &texturePath)
    {
        constexpr int GL_REPEAT_VALUE = 0x2901;
        constexpr int GL_LINEAR_VALUE = 0x2601;
        constexpr int GL_LINEAR_MIPMAP_NEAREST_VALUE = 0x2703;
        addresses::LoadTextureFunc(textureResourceID, texturePath.c_str(), 1,
                                   GL_REPEAT_VALUE, GL_REPEAT_VALUE,
                                   GL_LINEAR_VALUE, GL_LINEAR_MIPMAP_NEAREST_VALUE, 0);
    }

    bool Set(structs::ItemType &itemType, const std::string &cmoPath,
             const std::string &texturePath)
    {        if (*addresses::IsDedicated)
            throw std::runtime_error("Custom item models are not available in the dedicated server build");
        if (!addresses::LoadCMOFunc.ptr || !addresses::LoadTextureFunc.ptr ||
            !addresses::ModelResourceCount.ptr || !addresses::ModelTextureResources.ptr ||
            !addresses::ItemModelResources.ptr)
            throw std::runtime_error("Native model loader is unavailable");

        const int itemTypeID = itemType.customData.index;
        if (itemTypeID < 0 || itemTypeID >= static_cast<int>(structs::ItemType::ExpandedCount))
            throw std::out_of_range("Item type ID is outside the expanded table");

        const auto sourceCMO = ResolveExistingFile(cmoPath, "CMO");
        const auto sourceTexture = ResolveExistingFile(texturePath, "Texture");
        if (sourceCMO.extension() != ".cmo")
            throw std::invalid_argument("Custom item model must use the .cmo extension");
        // The native loader hardcodes data/model/<name>.cmo and uses strcpy
        // into a 64-byte buffer. Stage arbitrary addon paths under a short,
        // controlled name before entering it.
        const std::string nativeName = "sandium_item_" + std::to_string(itemTypeID);
        const auto stagedCMO = std::filesystem::path("data/model") / (nativeName + ".cmo");
        std::filesystem::copy_file(sourceCMO, stagedCMO,
                                   std::filesystem::copy_options::overwrite_existing);

        int modelResourceID = modelResources[static_cast<std::size_t>(itemTypeID)];
        bool allocated = false;
        if (modelResourceID < 0)
        {
            modelResourceID = *addresses::ModelResourceCount;
            if (modelResourceID < 0 || modelResourceID >= TextureMappedModelLimit)
                throw std::runtime_error("Native texture-mapped model resource pool is full");
            ++*addresses::ModelResourceCount;
            allocated = true;
        }

        const int textureResourceID = addresses::ModelTextureResources[modelResourceID];
        LoadSkin(textureResourceID, sourceTexture.string());
        // A nonnegative item type makes the original loader copy the mesh into
        // two fixed 64-entry arrays embedded in the 5,072-byte ItemType. That
        // legacy copy is unrelated to the renderer's much larger model pool
        // and corrupts the following collision fields for detailed meshes.
        // Standalone mode uploads the complete CMO while the cloned base type
        // continues to provide its stable native collision representation.
        if (!addresses::LoadCMOFunc(modelResourceID, -1, nativeName.c_str()))
        {
            if (allocated)
                --*addresses::ModelResourceCount;
            throw std::runtime_error("Sub Rosa rejected the CMO file: " + cmoPath);
        }

        modelResources[static_cast<std::size_t>(itemTypeID)] = modelResourceID;
        addresses::ItemModelResources[itemTypeID] = modelResourceID;

        // The vanilla item renderer only samples a per-type skin table for
        // type ids below 46 (cmp ecx,0x2D at subrosa+0x2248B) and only when a
        // hard-coded "textured type" bitmask allows it (mov r8, 0x200800002000
        // at +0x223FB and +0x22846). Types past 45 fall through to a black
        // default, so extend both gates to the expanded table and register the
        // skin resource, or the held item renders untextured black.
        const auto base = reinterpret_cast<std::uint8_t *>(addresses::Base.ptr);
        auto *perTypeSkin = reinterpret_cast<int *>(base + 0x43EBF580);
        perTypeSkin[itemTypeID] = textureResourceID;
        constexpr std::uintptr_t maskImmediateRvas[] = {0x223FD, 0x22848};
        for (const std::uintptr_t immediateRva : maskImmediateRvas)
        {
            std::uint64_t texturedTypeMask = 0;
            std::memcpy(&texturedTypeMask, base + immediateRva, sizeof(texturedTypeMask));
            texturedTypeMask |= 1ULL << itemTypeID;
            std::memcpy(base + immediateRva, &texturedTypeMask, sizeof(texturedTypeMask));
        }
        *reinterpret_cast<std::uint8_t *>(base + 0x2248D) = 0x3F; // cmp ecx, 0x2D -> 0x3F
        api::GetSandiumLogger()->Log(
            "Loaded custom model {} and texture {} for item type {} in native model resource {}",
            cmoPath, texturePath, itemTypeID, modelResourceID);
        return true;
    }

    bool SetTexture(structs::ItemType &itemType, const std::string &texturePath)
    {
        if (*addresses::IsDedicated)
            throw std::runtime_error("Custom item models are not available in the dedicated server build");
        if (!addresses::LoadTextureFunc.ptr || !addresses::ModelTextureResources.ptr)
            throw std::runtime_error("Native model loader is unavailable");

        const int itemTypeID = itemType.customData.index;
        if (itemTypeID < 0 || itemTypeID >= static_cast<int>(structs::ItemType::ExpandedCount))
            throw std::out_of_range("Item type ID is outside the expanded table");

        const int modelResourceID = modelResources[static_cast<std::size_t>(itemTypeID)];
        if (modelResourceID < 0)
            throw std::runtime_error("Item type has no custom model; call SetModel first");

        const int textureResourceID = addresses::ModelTextureResources[modelResourceID];
        LoadSkin(textureResourceID, ResolveExistingFile(texturePath, "Texture").string());
        api::GetSandiumLogger()->Log("Loaded custom texture {} for item type {} in native model resource {}",
                                     texturePath, itemTypeID, modelResourceID);
        return true;
    }
}
