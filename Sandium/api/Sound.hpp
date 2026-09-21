#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace api
{
    // One-shot audio player (audio-only files: mp3/wav via Media Foundation).
    // Play() restarts from the beginning if already playing.
    class Sound
    {
    public:
        static std::shared_ptr<Sound> Load(const std::string &path);
        // Decode to 48 kHz mono PCM and register it with Sub Rosa's native
        // HRTF mixer. This is intended for world-positioned sources.
        static std::shared_ptr<Sound> Load3D(const std::string &path);
        static std::shared_ptr<Sound> Load3D(const std::string &path, unsigned int soundID,
                                             float referenceDistance = 1.0f);
        ~Sound();

        void Play(float volume = 1.0f);
        void Play3D(float x, float y, float z, float volume = 1.0f, bool loop = false);
        // Fire-and-forget native source. Unlike Play3D, this does not stop a
        // previous source using the same registered sample.
        void PlayOneShot3D(float x, float y, float z, float volume = 1.0f,
                           float pitch = 1.0f);
        void SetPosition(float x, float y, float z);
        void SetVolume(float volume);

        // Pause preserves the decoder and device position. Resume continues
        // from that point without rewinding the file.
        void Pause();
        void Resume();

        // Live stereo gains for positional audio (updated while playing).
        void SetGainLR(float left, float right);
        void Stop();

    private:
        Sound() = default;
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
