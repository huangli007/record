#pragma once

#include <functional>

#include "capture/AudioFrame.h"
#include "core/Config.h"

namespace nr {

// Platform audio capturer interface (producer side of the pipeline).
class AudioCapturer {
public:
    using AudioFrameCallback = std::function<void(AudioFrame)>;

    virtual ~AudioCapturer() = default;

    virtual bool start(const AudioConfig& config, AudioFrameCallback onFrame) = 0;
    virtual void stop() = 0;
    virtual void setPaused(bool paused) = 0;
};

} // namespace nr
