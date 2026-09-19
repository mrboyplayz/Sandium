#include "Sound.hpp"

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if _WIN32
#include <audioclient.h>
#include <combaseapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <Windows.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace api
{
    template <typename T> static void SndRelease(T *&p)
    {
        if (p)
        {
            p->Release();
            p = nullptr;
        }
    }

    struct Sound::Impl
    {
        // Media Foundation decodes the file to 32-bit float stereo; a
        // background thread feeds WASAPI shared mode until end of stream.
        IMFSourceReader *reader = nullptr;
        IAudioClient *audioClient = nullptr;
        IAudioRenderClient *audioRender = nullptr;
        UINT32 audioBufferFrames = 0;
        int audioChannels = 2;

        std::thread audioThread;
        std::atomic<bool> audioRunning{false};
        std::atomic<bool> playing{false};
        std::atomic<float> volume{1.0f};
        std::atomic<float> gainL{1.0f};
        std::atomic<float> gainR{1.0f};
        std::atomic<bool> flushRequested{false};
        std::mutex audioMutex;
        std::vector<float> audioQueue;
        bool audioEnd = false;

        ~Impl() { StopThread(); }

        void StopThread()
        {
            if (audioThread.joinable())
            {
                audioRunning = false;
                audioThread.join();
            }
            if (audioClient)
                audioClient->Stop();
            SndRelease(audioRender);
            SndRelease(audioClient);
            SndRelease(reader);
        }

        // Decodes one audio frame into the queue. Returns false at end of stream.
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
            if (FAILED(result) || (flags & MF_SOURCE_READERF_ENDOFSTREAM))
            {
                SndRelease(sample);
                audioEnd = true;
                return false;
            }
            if (!sample)
            {
                // seek gap: the reader emits an empty sample after seeking
                return true;
            }
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)))
            {
                BYTE *data = nullptr;
                DWORD maxLength = 0, currentLength = 0;
                if (SUCCEEDED(buffer->Lock(&data, &maxLength, &currentLength)) && data)
                {
                    const float *samples = reinterpret_cast<const float *>(data);
                    const std::lock_guard<std::mutex> lock(audioMutex);
                    audioQueue.insert(audioQueue.end(), samples,
                                      samples + currentLength / sizeof(float));
                    buffer->Unlock();
                }
            }
            SndRelease(buffer);
            SndRelease(sample);
            return true;
        }

        void AudioThreadProc()
        {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            bool deviceRunning = false;
            while (audioRunning)
            {
                if (flushRequested.exchange(false))
                {
                    audioClient->Stop();
                    audioClient->Reset();
                    deviceRunning = false;
                    // rewind the reader: replays must decode from the start
                    {
                        const std::lock_guard<std::mutex> readerLock(readerMutex);
                        PROPVARIANT position;
                        PropVariantInit(&position);
                        position.vt = VT_I8;
                        position.hVal.QuadPart = 0;
                        reader->SetCurrentPosition(GUID_NULL, position);
                        PropVariantClear(&position);
                    }
                    const std::lock_guard<std::mutex> lock(audioMutex);
                    audioQueue.clear();
                    audioEnd = false;
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
                    Sleep(5);
                    continue;
                }

                UINT32 padding = 0;
                if (FAILED(audioClient->GetCurrentPadding(&padding)))
                    break;

                // one-shot: once the stream is decoded AND the device has
                // played out everything buffered, stop (padding would never
                // reach zero if we kept topping the buffer up with silence)
                std::size_t availableNow = 0;
                {
                    const std::lock_guard<std::mutex> lock(audioMutex);
                    availableNow = audioQueue.size() / audioChannels;
                }
                if (audioEnd && availableNow == 0)
                {
                    if (padding == 0)
                        playing = false;
                    Sleep(4);
                    continue;
                }

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
                            const float vol = volume.load();
                            const float gl_ = gainL.load(), gr_ = gainR.load();
                            const std::ptrdiff_t count = static_cast<std::ptrdiff_t>(writable) * audioChannels;
                            for (std::ptrdiff_t i = 0; i < count; ++i)
                            {
                                const float g = (i % audioChannels) == 0 ? gl_
                                                : ((i % audioChannels) == 1 ? gr_ : (gl_ + gr_) * 0.5f);
                                output[i] = audioQueue[i] * vol * g;
                            }
                            audioQueue.erase(audioQueue.begin(), audioQueue.begin() + count);
                        }
                        for (UINT32 i = static_cast<UINT32>(writable) * audioChannels; i < frames * audioChannels; ++i)
                            output[i] = 0.0f;
                        audioRender->ReleaseBuffer(frames, 0);
                    }
                }
                // keep a decoded buffer ahead of the device
                auto bufferedFrames = [&]()
                {
                    const std::lock_guard<std::mutex> lock(audioMutex);
                    return audioQueue.size() / static_cast<std::size_t>(audioChannels);
                };
                while (playing && !audioEnd && bufferedFrames() < static_cast<std::size_t>(audioBufferFrames))
                {
                    if (!DecodeAudioChunk())
                        break;
                }
                Sleep(4);
            }
            CoUninitialize();
        }

        IMFMediaBuffer *buffer = nullptr;
        std::mutex readerMutex;
    };

    std::shared_ptr<Sound> Sound::Load(const std::string &path)
    {
        auto sound = std::shared_ptr<Sound>(new Sound());
        sound->impl = std::unique_ptr<Impl>(new Impl());

        if (FAILED(MFStartup(MF_VERSION)))
            throw std::runtime_error("Media Foundation is not available");

        wchar_t widePath[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, widePath, MAX_PATH);

        HRESULT result = MFCreateSourceReaderFromURL(widePath, nullptr, &sound->impl->reader);
        if (FAILED(result))
            throw std::runtime_error("Could not open audio file: " + path);

        sound->impl->reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        sound->impl->reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

        IMFMediaType *audioType = nullptr;
        if (FAILED(MFCreateMediaType(&audioType)))
            throw std::runtime_error("Could not create audio media type");
        audioType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        audioType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        audioType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        audioType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        audioType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
        audioType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 8);
        audioType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 8);
        result = sound->impl->reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, audioType);
        SndRelease(audioType);
        if (FAILED(result))
            throw std::runtime_error("Could not decode audio in: " + path);

        IMMDeviceEnumerator *enumerator = nullptr;
        IMMDevice *device = nullptr;
        WAVEFORMATEX *mixFormat = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator))) ||
            !enumerator ||
            FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) || !device ||
            FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void **>(&sound->impl->audioClient))) ||
            !sound->impl->audioClient ||
            FAILED(sound->impl->audioClient->GetMixFormat(&mixFormat)) || !mixFormat ||
            FAILED(sound->impl->audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                                        3000000ull, 0, mixFormat, nullptr)) ||
            FAILED(sound->impl->audioClient->GetBufferSize(&sound->impl->audioBufferFrames)) ||
            FAILED(sound->impl->audioClient->GetService(__uuidof(IAudioRenderClient),
                                                        reinterpret_cast<void **>(&sound->impl->audioRender))))
        {
            if (mixFormat)
                CoTaskMemFree(mixFormat);
            SndRelease(device);
            SndRelease(enumerator);
            throw std::runtime_error("Could not initialize audio output for: " + path);
        }
        if (mixFormat)
            sound->impl->audioChannels = mixFormat->nChannels ? mixFormat->nChannels : 2;
        if (mixFormat)
            CoTaskMemFree(mixFormat);
        SndRelease(device);
        SndRelease(enumerator);

        sound->impl->audioRunning = true;
        sound->impl->audioThread = std::thread(&Impl::AudioThreadProc, sound->impl.get());
        return sound;
    }

    Sound::~Sound() = default;

    void Sound::Play(float volume)
    {
        impl->volume = volume;
        impl->gainL = 1.0f;
        impl->gainR = 1.0f;
        impl->flushRequested = true;
        impl->playing = true;
    }

    void Sound::SetGainLR(float left, float right)
    {
        impl->gainL = left;
        impl->gainR = right;
    }

    void Sound::Stop()
    {
        impl->flushRequested = true;
        impl->playing = false;
    }
}
#else
namespace api
{
    std::shared_ptr<Sound> Sound::Load(const std::string &path)
    {
        (void)path;
        throw std::runtime_error("Sound.Load is currently available on Windows only");
    }
    Sound::~Sound() = default;
    void Sound::Play() {}
}
#endif
