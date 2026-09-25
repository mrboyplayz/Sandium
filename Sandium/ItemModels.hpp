#pragma once

#include <string>

namespace structs { struct ItemType; }

namespace itemmodels
{
    inline constexpr int TextureMappedModelLimit = 1024;

    // Upload a texture through Sub Rosa's native resource loader. This is
    // shared by custom items and native character resources.
    void LoadSkin(int textureResourceID, const std::string &texturePath);

    // Load a CMO and PNG into the full native model pool, then bind that
    // resource through the renderer's item-type lookup. The base ItemType's
    // collision representation is intentionally retained. Paths are relative
    // to the Sub Rosa directory. Safe to call again to swap either asset.
    bool Set(structs::ItemType &itemType, const std::string &cmoPath,
             const std::string &texturePath);

    // Reload only the skin PNG for an item type that already has a custom
    // model (Set must have run first in this session).
    bool SetTexture(structs::ItemType &itemType, const std::string &texturePath);
}
