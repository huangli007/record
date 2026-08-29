#pragma once

#include <memory>
#include <vector>

#include "capture/ScreenCapturer.h"

namespace nr {

// macOS screen capturer built on ScreenCaptureKit (macOS 12.3+).
// Captures video frames and, optionally, system audio from the same stream.
class ScreenCaptureKitCapturer final : public ScreenCapturer {
public:
    // Debug counters for the audio capture path (for troubleshooting silent
    // recordings). Only meaningful while a stream is active.
    struct AudioDebugStats {
        long long audioCallbacks = 0;    // SCK audio sample buffers received
        long long audioEmptyDrops = 0;   // buffers dropped after parsing (no samples)
        long long audioSamples = 0;      // total float samples delivered to the app
        std::string audioSetupError;     // addStreamOutput audio error, if any
    };

    ScreenCaptureKitCapturer();
    ~ScreenCaptureKitCapturer() override;

    bool start(const VideoConfig& videoConfig,
               const AudioConfig& audioConfig,
               VideoFrameCallback onFrame,
               AudioFrameCallback onAudio) override;
    void stop() override;
    void setPaused(bool paused) override;
    void setSystemAudioCallback(AudioFrameCallback onAudio) override;

    // Lists capture-eligible windows (requires screen recording permission).
    static std::vector<WindowInfo> listWindows();
    // Backing scale factor of the main display (for Retina-aware region size).
    static double displayScale();
    // Current audio-path counters.
    AudioDebugStats audioDebugStats() const;

private:
    bool startStream();
    void restartStream();
    void registerObservers();
    void unregisterObservers();

public:
    // Exposed for the Objective-C++ delegate (same translation unit).
    struct Impl;
    Impl* impl() const { return impl_.get(); }

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace nr
