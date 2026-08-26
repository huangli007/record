#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "capture/AudioFrame.h"
#include "codec/EncodedPacket.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

namespace nr {

// FFmpeg AAC audio encoder. Consumes interleaved Float32 PCM.
class AudioEncoder {
public:
    using PacketCallback = std::function<void(EncodedPacket)>;

    struct Options {
        int sampleRate = 48000;
        int channels = 2;
        int bitrateKbps = 192;
    };

    AudioEncoder() = default;
    ~AudioEncoder();

    AudioEncoder(const AudioEncoder&) = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;

    bool open(const Options& options, PacketCallback onPacket);
    bool encode(const AudioFrame& frame);
    void flush();
    void close();

    bool isOpen() const { return codecCtx_ != nullptr; }
    const AVCodecParameters* codecParameters() const { return params_; }

private:
    void drainPackets();
    bool encodeBuffered(const float* data, int frames, int64_t ptsUs);

    Options options_;
    AVCodecContext* codecCtx_ = nullptr;
    AVCodecParameters* params_ = nullptr;
    PacketCallback onPacket_;
    std::vector<float> pending_;  // interleaved Float32 accumulation buffer
    int64_t samplesConsumed_ = 0;
    int64_t nextPtsUs_ = 0;
    int64_t ptsOffsetUs_ = 0;
};

} // namespace nr
