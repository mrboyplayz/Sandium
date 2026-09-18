#pragma once

#include <memory>
#include <string>

namespace api
{
    class Video
    {
    public:
        Video(const Video &) = delete;
        Video &operator=(const Video &) = delete;
        ~Video();

        static std::shared_ptr<Video> Load(const std::string &path);

        void Play();
        void Pause();
        void Stop();
        void Draw(float x, float y, float width, float height,
                  float red = 1.0f, float green = 1.0f,
                  float blue = 1.0f, float alpha = 1.0f);
        void Delete();

        int Width() const;
        int Height() const;
        bool IsValid() const;
        bool IsPlaying() const;
        double Duration() const;
        double Position() const;
        void SetPosition(double seconds);
        int Layer() const;
        void SetLayer(int layer);
        float Rotation() const;
        void SetRotation(float rotation);

    private:
        Video();
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

    void InitializeVideos();
    void ShutdownVideos();
}
