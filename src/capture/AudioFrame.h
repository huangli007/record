#pragma once

#include <cstdint>
#include <vector>

namespace nr {

enum class AudioSource {
    System,
    Microphone,
};

// Interleaved Float32 PCM frame.
struct AudioFrame {
    std::vector<float> samples;
    int sampleRate = 0;
    int channels = 0;
    int64_t ptsUs = 0;      // microsecond timeline
    int64_t durationUs = 0;
    AudioSource source = AudioSource::System;

    int frameCount() const {
        return channels > 0 ? static_cast<int>(samples.size()) / channels : 0;
    }
};

} // namespace nr
