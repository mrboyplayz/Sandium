#pragma once

namespace structs { struct Item; }

namespace custommodels
{
    void ConfigureBoomboxType();
    void ConfigureRevolverType();
    void ConfigureM9BayonetType();
    void UpdateRevolver();
    void UpdateM9Bayonet();
    void UpdateAndDraw();
    void Shutdown();
    bool IsBoomboxItem(const structs::Item &item);
}
