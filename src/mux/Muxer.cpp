#include "mux/Muxer.h"

#include <algorithm>

extern "C" {
#include <libavutil/opt.h>
}

namespace nr {

Muxer::~Muxer() {
    close();
}

bool Muxer::open(const std::string& path,
                 const std::string& format,
                 const AVCodecParameters* videoPar,
                 const AVCodecParameters* audioPar,
                 int videoFps,
                 int audioSampleRate) {
    close();

    AVFormatContext* ctx = nullptr;
    if (avformat_alloc_output_context2(&ctx, nullptr, format.c_str(),
                                       path.c_str()) < 0 ||
        !ctx) {
        return false;
    }
    fmtCtx_ = ctx;

    if (format == "mp4") {
        // Fragmented MP4: crash-safe, playable even if the app is killed.
        av_opt_set(fmtCtx_->priv_data, "movflags",
                   "frag_keyframe+empty_moov+default_base_moof", 0);
    }

    if (videoPar) {
        AVStream* stream = avformat_new_stream(fmtCtx_, nullptr);
        if (!stream) {
            return false;
        }
        avcodec_parameters_copy(stream->codecpar, videoPar);
        stream->time_base = AVRational{1, videoFps > 0 ? videoFps : 30};
        videoTimeBase_ = stream->time_base;
        if (videoFps > 0) {
            stream->avg_frame_rate = AVRational{videoFps, 1};
        }
        videoStream_ = stream->index;
    }

    if (audioPar) {
        AVStream* stream = avformat_new_stream(fmtCtx_, nullptr);
        if (!stream) {
            return false;
        }
        avcodec_parameters_copy(stream->codecpar, audioPar);
        stream->time_base = AVRational{1, audioSampleRate > 0 ? audioSampleRate : 48000};
        audioTimeBase_ = stream->time_base;
        audioStream_ = stream->index;
    }

    if (avio_open(&fmtCtx_->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) {
        close();
        return false;
    }
    lastDtsUs_[0] = -1;
    lastDtsUs_[1] = -1;
    headerWritten_ = false;
    return true;
}

bool Muxer::writeHeader() {
    if (!fmtCtx_ || headerWritten_) {
        return headerWritten_;
    }
    if (avformat_write_header(fmtCtx_, nullptr) < 0) {
        return false;
    }
    headerWritten_ = true;
    // avformat_write_header may change stream time bases (e.g. mp4 video
    // 1/30 -> 1/15360). Capture the real bases so packet timestamps can be
    // rescaled before av_interleaved_write_frame.
    if (videoStream_ >= 0) {
        videoTimeBase_ = fmtCtx_->streams[videoStream_]->time_base;
    }
    if (audioStream_ >= 0) {
        audioTimeBase_ = fmtCtx_->streams[audioStream_]->time_base;
    }
    return true;
}

bool Muxer::updateStreamParameters(int streamIndex, const AVCodecParameters* par) {
    if (!fmtCtx_ || !par) {
        return false;
    }
    const int target = streamIndex == 0 ? videoStream_ : audioStream_;
    if (target < 0 || target >= static_cast<int>(fmtCtx_->nb_streams)) {
        return false;
    }
    avcodec_parameters_copy(fmtCtx_->streams[target]->codecpar, par);
    return true;
}

bool Muxer::write(const EncodedPacket& packet) {
    if (!fmtCtx_ || packet.data.empty()) {
        return false;
    }
    if (!writeHeader()) {
        return false;
    }
    const int streamIndex = packet.streamIndex == 0 ? videoStream_ : audioStream_;
    const int counterIndex = packet.streamIndex == 0 ? 0 : 1;
    if (streamIndex < 0) {
        return false;
    }

    AVPacket pkt{};
    pkt.data = const_cast<uint8_t*>(packet.data.data());
    pkt.size = static_cast<int>(packet.data.size());
    const AVRational& tb = counterIndex == 0 ? videoTimeBase_ : audioTimeBase_;
    const AVRational us{1, 1'000'000};
    pkt.pts = av_rescale_q(packet.ptsUs, us, tb);
    pkt.dts = std::max(av_rescale_q(packet.dtsUs, us, tb),
                       lastDtsUs_[counterIndex] + 1);
    pkt.duration = av_rescale_q(packet.durationUs, us, tb);
    pkt.stream_index = streamIndex;
    if (packet.keyframe) {
        pkt.flags |= AV_PKT_FLAG_KEY;
    }
    lastDtsUs_[counterIndex] = pkt.dts;

    return av_interleaved_write_frame(fmtCtx_, &pkt) >= 0;
}

bool Muxer::close() {
    if (!fmtCtx_) {
        return true;
    }
    if (!headerWritten_) {
        writeHeader();
    }
    av_write_trailer(fmtCtx_);
    if (fmtCtx_->pb) {
        avio_closep(&fmtCtx_->pb);
    }
    avformat_free_context(fmtCtx_);
    fmtCtx_ = nullptr;
    videoStream_ = -1;
    audioStream_ = -1;
    lastDtsUs_[0] = -1;
    lastDtsUs_[1] = -1;
    headerWritten_ = false;
    return true;
}

} // namespace nr
