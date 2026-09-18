// Generates a small animated MP4 (H.264) for testing the Lua video API.
// Usage: make_test_mp4 <output.mp4>
#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <cstdio>
#include <cstdint>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

template<typename T> static void Release(T *&object) { if (object) object->Release(); object = nullptr; }

int main(int argc, char **argv)
{
    if (argc < 2) { std::printf("usage: make_test_mp4 <output.mp4>\n"); return 1; }
    const UINT32 width = 320, height = 180;
    const UINT32 fps = 15;
    const UINT64 frameCount = fps * 4; // four seconds

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);

    IMFSinkWriter *writer = nullptr;
    IMFAttributes *attributes = nullptr;
    hr = MFCreateAttributes(&attributes, 1);
    hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
    hr = MFCreateSinkWriterFromURL(argv[1], nullptr, attributes, &writer);

    DWORD streamIndex = 0;
    IMFMediaType *outputType = nullptr;
    hr = MFCreateMediaType(&outputType);
    hr = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    hr = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    hr = MFSetAttributeSize(outputType, MF_MT_FRAME_SIZE, width, height);
    hr = outputType->SetUINT32(MF_MT_AVG_BITRATE, 1500000);
    hr = MFSetAttributeRatio(outputType, MF_MT_FRAME_RATE, fps, 1);
    hr = MFSetAttributeRatio(outputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = writer->AddStream(outputType, &streamIndex);

    IMFMediaType *inputType = nullptr;
    hr = MFCreateMediaType(&inputType);
    hr = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    hr = inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = MFSetAttributeSize(inputType, MF_MT_FRAME_SIZE, width, height);
    hr = MFSetAttributeRatio(inputType, MF_MT_FRAME_RATE, fps, 1);
    hr = MFSetAttributeRatio(inputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = writer->SetInputMediaType(streamIndex, inputType, nullptr);

    hr = writer->BeginWriting();

    const UINT32 stride = width * 4;
    for (UINT64 frame = 0; frame < frameCount; frame++)
    {
        IMFSample *sample = nullptr;
        IMFMediaBuffer *buffer = nullptr;
        hr = MFCreateSample(&sample);
        hr = MFCreateMemoryBuffer(stride * height, &buffer);
        BYTE *data = nullptr;
        hr = buffer->Lock(&data, nullptr, nullptr);

        // Color-cycling thirds plus a sweeping white bar: easy to see motion.
        const BYTE hue = static_cast<BYTE>((frame * 255) / frameCount);
        const INT32 bar = static_cast<INT32>((frame * width) / frameCount);
        for (UINT32 y = 0; y < height; y++)
        {
            BYTE *row = data + static_cast<std::size_t>(y) * stride;
            for (UINT32 x = 0; x < width; x++)
            {
                BYTE r, g, b;
                if (static_cast<INT32>(x) >= bar && static_cast<INT32>(x) < bar + 24)
                    { r = 255; g = 255; b = 255; }
                else if (y < height / 3)
                    { r = static_cast<BYTE>(255 - hue); g = hue; b = 64; }
                else if (y < height * 2 / 3)
                    { r = 32; g = static_cast<BYTE>(255 - hue); b = hue; }
                else
                    { r = hue; g = 64; b = static_cast<BYTE>(255 - hue); }
                row[static_cast<std::size_t>(x) * 4 + 0] = b;
                row[static_cast<std::size_t>(x) * 4 + 1] = g;
                row[static_cast<std::size_t>(x) * 4 + 2] = r;
                row[static_cast<std::size_t>(x) * 4 + 3] = 255;
            }
        }
        hr = buffer->Unlock();
        hr = buffer->SetCurrentLength(stride * height);
        hr = sample->AddBuffer(buffer);
        const LONGLONG sampleTime = static_cast<LONGLONG>(frame * 10000000 / fps);
        hr = sample->SetSampleTime(sampleTime);
        hr = sample->SetSampleDuration(10000000 / fps);
        hr = writer->WriteSample(streamIndex, sample);
        Release(buffer);
        Release(sample);
    }

    hr = writer->Finalize();
    Release(outputType);
    Release(inputType);
    Release(attributes);
    Release(writer);
    MFShutdown();
    CoUninitialize();
    std::printf("wrote %s\n", argv[1]);
    return SUCCEEDED(hr) ? 0 : 2;
}
