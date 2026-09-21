#pragma once

namespace gate
{
    // True while the main-menu password gate has not been satisfied yet.
    bool Locked();

    // Per-frame update + draw at the main menu (called from the DrawMenu hook).
    void UpdateAndDraw();

    // Blocks the master-server connect until the gate is satisfied.
    bool ShouldBlockConnect();
}
