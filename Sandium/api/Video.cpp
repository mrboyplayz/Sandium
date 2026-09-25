#include "Video.hpp"

#if _WIN32
// moonjit's luaconf.h pins WINVER to 0x0501 (XP), which hides the Windows 8
// Source Reader API used here; set a modern WINVER before any Windows header
// or Lua header can load first.
#ifdef WINVER
#undef WINVER
#endif
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include <Windows.h>
#include <audioclient.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#undef DrawText
#endif

#include "Image.hpp"

#include "../Addon.hpp"
#include "../Diagnostics.hpp"
#include "../LuaManager.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace api
{
#if _WIN32
    template<typename T> static void Release(T *&object)
    {
        if (object) object->Release();
        object = nullptr;
    }

    namespace
    {
        using GLProc = void (APIENTRY *)();
        using GetProcAddressFn = void *(*)(const char *);

        GetProcAddressFn GetGLProc()
        {
            static GetProcAddressFn proc = []
            {
                const HMODULE sdl = GetModuleHandleA("SDL2.dll");
                return sdl ? reinterpret_cast<GetProcAddressFn>(GetProcAddress(sdl, "SDL_GL_GetProcAddress")) : nullptr;
            }();
            return proc;
        }

        template<typename T> T GL(const char *name)
        {
            const GetProcAddressFn proc = GetGLProc();
            if (!proc)
                throw std::runtime_error("OpenGL is not initialized");
            T function = reinterpret_cast<T>(proc(name));
            if (!function)
                throw std::runtime_error(std::string("OpenGL function unavailable: ") + name);
            return function;
        }

        constexpr unsigned int GL_TEXTURE_2D_VALUE = 0x0DE1;
        constexpr unsigned int GL_RGBA_VALUE = 0x1908;
        constexpr unsigned int GL_UNSIGNED_BYTE_VALUE = 0x1401;
        constexpr unsigned int GL_LINEAR_VALUE = 0x2601;
        constexpr unsigned int GL_TEXTURE_MIN_FILTER_VALUE = 0x2801;
        constexpr unsigned int GL_TEXTURE_MAG_FILTER_VALUE = 0x2800;

        // CLSID_MMDeviceEnumerator; defined inline because its import library
        // varies between SDKs.
        // {BCDE0395-E52F-467C-8E3D-C4579291692E}
        const CLSID SandiumMMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C,
                                                 {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
    }

    struct Video::Impl
    {
        IMFSourceReader *reader = nullptr;
        unsigned int texture = 0;
        int width = 0;
        int height = 0;
        long long duration100ns = 0;
        int layer = 0;
        float rotation = 0.0f;

        bool playing = false;
        bool ended = false;
        bool hasFrame = false;
        // Playback position = positionOffset100ns + (now - clockStart) while playing.
        long long positionOffset100ns = 0;
        std::chrono::steady_clock::time_point clockStart{};
        long long nextFrameTime100ns = 0;

        long long stride = 0;
        std::vector<unsigned char> uploadBuffer;

        // Audio. The source reader decodes the audio track to 32-bit float PCM and
        // a background thread feeds WASAPI shared-mode rendering; both video and
        // audio free-run on their own clocks, which is close enough for overlays.
        bool audioAvailable = false;
        bool audioTried = false;
        bool audioEnd = false;
        UINT32 audioBufferFrames = 0;
        int audioChannels = 2;
        IAudioClient *audioClient = nullptr;
        IAudioRenderClient *audioRender = nullptr;
        std::thread audioThread;
        std::atomic<bool> audioRunning{false};
        // Device control stays on the audio thread; the main thread only
        // requests state changes here.
        std::atomic<bool> flushRequested{false};
        bool deviceRunning = false;
        std::mutex audioMutex;
        // The MF source reader is NOT thread-safe; video decodes on the main
        // thread and audio on its own, so every reader call is serialized.
        std::mutex readerMutex;
        std::vector<float> audioQueue;
        // Audio playout position is the master A/V clock: frames actually
        // consumed by the device (written minus still-buffered padding).
        std::atomic<unsigned long long> audioFramesWritten{0};
        std::atomic<UINT32> audioPadding{0};
        UINT32 audioSampleRate = 48000;

        ~Impl()
        {
            StopAudioThread();
            ReleaseAudioDevice();
            ReleaseTexture();
            Release(reader);
        }

        double PositionSeconds() const
        {
            if (!playing)
                return static_cast<double>(positionOffset100ns) / 10000000.0;
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - clockStart).count();
            return static_cast<double>(positionOffset100ns + elapsed * 10000) / 10000000.0;
        }

        void ReleaseTexture()
        {
            if (!texture) return;
            using DeleteTextures = void (APIENTRY *)(int, const unsigned int *);
            if (GetGLProc())
                GL<DeleteTextures>("glDeleteTextures")(1, &texture);
            texture = 0;
        }

        void Seek(long long time100ns)
        {
            flushRequested = true;
            PROPVARIANT position;
            PropVariantInit(&position);
            position.vt = VT_I8;
            position.hVal.QuadPart = time100ns;
            HRESULT result;
            {
                const std::lock_guard<std::mutex> readerLock(readerMutex);
                result = reader->SetCurrentPosition(GUID_NULL, position);
            }
            PropVariantClear(&position);
            if (FAILED(result))
                throw std::runtime_error("Could not seek the video");
            positionOffset100ns = time100ns;
            nextFrameTime100ns = time100ns;
            clockStart = std::chrono::steady_clock::now();
            ended = false;
            hasFrame = false;
            const std::lock_guard<std::mutex> lock(audioMutex);
            audioQueue.clear();
            audioEnd = false;
        }

        void StopAudioThread()
        {
            if (audioThread.joinable())
            {
                audioRunning = false;
                audioThread.join();
            }
        }

        void ReleaseAudioDevice()
        {
            Release(audioRender);
            Release(audioClient);
        }

        // Decodes one audio frame into the queue. Returns false at end of the audio track.
        bool DecodeAudioChunk()
        {
            IMFSample *sample = nullptr;
            DWORD flags = 0;
            long long timestamp = 0;
            HRESULT result;
            {
                const std::lock_guard<std::mutex> readerLock(readerMutex);
                result = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr,
                                            &flags, &timestamp, &sample);
            }
            if (FAILED(result))
            {
                Release(sample);
                audioEnd = true;
                return false;
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) || !sample)
            {
                Release(sample);
                audioEnd = true;
                return false;
            }
            IMFMediaBuffer *buffer = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)))
            {
                BYTE *data = nullptr;
                DWORD maxLength = 0, currentLength = 0;
                if (SUCCEEDED(buffer->Lock(&data, &maxLength, &currentLength)) && data)
                {
                    const float *samples = reinterpret_cast<const float *>(data);
                    const std::size_t count = currentLength / sizeof(float);
                    const std::lock_guard<std::mutex> lock(audioMutex);
                    audioQueue.insert(audioQueue.end(), samples, samples + count);
                    buffer->Unlock();
                }
            }
            Release(buffer);
            Release(sample);
            return true;
        }

        void AudioThreadProc()
        {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            UINT64 loops = 0;
            while (audioRunning)
            {
                // Device follows playback: seek flushes stale buffered audio
                // (loop restarts, leave-server stops), pause halts output so
                // the audio clock never runs ahead of the video clock.
                if (flushRequested.exchange(false))
                {
                    audioClient->Stop();
                    audioClient->Reset();
                    deviceRunning = false;
                    audioFramesWritten = 0;
                    audioPadding = 0;
                }
                if (playing && !deviceRunning)
                {
                    audioClient->Start();
                    deviceRunning = true;
                }
                else if (!playing && deviceRunning)
                {
                    audioClient->Stop();
                    deviceRunning = false;
                }
                if (!playing)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                // live diagnostics for silent machines: decode/render progress
                if (++loops % 200 == 0)
                {
                    const std::lock_guard<std::mutex> lock(audioMutex);
                    diag::Log("audio", "channels=%d bufferFrames=%u queue=%.1fs end=%d playing=%d",
                              audioChannels, audioBufferFrames,
                              static_cast<double>(audioQueue.size()) / (48000.0 * audioChannels),
                              audioEnd ? 1 : 0, playing ? 1 : 0);
                }
                UINT32 padding = 0;
                if (FAILED(audioClient->GetCurrentPadding(&padding)))
                    break;
                audioPadding = padding;
                UINT32 frames = audioBufferFrames - padding;
                if (frames > 0)
                {
                    BYTE *data = nullptr;
                    if (SUCCEEDED(audioRender->GetBuffer(frames, &data)))
                    {
                        std::size_t available = 0;
                        {
                            const std::lock_guard<std::mutex> lock(audioMutex);
                            available = audioQueue.size() / audioChannels;
                        }
                        const UINT32 writable = static_cast<UINT32>(std::min<std::size_t>(frames, available));
                        float *output = reinterpret_cast<float *>(data);
                        if (writable > 0)
                        {
                            const std::lock_guard<std::mutex> lock(audioMutex);
                            std::copy(audioQueue.begin(),
                                      audioQueue.begin() + static_cast<std::ptrdiff_t>(writable) * audioChannels,
                                      output);
                            audioQueue.erase(audioQueue.begin(),
                                             audioQueue.begin() + static_cast<std::ptrdiff_t>(writable) * audioChannels);
                        }
                        // anything the queue could not fill stays silent
                        for (UINT32 i = static_cast<UINT32>(writable) * audioChannels; i < frames * audioChannels; ++i)
                            output[i] = 0.0f;
                        audioRender->ReleaseBuffer(frames, 0);
                        audioFramesWritten += frames;
                    }
                }
                // keep a healthy buffer of decoded frames ahead of the device.
                // DecodeAudioChunk takes the lock itself, so check outside of it.
                auto bufferedFrames = [&]()
                {
                    const std::lock_guard<std::mutex> lock(audioMutex);
                    return audioQueue.size() / static_cast<std::size_t>(audioChannels);
                };
                while (audioAvailable && !audioEnd && playing &&
                       bufferedFrames() < static_cast<std::size_t>(audioBufferFrames))
                {
                    if (!DecodeAudioChunk()) break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            audioClient->Stop();
            CoUninitialize();
        }

        // Decodes one video frame and uploads it to the texture. Returns false at end of stream.
        bool DecodeNextFrame()
        {
            IMFSample *sample = nullptr;
            DWORD flags = 0;
            long long timestamp = 0;
            HRESULT result;
            {
                const std::lock_guard<std::mutex> readerLock(readerMutex);
                result = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr,
                                            &flags, &timestamp, &sample);
            }
            if (FAILED(result))
            {
                Release(sample);
                throw std::runtime_error("Video decoding failed");
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) || !sample)
            {
                Release(sample);
                ended = true;
                playing = false;
                positionOffset100ns = duration100ns;
                return false;
            }

            IMFMediaBuffer *buffer = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)))
            {
                BYTE *data = nullptr;
                DWORD maxLength = 0, currentLength = 0;
                if (SUCCEEDED(buffer->Lock(&data, &maxLength, &currentLength)) && data)
                {
                    // RGB32 rows are BGRX and videos carry no alpha. The source stride may
                    // make rows top-down (positive) or bottom-up (negative); we normalize to
                    // bottom-up upload, the convention every texture in this API shares.
                    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
                    const bool topDown = stride >= 0;
                    const std::size_t sourceStride = static_cast<std::size_t>(topDown ? stride : -stride);
                    uploadBuffer.resize(static_cast<std::size_t>(width) * height * 4);
                    for (int row = 0; row < height; ++row)
                    {
                        // upload row 0 is the bottom row of the picture
                        const std::size_t pictureRowFromBottom = static_cast<std::size_t>(row);
                        const std::size_t sourceRow = topDown ? (static_cast<std::size_t>(height) - 1 - pictureRowFromBottom)
                                                              : pictureRowFromBottom;
                        const unsigned char *source = data + sourceRow * sourceStride;
                        unsigned char *destination = uploadBuffer.data() + pictureRowFromBottom * rowBytes;
                        for (int x = 0; x < width; ++x)
                        {
                            destination[static_cast<std::size_t>(x) * 4 + 0] = source[static_cast<std::size_t>(x) * 4 + 2];
                            destination[static_cast<std::size_t>(x) * 4 + 1] = source[static_cast<std::size_t>(x) * 4 + 1];
                            destination[static_cast<std::size_t>(x) * 4 + 2] = source[static_cast<std::size_t>(x) * 4 + 0];
                            destination[static_cast<std::size_t>(x) * 4 + 3] = 255;
                        }
                    }
                    buffer->Unlock();

                    using BindTextureFn = void (APIENTRY *)(unsigned int, unsigned int);
                    using TexSubImage2D = void (APIENTRY *)(unsigned int, int, int, int, int, int,
                                                            unsigned int, unsigned int, const void *);
                    GL<BindTextureFn>("glBindTexture")(GL_TEXTURE_2D_VALUE, texture);
                    GL<TexSubImage2D>("glTexSubImage2D")(GL_TEXTURE_2D_VALUE, 0, 0, 0, width, height,
                                                         GL_RGBA_VALUE, GL_UNSIGNED_BYTE_VALUE, uploadBuffer.data());
                    hasFrame = true;
                }
            }
            Release(buffer);
            Release(sample);
            nextFrameTime100ns = timestamp;
            return true;
        }

        // Temporary diagnostics: one status line per second of draws.

        void AdvancePlayback()
        {
            if (!playing) return;
            long long target;
            if (audioAvailable && audioSampleRate)
            {
                // Audio-clock sync: chase the frames the device actually
                // played (written minus still-buffered padding) since the
                // last seek. The video can never outrun the audible audio.
                const unsigned long long written = audioFramesWritten.load();
                const unsigned long long pad = std::min<unsigned long long>(audioPadding.load(), written);
                target = positionOffset100ns +
                         (long long)((double)(written - pad) * 10000000.0 / audioSampleRate);
            }
            else
            {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - clockStart).count() * 10000;
                target = positionOffset100ns + elapsed;
            }
            // Catch up on every frame the clock has passed, bounded so a stall cannot
            // spin decoding forever inside a draw call.
            int guard = 0;
            while (!ended && nextFrameTime100ns <= target && guard++ < 64)
                if (!DecodeNextFrame()) break;
        }

        void StartAudio()
        {
            if (audioTried)
            {
                if (audioAvailable && !audioThread.joinable())
                {
                    audioRunning = true;
                    audioThread = std::thread(&Impl::AudioThreadProc, this);
                }
                return;
            }
            audioTried = true;

            // configure the reader's audio output to 32-bit float stereo; resampling
            // to the device rate is handled by the source reader's resampler.
            // The audio stream must be selected before its media type can change.
            {
                IMFMediaType *audioType = nullptr;
                if (SUCCEEDED(MFCreateMediaType(&audioType)))
                {
                    audioType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                    audioType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
                    audioType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
                    audioType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                    audioType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
                    audioType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 8);
                    audioType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 8);
                    if (SUCCEEDED(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE)) &&
                        SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, audioType)))
                        audioAvailable = true;
                    Release(audioType);
                }
            }

            IMMDeviceEnumerator *enumerator = nullptr;
            IMMDevice *device = nullptr;
            WAVEFORMATEX *mixFormat = nullptr;
            if (SUCCEEDED(CoCreateInstance(SandiumMMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                           __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator))) &&
                enumerator)
            {
                if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) && device)
                {
                    if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                   reinterpret_cast<void **>(&audioClient))) &&
                        audioClient)
                    {
                        if (SUCCEEDED(audioClient->GetMixFormat(&mixFormat)) && mixFormat)
                        {
                            audioChannels = mixFormat->nChannels;
                            if (SUCCEEDED(audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                                                  3000000ull, 0, mixFormat, nullptr)) &&
                                SUCCEEDED(audioClient->GetBufferSize(&audioBufferFrames)))
                            {
                                audioSampleRate = mixFormat->nChannels ? mixFormat->nSamplesPerSec : 48000;
                                audioClient->GetService(__uuidof(IAudioRenderClient),
                                                        reinterpret_cast<void **>(&audioRender));
                            }
                        }
                    }
                }
            }
            if (mixFormat)
                CoTaskMemFree(mixFormat);
            Release(device);
            Release(enumerator);

            if (audioAvailable && audioRender)
            {
                // the media type change above needs a flush before decoding continues
                Seek(positionOffset100ns);
                audioRunning = true;
                audioThread = std::thread(&Impl::AudioThreadProc, this);
            }
            else
            {
                audioAvailable = false;
                ReleaseAudioDevice();
            }

            // one-line status for diagnosing silent machines
            diag::Log("audio", "available=%d render=%d channels=%d bufferFrames=%u",
                      audioAvailable ? 1 : 0, audioRender ? 1 : 0,
                      audioChannels, audioBufferFrames);
        }
    };
