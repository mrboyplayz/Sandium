#pragma once

namespace radioplayer
{
    // Car radio playback: watches the server's radio flag
    // ("radio <serial> <file> <vehicleIndex>"), downloads the mp3 from the
    // master, and plays it positionally anchored to that vehicle -- volume
    // and stereo pan follow the listener camera, so the music comes FROM
    // the car. Call Update() once per frame. Windows only.
    void Update();
    void StopSession();
}
