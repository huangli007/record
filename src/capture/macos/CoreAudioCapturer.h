#pragma once

#include <memory>

#include "capture/AudioCapturer.h"

namespace nr {

// macOS microphone capturer based on CoreAudio's AUHAL input unit.
class CoreAudioCapturer final : public AudioCapturer {
public:
    CoreAudioCapturer();
    ~CoreAudioCapturer() override;

    bool start(const AudioConfig& config, AudioFrameCallback onFrame) override;
    void stop() override;
    void setPaused(bool paused) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nr