#endif

    Video::Video() = default;
    Video::~Video() = default;

    void Video::Delete()
    {
#if _WIN32
        impl.reset();
#endif
    }
    int Video::Width() const { return impl->width; }
    int Video::Height() const { return impl->height; }
    bool Video::IsValid() const { return impl && impl->texture != 0; }
    int Video::Layer() const { return impl->layer; }
    void Video::SetLayer(int layer) { impl->layer = layer; }
    float Video::Rotation() const { return impl->rotation; }
    void Video::SetRotation(float rotation) { impl->rotation = rotation; }

    std::shared_ptr<Video> Video::Load(const std::string &path)
    {
#if _WIN32
        const auto resolved = ResolveAddonMediaPath(path);
        if (!std::filesystem::is_regular_file(resolved))
            throw std::runtime_error("Video file does not exist: " + path);

        auto video = std::shared_ptr<Video>(new Video());
        video->impl = std::unique_ptr<Impl>(new Impl());

        IMFMediaType *outputType = nullptr;
        HRESULT result = S_OK;
        // Decoders output NV12/YUV; converting to RGB32 needs the reader's
        // built-in video processor, which is off by default.
        IMFAttributes *readerAttributes = nullptr;
        if (SUCCEEDED(result)) result = MFCreateAttributes(&readerAttributes, 1);
        if (SUCCEEDED(result))
            result = readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        if (SUCCEEDED(result))
            result = MFCreateSourceReaderFromURL(resolved.c_str(), readerAttributes, &video->impl->reader);
        Release(readerAttributes);

        if (SUCCEEDED(result))
            result = video->impl->reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        if (SUCCEEDED(result))
            result = video->impl->reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
        if (SUCCEEDED(result)) result = MFCreateMediaType(&outputType);
        if (SUCCEEDED(result))
            result = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(result))
            result = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (SUCCEEDED(result))
            result = video->impl->reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outputType);
        Release(outputType);

        IMFMediaType *nativeType = nullptr;
        UINT32 width = 0, height = 0;
        if (SUCCEEDED(result))
            result = video->impl->reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &nativeType);
        if (SUCCEEDED(result))
            result = MFGetAttributeSize(nativeType, MF_MT_FRAME_SIZE, &width, &height);
        UINT32 strideValue = static_cast<UINT32>(width * 4);
        if (nativeType)
            nativeType->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideValue);
        video->impl->stride = static_cast<INT32>(strideValue);
        PROPVARIANT durationValue;
        PropVariantInit(&durationValue);
        if (SUCCEEDED(video->impl->reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,
                                                                    MF_PD_DURATION, &durationValue)) &&
            durationValue.vt == VT_I8)
            video->impl->duration100ns = durationValue.hVal.QuadPart;
        PropVariantClear(&durationValue);
        Release(nativeType);
        if (FAILED(result) || width == 0 || height == 0)
            throw std::runtime_error("Could not open a video stream in: " + path);

        video->impl->width = static_cast<int>(width);
        video->impl->height = static_cast<int>(height);

        using GenTextures = void (APIENTRY *)(int, unsigned int *);
        using BindTexture = void (APIENTRY *)(unsigned int, unsigned int);
        using TexParameteri = void (APIENTRY *)(unsigned int, unsigned int, int);
        using TexImage2D = void (APIENTRY *)(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void *);
        if (!GetGLProc())
            throw std::runtime_error("OpenGL is not initialized");
        GL<GenTextures>("glGenTextures")(1, &video->impl->texture);
        GL<BindTexture>("glBindTexture")(GL_TEXTURE_2D_VALUE, video->impl->texture);
        GL<TexParameteri>("glTexParameteri")(GL_TEXTURE_2D_VALUE, GL_TEXTURE_MIN_FILTER_VALUE, GL_LINEAR_VALUE);
        GL<TexParameteri>("glTexParameteri")(GL_TEXTURE_2D_VALUE, GL_TEXTURE_MAG_FILTER_VALUE, GL_LINEAR_VALUE);
        GL<TexImage2D>("glTexImage2D")(GL_TEXTURE_2D_VALUE, 0, GL_RGBA_VALUE, video->impl->width, video->impl->height,
                                       0, GL_RGBA_VALUE, GL_UNSIGNED_BYTE_VALUE, nullptr);
        return video;
