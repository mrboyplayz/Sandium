#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace api
{
    class Image
    {
    public:
        Image(const Image &) = delete;
        Image &operator=(const Image &) = delete;
        ~Image();

        static std::shared_ptr<Image> Load(const std::string &path);
        void Draw(float x, float y, float width, float height,
                  float red = 1.0f, float green = 1.0f,
                  float blue = 1.0f, float alpha = 1.0f);
        void Delete();

        int Width() const { return _width; }
        int Height() const { return _height; }
        bool IsValid() const { return _texture != 0; }
        unsigned int TextureID() const { return _texture; }
        int Layer() const { return _layer; }
        void SetLayer(int layer) { _layer = layer; }
        float Rotation() const { return _rotation; }
        void SetRotation(float rotation) { _rotation = rotation; }

    private:
        Image(unsigned int texture, int width, int height);
        unsigned int _texture;
        int _width;
        int _height;
        int _layer = 0;
        float _rotation = 0.0f;
    };

    void InitializeImages();
    void ShutdownImages();
    void BeginLuaDrawing();
    void EndLuaDrawing();
    bool IsLuaDrawing();
    void FlushImageLayer(bool foreground);

    // Queues a texture for drawing with the image pass. Shared by images and videos.
    void QueueDraw(unsigned int texture, float x, float y, float width, float height,
                   float red, float green, float blue, float alpha, int layer, float rotation);

    // Resolves a path relative to the calling addon's folder; refuses paths that escape it.
    std::filesystem::path ResolveAddonMediaPath(const std::string &relativePath);
}
