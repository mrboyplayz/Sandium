#pragma once

namespace structs { struct Item; }

namespace custommodels
{
    void ConfigureBoomboxType();
    void UpdateAndDraw();
    void Shutdown();
    bool IsBoomboxItem(const structs::Item &item);
}