#else
        (void)path;
        throw std::runtime_error("Video.Load is currently available on Windows only");
#endif
    }

    void Video::Play()
    {
#if _WIN32
        if (!impl->reader) throw std::runtime_error("Cannot play a deleted video");
        // Idempotent: a redundant Play (e.g. an addon calling Play every draw
        // because its isPlaying check misfires) must not restart the clock.
        if (impl->playing && !impl->ended)
        {
            return;
        }
        if (impl->ended) impl->Seek(0);
        impl->StartAudio();
        impl->clockStart = std::chrono::steady_clock::now();
        impl->playing = true;
#endif
    }

    void Video::Pause()
    {
#if _WIN32
        impl->positionOffset100ns = static_cast<long long>(impl->PositionSeconds() * 10000000.0);
        impl->playing = false;
#endif
    }

    void Video::Stop()
    {
#if _WIN32
        if (!impl->reader) return;
        impl->playing = false;
        impl->Seek(0);
#endif
    }

    bool Video::IsPlaying() const { return impl->playing; }

    double Video::Duration() const
    {
        return static_cast<double>(impl->duration100ns) / 10000000.0;
    }

    double Video::Position() const { return impl->PositionSeconds(); }

    void Video::SetPosition(double seconds)
    {
#if _WIN32
        if (!impl->reader) throw std::runtime_error("Cannot seek a deleted video");
        const double clamped = std::clamp(seconds, 0.0, Duration());
        impl->Seek(static_cast<long long>(clamped * 10000000.0));
#else
        (void)seconds;
#endif
    }

    void Video::Draw(float x, float y, float width, float height, float red, float green, float blue, float alpha)
    {
#if _WIN32
        if (!impl->texture) throw std::runtime_error("Cannot draw a deleted video");
        if (!IsLuaDrawing()) throw std::runtime_error("Video:Draw may only be used inside DrawHUD or DrawMenu hooks");
        if (width <= 0 || height <= 0) throw std::runtime_error("Video width and height must be positive");
        impl->AdvancePlayback();
        red = std::clamp(red, 0.0f, 1.0f); green = std::clamp(green, 0.0f, 1.0f);
        blue = std::clamp(blue, 0.0f, 1.0f); alpha = std::clamp(alpha, 0.0f, 1.0f);
        QueueDraw(impl->texture, x, y, width, height, red, green, blue, alpha, impl->layer, impl->rotation);
#else
        (void)x;(void)y;(void)width;(void)height;(void)red;(void)green;(void)blue;(void)alpha;
#endif
    }

    void InitializeVideos()
    {
#if _WIN32
        MFStartup(MF_VERSION, MFSTARTUP_LITE);
#endif
    }
    void ShutdownVideos()
    {
#if _WIN32
        MFShutdown();
#endif
    }
}
