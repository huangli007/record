#pragma once

#include <cstdint>
#include <string>

#include "codec/EncodedPacket.h"

extern "C" {
#include <libavformat/avformat.h>
}

namespace nr {

// FFmpeg muxer writing fragmented MP4 (or MKV/FLV) files.
class Muxer {
public:
    Muxer() = default;
    ~Muxer();

    Muxer(const Muxer&) = delete;
    Muxer& operator=(const Muxer&) = delete;

    // Opens the output file. `videoPar` / `audioPar` may be null when the
    // respective stream is disabled. The header is deferred until the first
    // encoded packet because codec extradata (H.264 SPS/PPS) only exists
    // after the first frame has been encoded.
    bool open(const std::string& path,
              const std::string& format,
              const AVCodecParameters* videoPar,
              const AVCodecParameters* audioPar,
              int videoFps,
              int audioSampleRate);

    // Writes the header (idempotent). Call after refreshing stream parameters.
    bool writeHeader();
    bool headerWritten() const { return headerWritten_; }
    bool updateStreamParameters(int streamIndex, const AVCodecParameters* par);

    bool write(const EncodedPacket& packet);
    bool close();
    bool isOpen() const { return fmtCtx_ != nullptr; }
    // Returns true if at least one encoded packet has been written.
    bool hasWrittenPackets() const;

private:
    AVFormatContext* fmtCtx_ = nullptr;
    int videoStream_ = -1;
    int audioStream_ = -1;
    int64_t lastDtsUs_[2] = {-1, -1};
    bool headerWritten_ = false;
    AVRational videoTimeBase_{1, 1};
    AVRational audioTimeBase_{1, 1};
};

} // namespace nr
