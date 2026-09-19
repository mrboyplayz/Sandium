#pragma once

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
        ~Sound();

        void Play(float volume = 1.0f);

    private:
        Sound() = default;
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
