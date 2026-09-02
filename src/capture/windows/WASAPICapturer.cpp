#include "capture/windows/WASAPICapturer.h"

#if !defined(_WIN32)
#error "WASAPICapturer is Windows-only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>

// audioclient.h / mmdeviceapi.h declare these GUIDs as extern, but some
// Windows SDK / linker configurations do not provide their definitions
// through uuid.lib. Define the standard values here so the link always
// succeeds (they are only pulled from uuid.lib when left undefined).
EXTERN_C const CLSID CLSID_MMDeviceEnumerator = {
    0xBCDE0395, 0xE52F, 0x467C,
    {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
EXTERN_C const IID IID_IAudioClient = {
    0x1CB9AD4C, 0xDBFA, 0x4C32,
    {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <future>
#include <thread>
#include <vector>

#include "capture/AudioFrame.h"

namespace nr {

struct WASAPICapturer::Impl {
    bool loopback = false;
    AudioConfig config;
    AudioFrameCallback onFrame;
    std::atomic<bool> running{false};
    std::atomic<bool> paused{false};
    std::atomic<float> volume{1.0f};
    std::thread captureThread;

    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    IMMDevice* device = nullptr;
    bool comInitialized = false;

    int sampleRate = 48000;
    int channels = 2;
    int bytesPerSample = 4;
    int validBits = 32;
    bool isFloat = true;

    std::vector<float> resampleScratch;
    int64_t samplesConsumed = 0;

    ~Impl() = default;

    void cleanup() {
        if (audioClient) {
            audioClient->Stop();
        }
        if (captureClient) {
            captureClient->Release();
            captureClient = nullptr;
        }
        if (audioClient) {
            audioClient->Release();
            audioClient = nullptr;
        }
        if (device) {
            device->Release();
            device = nullptr;
        }
        if (comInitialized) {
            CoUninitialize();
            comInitialized = false;
        }
    }

    bool startClient() {
        // Initialize COM on this thread (the capture thread).  Using
        // COINIT_MULTITHREADED lets the audio client be used from the same
        // thread that created it, avoiding cross-thread COM issues.
        HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
            return false;
        }
        comInitialized = true;

        IMMDeviceEnumerator* enumerator = nullptr;
        if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr,
                                    CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
            return false;
        }

        const EDataFlow flow = loopback ? eRender : eCapture;
        const HRESULT hr = enumerator->GetDefaultAudioEndpoint(
            flow, eConsole, &device);
        enumerator->Release();
        if (FAILED(hr) || !device) {
            return false;
        }

        if (FAILED(device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(&audioClient)))) {
            return false;
        }

        WAVEFORMATEX* mixFormat = nullptr;
        if (FAILED(audioClient->GetMixFormat(&mixFormat))) {
            return false;
        }
        sampleRate = static_cast<int>(mixFormat->nSamplesPerSec);
        channels = static_cast<int>(mixFormat->nChannels);

        WAVEFORMATEXTENSIBLE* ext =
            reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFormat);
        if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            isFloat = true;
            bytesPerSample = mixFormat->wBitsPerSample / 8;
            validBits = ext->Samples.wValidBitsPerSample;
        } else if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                   ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
            isFloat = false;
            bytesPerSample = mixFormat->wBitsPerSample / 8;
            validBits = ext->Samples.wValidBitsPerSample;
        } else if (mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            isFloat = true;
            bytesPerSample = mixFormat->wBitsPerSample / 8;
            validBits = mixFormat->wBitsPerSample;
        } else {
            isFloat = false;
            bytesPerSample = mixFormat->wBitsPerSample / 8;
            validBits = mixFormat->wBitsPerSample;
        }
        if (bytesPerSample <= 0) {
            bytesPerSample = 4;
        }
        if (validBits <= 0) {
            validBits = bytesPerSample * 8;
        }

        // 500 ms buffer for reliable capture across all audio drivers.
        // Shared mode is required for loopback capture.
        const REFERENCE_TIME bufferDuration = 50000000LL;  // 500ms
        const DWORD flags = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
        HRESULT initHr = audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED, flags, bufferDuration, 0, mixFormat, nullptr);
        CoTaskMemFree(mixFormat);
        if (FAILED(initHr)) {
            return false;
        }

        if (FAILED(audioClient->GetService(
                IID_PPV_ARGS(&captureClient)))) {
            return false;
        }
        if (FAILED(audioClient->Start())) {
            return false;
        }
        return true;
    }

    // Converts one interleaved buffer into float32 interleaved (native rate).
    std::vector<float> convert(const BYTE* data, UINT32 frames, UINT32 frameSize) {
        std::vector<float> out(static_cast<size_t>(frames) * channels);
        const int srcChannels = channels;
        if (isFloat && bytesPerSample == 4) {
            const auto* src = reinterpret_cast<const float*>(data);
            for (UINT32 i = 0; i < frames * static_cast<UINT32>(srcChannels); ++i) {
                out[i] = src[i];
            }
        } else if (!isFloat && bytesPerSample == 2) {
            const auto* src = reinterpret_cast<const int16_t*>(data);
            for (UINT32 i = 0; i < frames * static_cast<UINT32>(srcChannels); ++i) {
                out[i] = static_cast<float>(src[i]) / 32768.0f;
            }
        } else if (!isFloat && bytesPerSample == 4) {
            const auto* src = reinterpret_cast<const int32_t*>(data);
            const float scale = 1.0f / static_cast<float>(1u << (validBits - 1));
            for (UINT32 i = 0; i < frames * static_cast<UINT32>(srcChannels); ++i) {
                out[i] = static_cast<float>(src[i]) * scale;
            }
        } else if (!isFloat && bytesPerSample == 3) {
            const auto* src = data;
            const float scale = 1.0f / static_cast<float>(1u << (validBits - 1));
            for (UINT32 i = 0; i < frames * static_cast<UINT32>(srcChannels); ++i) {
                int32_t v = (src[i * 3]) | (src[i * 3 + 1] << 8) |
                            (src[i * 3 + 2] << 16);
                if (v & 0x800000) {
                    v |= ~0xFFFFFF;  // sign-extend
                }
                out[i] = static_cast<float>(v) * scale;
            }
        } else {
            // Unknown layout: treat as float32 if big enough, else silence.
            if (bytesPerSample >= 4) {
                const auto* src = reinterpret_cast<const float*>(data);
                for (UINT32 i = 0; i < frames * static_cast<UINT32>(srcChannels); ++i) {
                    out[i] = src[i];
                }
            }
        }
        (void)frameSize;
        return out;
    }

    // Linear-interpolation resampler: native rate -> 48 kHz stereo.
    void resampleTo48k(const std::vector<float>& in, std::vector<float>& out,
                       int frames) {
        const int inCh = channels > 0 ? channels : 2;
        const int targetRate = 48000;
        if (sampleRate == targetRate) {
            out.assign(in.begin(), in.end());
            return;
        }
        const int64_t outFrames =
            static_cast<int64_t>(frames) * targetRate / sampleRate;
        out.resize(static_cast<size_t>(outFrames) * 2);
        for (int64_t o = 0; o < outFrames; ++o) {
            const double pos = static_cast<double>(o) * sampleRate / targetRate;
            const int64_t i0 = static_cast<int64_t>(pos);
            const int64_t i1 = std::min<int64_t>(i0 + 1, frames - 1);
            const float frac = static_cast<float>(pos - i0);
            for (int ch = 0; ch < 2; ++ch) {
                const int srcCh = std::min(ch, inCh - 1);
                const float a = in[static_cast<size_t>(i0) * inCh + srcCh];
                const float b = in[static_cast<size_t>(i1) * inCh + srcCh];
                out[static_cast<size_t>(o) * 2 + ch] = a + frac * (b - a);
            }
        }
    }

    void captureLoop() {
        // COM must be initialized on this thread for WASAPI to work.
        // startClient() already calls CoInitializeEx, but if the capture
        // thread is different from the thread that called startClient(),
        // we re-initialize here.
        bool localCom = false;
        HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(comHr) || comHr == RPC_E_CHANGED_MODE) {
            localCom = true;
        }

        const int sleepMs = 10;
        while (running.load()) {
            UINT32 packetLength = 0;
            if (FAILED(captureClient->GetNextPacketSize(&packetLength))) {
                break;
            }
            if (packetLength == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
                continue;
            }

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            const HRESULT hr = captureClient->GetBuffer(
                &data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr) || !data || frames == 0) {
                if (data || frames > 0) {
                    captureClient->ReleaseBuffer(frames);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
                continue;
            }

            if (!paused.load() && onFrame) {
                std::vector<float> native = convert(data, frames, 0);
                std::vector<float> stereo;
                resampleTo48k(native, stereo, static_cast<int>(frames));
                if (!stereo.empty()) {
                    AudioFrame frame;
                    frame.source = loopback ? AudioSource::System
                                            : AudioSource::Microphone;
                    frame.sampleRate = 48000;
                    frame.channels = 2;
                    const int64_t counter = samplesConsumed;
                    frame.ptsUs = counter * 1'000'000LL / 48000;
                    frame.durationUs =
                        static_cast<int64_t>(stereo.size() / 2) * 1'000'000LL /
                        48000;
                    samplesConsumed += static_cast<int64_t>(stereo.size() / 2);
                    frame.samples = std::move(stereo);
                    const float vol = volume.load();
                    if (vol != 1.0f) {
                        for (float& s : frame.samples) {
                            s *= vol;
                        }
                    }
                    onFrame(std::move(frame));
                }
            } else if (frames > 0) {
                samplesConsumed += frames;
            }
            captureClient->ReleaseBuffer(frames);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (localCom) {
            CoUninitialize();
        }
    }
};

