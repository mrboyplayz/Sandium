#pragma once

namespace carradio
{
    // In-car radio UI, drawn above the gear selector while sitting in a
    // vehicle. Y toggles a free cursor; click the box beside "youtube link"
    // to focus it (paste with Ctrl+V or type), Enter submits the link to the
    // server, which downloads and broadcasts it to every client.
    void Update();
    void Draw();
}
