#pragma once

#include <functional>
#include <string>
#include <vector>

#include "capture/AudioFrame.h"
#include "capture/VideoFrame.h"
#include "core/Config.h"

namespace nr {

// Describes an on-screen window for "lock window" capture.
struct WindowInfo {
    int id = 0;
    std::string title;
    std::string application;
    int width = 0;   // pixel dimensions
    int height = 0;
};

// Platform screen capturer interface (producer side of the pipeline).
class ScreenCapturer {
public:
    using VideoFrameCallback = std::function<void(VideoFrame)>;
    using AudioFrameCallback = std::function<void(AudioFrame)>;

    virtual ~ScreenCapturer() = default;

    // The audio config is passed alongside the video config because on some
    // platforms (ScreenCaptureKit) system audio is captured from the same
    // stream as the screen.
    virtual bool start(const VideoConfig& videoConfig,
                       const AudioConfig& audioConfig,
                       VideoFrameCallback onFrame,
                       AudioFrameCallback onAudio = {}) = 0;
    virtual void stop() = 0;
    virtual void setPaused(bool paused) = 0;

    // Some platforms (ScreenCaptureKit) can also capture system audio from the
    // same session. Default is a no-op for platforms without that capability.
    virtual void setSystemAudioCallback(AudioFrameCallback /*onAudio*/) {}
};

} // namespace nr
