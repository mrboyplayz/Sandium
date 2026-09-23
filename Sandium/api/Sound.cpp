#include "Sound.hpp"

#include "../Addresses.hpp"

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
        // Media Foundation is explicitly configured to decode stereo.  Keep
        // that separate from the endpoint channel count: the old mixer treated
        // a stereo music file as if it already had the speaker layout of the
        // output device, which also preserved the song's original stereo image.
        // A positional source must first become one mono point source.
        static constexpr int sourceChannels = 2;
        int outputChannels = 2;

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

        bool native3D = false;
        unsigned int nativeSoundID = 4095;
        int nativeSourceID = -1;
        float nativeVolume = 1.0f;

        void SetNativeSourceValue(std::size_t field, float value)
        {
            if (!native3D || nativeSourceID < 0 || nativeSourceID >= 64 || !addresses::Base.ptr)
                return;
            constexpr std::size_t DWORD_BASE_RVA = 0x404A6C;
            constexpr std::size_t SOURCE_STRIDE_DWORDS = 278720;
            auto *globals = reinterpret_cast<std::uint32_t *>(reinterpret_cast<std::uintptr_t>(addresses::Base.ptr) + DWORD_BASE_RVA);
            std::memcpy(&globals[SOURCE_STRIDE_DWORDS * nativeSourceID + field], &value, sizeof(value));
        }

        void StopNativeSource()
        {
            if (native3D && nativeSourceID >= 0 && nativeSourceID < 64 && addresses::Base.ptr)
            {
                constexpr std::size_t DWORD_BASE_RVA = 0x404A6C;
                constexpr std::size_t SOURCE_STRIDE_DWORDS = 278720;
                constexpr std::size_t SOURCE_ACTIVE = 327697;
                auto *globals = reinterpret_cast<std::uint32_t *>(reinterpret_cast<std::uintptr_t>(addresses::Base.ptr) + DWORD_BASE_RVA);
                globals[SOURCE_STRIDE_DWORDS * nativeSourceID + SOURCE_ACTIVE] = 0;
            }
            nativeSourceID = -1;
        }

        ~Impl() { StopNativeSource(); StopThread(); }

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
                    availableNow = audioQueue.size() / sourceChannels;
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
                            available = audioQueue.size() / sourceChannels;
                        }
                        const UINT32 writable = static_cast<UINT32>(std::min<std::size_t>(frames, available));
                        float *output = reinterpret_cast<float *>(data);
                        if (writable > 0)
                        {
                            const std::lock_guard<std::mutex> lock(audioMutex);
                            const float vol = volume.load();
                            const float gl_ = gainL.load(), gr_ = gainR.load();
                            const std::ptrdiff_t count = static_cast<std::ptrdiff_t>(writable) * sourceChannels;
                            for (UINT32 frame = 0; frame < writable; ++frame)
                            {
                                const std::ptrdiff_t source = static_cast<std::ptrdiff_t>(frame) * sourceChannels;
                                const float mono = (audioQueue[source] + audioQueue[source + 1]) * 0.5f * vol;
                                const std::ptrdiff_t destination = static_cast<std::ptrdiff_t>(frame) * outputChannels;
                                for (int channel = 0; channel < outputChannels; ++channel)
                                    output[destination + channel] = 0.0f;
                                output[destination] = mono * gl_;
                                if (outputChannels > 1)
                                    output[destination + 1] = mono * gr_;
                            }
                            audioQueue.erase(audioQueue.begin(), audioQueue.begin() + count);
                        }
                        for (UINT32 i = static_cast<UINT32>(writable) * outputChannels; i < frames * outputChannels; ++i)
                            output[i] = 0.0f;
                        audioRender->ReleaseBuffer(frames, 0);
                    }
                }
                // keep a decoded buffer ahead of the device
                auto bufferedFrames = [&]()
                {
                    const std::lock_guard<std::mutex> lock(audioMutex);
                    return audioQueue.size() / static_cast<std::size_t>(sourceChannels);
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
        sound->impl->outputChannels = mixFormat->nChannels ? mixFormat->nChannels : 2;
        const UINT32 outputSampleRate = mixFormat->nSamplesPerSec;

        // Decode at the endpoint's actual clock rate. Feeding fixed 48 kHz
        // samples to a 96 kHz shared-mode endpoint plays audio at double speed.
        IMFMediaType *audioType = nullptr;
        if (FAILED(MFCreateMediaType(&audioType)))
        {
            CoTaskMemFree(mixFormat);
            SndRelease(device);
            SndRelease(enumerator);
            throw std::runtime_error("Could not create audio media type");
        }
        audioType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        audioType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        audioType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, outputSampleRate);
        audioType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        audioType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
        audioType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 8);
        audioType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, outputSampleRate * 8);
        result = sound->impl->reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, audioType);
        SndRelease(audioType);
        if (FAILED(result))
        {
            CoTaskMemFree(mixFormat);
            SndRelease(device);
            SndRelease(enumerator);
            throw std::runtime_error("Could not decode audio in: " + path);
        }

        if (mixFormat)
            CoTaskMemFree(mixFormat);
        SndRelease(device);
        SndRelease(enumerator);

        sound->impl->audioRunning = true;
        sound->impl->audioThread = std::thread(&Impl::AudioThreadProc, sound->impl.get());
        return sound;
    }

    std::shared_ptr<Sound> Sound::Load3D(const std::string &path)
    {
        return Load3D(path, 4095, 1.0f);
    }

    std::shared_ptr<Sound> Sound::Load3D(const std::string &path, unsigned int soundID,
                                         float referenceDistance)
    {
        if (!addresses::RegisterSoundPCMFunc.ptr || !addresses::PlayPositionedSoundFunc.ptr)
            throw std::runtime_error("Sub Rosa native audio functions are unavailable");

        auto sound = std::shared_ptr<Sound>(new Sound());
        sound->impl = std::unique_ptr<Impl>(new Impl());
        sound->impl->native3D = true;
        sound->impl->nativeSoundID = soundID;

        if (FAILED(MFStartup(MF_VERSION)))
            throw std::runtime_error("Media Foundation is not available");
        wchar_t widePath[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, widePath, MAX_PATH);
        IMFSourceReader *reader = nullptr;
        if (FAILED(MFCreateSourceReaderFromURL(widePath, nullptr, &reader)) || !reader)
            throw std::runtime_error("Could not open audio file: " + path);

        reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
        IMFMediaType *type = nullptr;
        MFCreateMediaType(&type);
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
        type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2);
        type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 96000);
        const HRESULT mediaResult = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type);
        SndRelease(type);
        if (FAILED(mediaResult))
        {
            SndRelease(reader);
            throw std::runtime_error("Could not decode native 3D audio: " + path);
        }

        std::vector<std::int16_t> pcm;
        while (true)
        {
            IMFSample *sample = nullptr;
            DWORD flags = 0;
            long long timestamp = 0;
            const HRESULT readResult = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &timestamp, &sample);
            if (FAILED(readResult) || (flags & MF_SOURCE_READERF_ENDOFSTREAM))
            {
                SndRelease(sample);
                break;
            }
            if (!sample)
                continue;
            IMFMediaBuffer *buffer = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && buffer)
            {
                BYTE *data = nullptr;
                DWORD maximum = 0, length = 0;
                if (SUCCEEDED(buffer->Lock(&data, &maximum, &length)) && data)
                {
                    const auto *samples = reinterpret_cast<const std::int16_t *>(data);
                    pcm.insert(pcm.end(), samples, samples + length / sizeof(std::int16_t));
                    buffer->Unlock();
                }
            }
            SndRelease(buffer);
            SndRelease(sample);
        }
        SndRelease(reader);
        if (pcm.empty() || pcm.size() > static_cast<std::size_t>(INT_MAX / 2))
            throw std::runtime_error("Native 3D audio decoded no usable samples: " + path);

        addresses::RegisterSoundPCMFunc(soundID, static_cast<int>(pcm.size() * sizeof(std::int16_t)),
                                        pcm.data(), referenceDistance);
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

    void Sound::Play3D(float x, float y, float z, float volume, bool loop)
    {
        if (!impl->native3D)
        {
            Play(volume);
            return;
        }
        impl->StopNativeSource();
        structs::CVector3 position(x, y, z);
        impl->nativeVolume = volume;
        impl->nativeSourceID = addresses::PlayPositionedSoundFunc(
            static_cast<int>(impl->nativeSoundID), &position, volume, 1.0f, loop ? 1u : 0u);
        if (impl->nativeSourceID < 0)
            throw std::runtime_error("Sub Rosa has no free native audio source");
    }

    void Sound::PlayOneShot3D(float x, float y, float z, float volume, float pitch)
    {
        if (!impl->native3D)
        {
            Play(volume);
            return;
        }
        structs::CVector3 position(x, y, z);
        addresses::PlayPositionedSoundFunc(static_cast<int>(impl->nativeSoundID), &position,
                                            volume, pitch, 0u);
    }

    void Sound::SetPosition(float x, float y, float z)
    {
        constexpr std::size_t SOURCE_X = 606302;
        impl->SetNativeSourceValue(SOURCE_X, x);
        impl->SetNativeSourceValue(SOURCE_X + 1, y);
        impl->SetNativeSourceValue(SOURCE_X + 2, z);
    }

    void Sound::SetVolume(float volume)
    {
        impl->nativeVolume = volume;
        constexpr std::size_t SOURCE_GAIN = 327699;
        impl->SetNativeSourceValue(SOURCE_GAIN, volume);
    }

    void Sound::Pause()
    {
        if (impl->native3D) { constexpr std::size_t SOURCE_GAIN = 327699; impl->SetNativeSourceValue(SOURCE_GAIN, 0.0f); return; }
        impl->playing = false;
    }

    void Sound::Resume()
    {
        if (impl->native3D) { SetVolume(impl->nativeVolume); return; }
        impl->playing = true;
    }

    void Sound::Stop()
    {
        if (impl->native3D) { impl->StopNativeSource(); return; }
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
    std::shared_ptr<Sound> Sound::Load3D(const std::string &path) { return Load(path); }
    std::shared_ptr<Sound> Sound::Load3D(const std::string &path, unsigned int, float) { return Load(path); }
    Sound::~Sound() = default;
    void Sound::Play(float) {}
    void Sound::Play3D(float, float, float, float, bool) {}
    void Sound::PlayOneShot3D(float, float, float, float, float) {}
    void Sound::SetPosition(float, float, float) {}
    void Sound::SetVolume(float) {}
    void Sound::Pause() {}
    void Sound::Resume() {}
    void Sound::SetGainLR(float, float) {}
    void Sound::Stop() {}
}
#endif
