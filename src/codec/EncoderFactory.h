#pragma once

#include <memory>
#include <string>

#include "codec/AudioEncoder.h"
#include "codec/VideoEncoder.h"
#include "core/Config.h"

namespace nr {

class EncoderFactory {
public:
    // Returns whether a hardware encoder is available for the given codec
    // ("h264" or "h265").
    static bool hardwareAvailable(const std::string& codec);

    static std::unique_ptr<VideoEncoder> createVideoEncoder(const VideoConfig& config);
    static std::unique_ptr<AudioEncoder> createAudioEncoder(const AudioConfig& config);
};

} // namespace nr
