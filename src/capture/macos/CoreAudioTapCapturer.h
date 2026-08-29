#pragma once

#include "capture/AudioCapturer.h"

namespace nr {

// Captures the system-wide audio output (all apps mixed to stereo) using the
// CoreAudio process-tap API introduced in macOS 14.2. This is used as a
// fallback for "system audio" when ScreenCaptureKit delivers no audio buffers
// (a known limitation on recent macOS versions depending on the granted
// privacy permission). Falls back cleanly to failure if the tap cannot start.
class CoreAudioTapCapturer final : public AudioCapturer {
public:
    CoreAudioTapCapturer();
    ~CoreAudioTapCapturer() override;

    bool start(const AudioConfig& config, AudioFrameCallback onFrame) override;
    void stop() override;
    void setPaused(bool paused) override;

    // Counters for diagnostics.
    struct Stats {
        long long frames = 0;      // audio frames delivered
        long long samples = 0;     // total float samples delivered
        long long setupErrors = 0; // failures during tap/device setup
        bool started = false;
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nr
