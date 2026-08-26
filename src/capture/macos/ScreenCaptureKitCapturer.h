#pragma once

#include <memory>

#include "capture/ScreenCapturer.h"

namespace nr {

// macOS screen capturer built on ScreenCaptureKit (macOS 12.3+).
// Captures video frames and, optionally, system audio from the same stream.
class ScreenCaptureKitCapturer final : public ScreenCapturer {
public:
    ScreenCaptureKitCapturer();
    ~ScreenCaptureKitCapturer() override;

    bool start(const VideoConfig& videoConfig,
               const AudioConfig& audioConfig,
               VideoFrameCallback onFrame) override;
    void stop() override;
    void setPaused(bool paused) override;
    void setSystemAudioCallback(AudioFrameCallback onAudio) override;

private:
public:
    // Exposed for the Objective-C++ delegate (same translation unit).
    struct Impl;
    Impl* impl() const { return impl_.get(); }

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace nr
