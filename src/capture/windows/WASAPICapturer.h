#pragma once

#include <memory>

#include "capture/AudioCapturer.h"

namespace nr {

// Windows audio capturer built on WASAPI. In loopback mode it captures the
// system audio output (what the user hears); otherwise it captures the
// default microphone. Output is always interleaved Float32 at 48 kHz stereo.
class WASAPICapturer final : public AudioCapturer {
public:
    explicit WASAPICapturer(bool loopback);
    ~WASAPICapturer() override;

    bool start(const AudioConfig& config, AudioFrameCallback onFrame) override;
    void stop() override;
    void setPaused(bool paused) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nr
