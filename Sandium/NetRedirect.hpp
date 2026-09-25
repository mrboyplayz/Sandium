#pragma once

namespace netredirect
{
    // Redirect the game's traffic to the dead-era hardcoded real master
    // (66.226.72.227, also what www.crypticsea.com resolves to) to our own
    // master, so clients only ever see our server list. Windows-only.
    void Install();
    // True only while this client is actively exchanging game packets with
    // the Noxus game server. Master-server browsing does not count.
    bool IsNoxusGame();
    void ResetGameSession();
}
