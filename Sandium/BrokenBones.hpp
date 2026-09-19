#pragma once

namespace brokenbones
{
    // Watches every human's limb health; a big single-tick drop means a hard
    // impact (fall, car crash): the limb snaps to 0 (vanilla renders it
    // black) and the bone-crack sound plays. Works in practice mode (local
    // damage) and multiplayer (server breaks confirm through the same sync).
    // Call every frame from the HUD hook. Windows only.
    void Update();

    // Number of broken limbs on the local human (0..6).
    int BrokenCount();
}
