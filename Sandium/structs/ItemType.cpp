#include "ItemType.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace structs
{
    std::string ItemType::GetName() const
    {
        return std::string(this->name);
    }
    void ItemType::SetName(const std::string &right)
    {
        std::memset(this->name, 0, sizeof(this->name));
        right.copy(this->name, std::min(right.size(), sizeof(this->name) - 1));
    }

    int ItemType::GetIndex() const
    {
        return this->customData.index;
    }
    std::string ItemType::GetTypeID() const
    {
        return *this->customData.typeIDPtr;
    }
}
