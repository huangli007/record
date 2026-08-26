#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "capture/VideoFrame.h"
#include "codec/EncodedPacket.h"
#include "core/Config.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

namespace nr {

// FFmpeg video encoder (hardware videotoolbox with libx264/libx265 fallback).
class VideoEncoder {
public:
    using PacketCallback = std::function<void(EncodedPacket)>;

    struct Options {
        int width = 1920;
        int height = 1080;
        int fps = 60;
        std::string codec = "auto";  // auto | h264 | h265
        BitrateMode bitrateMode = BitrateMode::FileSize;
        int bitrateKbps = 6000;
        int crf = 18;
        std::string preset = "medium";
    };

    VideoEncoder() = default;
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    // Opens the codec, preferring hardware acceleration. Returns false if the
    // requested codec is unavailable.
    bool open(const Options& options, PacketCallback onPacket);

    // Encodes one raw frame (CVPixelBuffer on macOS). Returns false on error.
    bool encode(VideoFrame&& frame);

    // Flushes remaining frames and closes the codec.
    void flush();
    void close();

    bool isOpen() const { return codecCtx_ != nullptr; }
    const AVCodecParameters* codecParameters() const { return params_; }
    // Re-copies codec parameters from the context. H.264 SPS/PPS extradata is
    // only available after the first frame has been encoded, so call this
    // after encoding begins and before the muxer writes its header.
    void refreshParameters();
    const char* codecName() const { return codecName_.c_str(); }
    int width() const { return options_.width; }
    int height() const { return options_.height; }
    int fps() const { return options_.fps; }

private:
    bool openHardware(const std::string& encoderName);
    bool openSoftware(const std::string& encoderName);
    bool configure();
    bool sendFrame(AVFrame* frame);
    void drainPackets();

    Options options_;
    AVCodecContext* codecCtx_ = nullptr;
    AVCodecParameters* params_ = nullptr;
    PacketCallback onPacket_;
    std::string codecName_;
    bool hardware_ = false;
    int64_t frameIndex_ = 0;
};

} // namespace nr