WASAPICapturer::WASAPICapturer(bool loopback) : impl_(std::make_unique<Impl>()) {
    impl_->loopback = loopback;
}

WASAPICapturer::~WASAPICapturer() {
    stop();
}

bool WASAPICapturer::start(const AudioConfig& config,
                           AudioFrameCallback onFrame) {
    if (!impl_) {
        return false;
    }
    stop();
    impl_->config = config;
    impl_->onFrame = std::move(onFrame);
    impl_->volume.store(static_cast<float>(
        (impl_->loopback ? config.systemVolume : config.micVolume)) / 100.0f);
    impl_->paused.store(false);
    impl_->samplesConsumed = 0;
    impl_->running.store(true);

    // Start the capture thread first; it will initialize COM and the audio
    // client on the same thread that will read audio data.
    std::promise<bool> started;
    auto future = started.get_future();
    impl_->captureThread = std::thread([this, &started] {
        if (!impl_->startClient()) {
            impl_->cleanup();
            impl_->running.store(false);
            started.set_value(false);
            return;
        }
        started.set_value(true);
        impl_->captureLoop();
    });

    // Wait up to 2 seconds for the audio client to initialize.
    if (future.wait_for(std::chrono::seconds(2)) == std::future_status::timeout ||
        !future.get()) {
        impl_->running.store(false);
        if (impl_->captureThread.joinable()) {
            impl_->captureThread.join();
        }
        impl_->onFrame = nullptr;
        return false;
    }
    return true;
}

void WASAPICapturer::stop() {
    if (!impl_) {
        return;
    }
    impl_->running.store(false);
    if (impl_->captureThread.joinable()) {
        impl_->captureThread.join();
    }
    impl_->cleanup();
    impl_->onFrame = nullptr;
}

void WASAPICapturer::setPaused(bool paused) {
    if (impl_) {
        impl_->paused.store(paused);
    }
}

} // namespace nr
