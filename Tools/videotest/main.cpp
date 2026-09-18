// Standalone repro of Sandium's Video playback sequence (MF reader, RGB32,
// audio float stream + device-free audio decode thread, video decode loop).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfobjects.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <stdio.h>
#include <mutex>
#include <thread>
#include <atomic>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "propsys.lib")

template <typename T> static void Release(T *&p) { if (p) { p->Release(); p = nullptr; } }

static WAVEFORMATEX *g_fmt_unused = nullptr;
int main(int argc, char **argv)
{
    const char *file = argc > 1 ? argv[1] : "server.mp4";
    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, file, -1, wpath, MAX_PATH);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    IMFAttributes *attrs = nullptr;
    IMFSourceReader *reader = nullptr;
    MFCreateAttributes(&attrs, 1);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    HRESULT hr = MFCreateSourceReaderFromURL(wpath, attrs, &reader);
    Release(attrs);
    printf("open: 0x%lx\n", hr);

    reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    IMFMediaType *vt = nullptr;
    MFCreateMediaType(&vt);
    vt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    vt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, vt);
    Release(vt);
    printf("video type: 0x%lx\n", hr);

    PROPVARIANT dur; PropVariantInit(&dur);
    if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &dur)) && dur.vt == VT_I8)
        printf("duration: %.3fs\n", dur.hVal.QuadPart / 10000000.0);
    else
        printf("duration: unreadable (vt=%d)\n", dur.vt);
    PropVariantClear(&dur);

    // --- audio enable exactly like StartAudio(), including the flush seek
    IMFMediaType *at = nullptr;
    MFCreateMediaType(&at);
    at->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    at->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    at->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    at->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    at->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
    at->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 8);
    at->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 8);
    hr = reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    printf("audio select: 0x%lx\n", hr);
    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, at);
    Release(at);
    printf("audio type: 0x%lx\n", hr);
    PROPVARIANT pos; PropVariantInit(&pos); pos.vt = VT_I8; pos.hVal.QuadPart = 0;
    hr = reader->SetCurrentPosition(GUID_NULL, pos);
    PropVariantClear(&pos);
    printf("seek0: 0x%lx\n", hr);

    // --- audio decode thread (mutex-serialized like the DLL)
    std::mutex readerMutex;
    std::atomic<bool> run{true};
    std::atomic<bool> playing{true};
    std::atomic<bool> flushRequested{false};
    static WAVEFORMATEX *g_fmt = nullptr;
    static IAudioClient *g_client = nullptr;
    std::thread audio([&]()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator *enumr = nullptr;
        IMMDevice *dev = nullptr;
        IAudioRenderClient *render = nullptr;
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumr);
        if (enumr) enumr->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        if (dev) dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&g_client);
        UINT32 bufferFrames = 0;
        if (g_client && SUCCEEDED(g_client->GetMixFormat(&g_fmt)))
            if (SUCCEEDED(g_client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, g_fmt, nullptr)))
            {
                g_client->GetBufferSize(&bufferFrames);
                g_client->GetService(__uuidof(IAudioRenderClient), (void**)&render);
            }
        printf("wasapi: client=%d render=%d bufferFrames=%u\n", !!g_client, !!render, bufferFrames);
        bool deviceRunning = false;
        std::vector<float> queue;
        int decoded = 0;
        while (run)
        {
            if (flushRequested.exchange(false)) { if (g_client) { g_client->Stop(); g_client->Reset(); } deviceRunning = false; queue.clear(); }
            if (playing && !deviceRunning && g_client) { g_client->Start(); deviceRunning = true; }
            else if (!playing && deviceRunning && g_client) { g_client->Stop(); deviceRunning = false; }
            if (!playing) { Sleep(5); continue; }
            UINT32 padding = 0;
            if (g_client && render && SUCCEEDED(g_client->GetCurrentPadding(&padding)))
            {
                UINT32 frames = bufferFrames - padding;
                if (frames > 0)
                {
                    BYTE *out = nullptr;
                    if (SUCCEEDED(render->GetBuffer(frames, &out)))
                    {
                        float *fout = (float*)out;
                        UINT32 writable = (UINT32)(queue.size() / 2 < frames ? queue.size() / 2 : frames);
                        for (UINT32 i = 0; i < writable * 2; ++i) fout[i] = queue[i];
                        for (UINT32 i = writable * 2; i < frames * 2; ++i) fout[i] = 0.0f;
                        render->ReleaseBuffer(frames, 0);
                        queue.erase(queue.begin(), queue.begin() + writable * 2);
                    }
                }
            }
            while (queue.size() / 2 < bufferFrames)
            {
                DWORD flags = 0; long long ts = 0; IMFSample *s = nullptr;
                HRESULT r;
                {
                    std::lock_guard<std::mutex> lock(readerMutex);
                    r = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &ts, &s);
                }
                if (FAILED(r) || (flags & MF_SOURCE_READERF_ENDOFSTREAM) || !s) { Release(s); printf("audio EOS after %d chunks\n", decoded); break; }
                IMFMediaBuffer *b = nullptr;
                if (SUCCEEDED(s->ConvertToContiguousBuffer(&b)))
                {
                    BYTE *data = nullptr; DWORD len = 0;
                    if (SUCCEEDED(b->Lock(&data, nullptr, &len)))
                    {
                        float *f = (float*)data;
                        for (DWORD i = 0; i < len / 4; ++i) queue.push_back(f[i]);
                        b->Unlock();
                    }
                    Release(b);
                }
                Release(s);
                ++decoded;
            }
            Sleep(5);
        }
        if (g_client) g_client->Stop();
    });

    // --- video decode loop (main "thread")
    int videoFrames = 0;
    long long lastTs = -1;
    while (videoFrames < 500)
    {
        DWORD flags = 0; long long ts = 0; IMFSample *s = nullptr;
        HRESULT r;
        {
            std::lock_guard<std::mutex> lock(readerMutex);
            r = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s);
        }
        Release(s);
        if (FAILED(r) || (flags & MF_SOURCE_READERF_ENDOFSTREAM))
        {
            printf("video EOS r=0x%lx flags=%x after %d frames (last ts %.3fs)\n", r, flags, videoFrames, lastTs / 10000000.0);
            break;
        }
        lastTs = ts;
        if (++videoFrames % 100 == 0) printf("video %d frames, ts %.2fs\n", videoFrames, ts / 10000000.0);
    }
    run = false;
    audio.join();
    printf("done, %d video frames\n", videoFrames);
    Release(reader);
    MFShutdown();
    CoUninitialize();
    return 0;
}
