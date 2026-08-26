#include "codec/EncoderFactory.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace nr {

bool EncoderFactory::hardwareAvailable(const std::string& codec) {
    if (codec == "h265") {
        return avcodec_find_encoder_by_name("hevc_videotoolbox") != nullptr;
    }
    return avcodec_find_encoder_by_name("h264_videotoolbox") != nullptr;
}

std::unique_ptr<VideoEncoder> EncoderFactory::createVideoEncoder(const VideoConfig& config) {
    auto encoder = std::make_unique<VideoEncoder>();
    VideoEncoder::Options options;
    options.width = config.width > 0 ? config.width : 1920;
    options.height = config.height > 0 ? config.height : 1080;
    options.fps = config.fps > 0 ? config.fps : 60;
    options.codec = config.codec;
    options.bitrateMode = config.bitrateMode;
    options.bitrateKbps = config.bitrateKbps;
    options.crf = config.crf;
    options.preset = config.preset;
    return encoder;
}

std::unique_ptr<AudioEncoder> EncoderFactory::createAudioEncoder(const AudioConfig& config) {
    auto encoder = std::make_unique<AudioEncoder>();
    AudioEncoder::Options options;
    options.sampleRate = config.sampleRate > 0 ? config.sampleRate : 48000;
    options.channels = config.channels > 0 ? config.channels : 2;
    options.bitrateKbps = 192;
    return encoder;
}

} // namespace nr
