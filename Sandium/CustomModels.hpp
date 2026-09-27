#pragma once

namespace structs { struct Item; }

namespace custommodels
{
    inline constexpr float FlashlightMeshScale = 0.7225f;
    inline constexpr float FlashlightForwardPitchDegrees = 0.0f;

    struct FlashlightRotation
    {
        float pitch = FlashlightForwardPitchDegrees;
        float yaw = 0.0f;
        float roll = 0.0f;
    };

    struct FlashlightGripOffset
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    FlashlightRotation GetFlashlightRotation();
    FlashlightGripOffset GetFlashlightGripOffset();

    void ConfigureBoomboxType();
    void ConfigureRevolverType();
    void ConfigureM9BayonetType();
    void ConfigureFlashlightType();
    void UpdateRevolver();
    void UpdateM9Bayonet();
    void UpdateFlashlight();
    void UpdateAndDraw();
    void Shutdown();
    bool IsBoomboxItem(const structs::Item &item);
}
