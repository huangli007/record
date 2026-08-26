#include "codec/AudioEncoder.h"

#include <cstring>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace nr {

AudioEncoder::~AudioEncoder() {
    close();
}

bool AudioEncoder::open(const Options& options, PacketCallback onPacket) {
    close();
    options_ = options;
    onPacket_ = std::move(onPacket);

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        return false;
    }
    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        return false;
    }

    codecCtx_->sample_rate = options_.sampleRate;
    codecCtx_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    codecCtx_->bit_rate = static_cast<int64_t>(options_.bitrateKbps) * 1000;
    av_channel_layout_default(&codecCtx_->ch_layout, options_.channels);
    codecCtx_->time_base = AVRational{1, options_.sampleRate};

    if (avcodec_open2(codecCtx_, codec, nullptr) < 0) {
        avcodec_free_context(&codecCtx_);
        return false;
    }
    params_ = avcodec_parameters_alloc();
    avcodec_parameters_from_context(params_, codecCtx_);
    // AAC has a priming delay (~1024 samples); shift input timestamps so
    // encoded packet pts/dts never go negative.
    const int delaySamples =
        codecCtx_->delay > 0 ? codecCtx_->delay : codecCtx_->frame_size;
    ptsOffsetUs_ =
        delaySamples * 1'000'000LL / options_.sampleRate;
    pending_.reserve(codecCtx_->frame_size * options_.channels);
    nextPtsUs_ = 0;
    samplesConsumed_ = 0;
    return true;
}

bool AudioEncoder::encode(const AudioFrame& frame) {
    if (!codecCtx_ || frame.samples.empty()) {
        return false;
    }
    if (frame.channels != options_.channels) {
        // Channel count mismatch: drop the frame rather than corrupt the stream.
        return false;
    }
    pending_.insert(pending_.end(), frame.samples.begin(), frame.samples.end());

    const int frameSize = codecCtx_->frame_size > 0 ? codecCtx_->frame_size : 1024;
    const size_t perFrame = static_cast<size_t>(frameSize) * options_.channels;
    while (pending_.size() >= perFrame) {
        const int64_t ptsUs =
            samplesConsumed_ * 1'000'000LL / options_.sampleRate;
        if (!encodeBuffered(pending_.data(), frameSize, ptsUs)) {
            return false;
        }
        pending_.erase(pending_.begin(), pending_.begin() + perFrame);
        samplesConsumed_ += frameSize;
    }
    return true;
}

bool AudioEncoder::encodeBuffered(const float* data, int frames, int64_t ptsUs) {
    // Interleaved Float32 -> planar Float32 for the AAC encoder.
    SwrContext* swr = nullptr;
    AVChannelLayout outLayout;
    AVChannelLayout inLayout;
    av_channel_layout_default(&outLayout, options_.channels);
    av_channel_layout_default(&inLayout, options_.channels);
    const int allocRet = swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLTP,
                                             options_.sampleRate, &inLayout,
                                             AV_SAMPLE_FMT_FLT, options_.sampleRate,
                                             0, nullptr);
    if (allocRet < 0 || !swr || swr_init(swr) < 0) {
        if (swr) {
            swr_free(&swr);
        }
        return false;
    }

    const uint8_t* inData = reinterpret_cast<const uint8_t*>(data);
    AVFrame* frame = av_frame_alloc();
    frame->format = AV_SAMPLE_FMT_FLTP;
    frame->sample_rate = options_.sampleRate;
    frame->nb_samples = frames;
    av_channel_layout_copy(&frame->ch_layout, &outLayout);
    av_frame_get_buffer(frame, 0);

    int converted = swr_convert(swr, frame->data, frame->nb_samples,
                                &inData, frames);
    swr_free(&swr);
    if (converted <= 0) {
        av_frame_free(&frame);
        return false;
    }
    frame->pts = av_rescale_q(ptsUs + ptsOffsetUs_, AVRational{1, 1'000'000},
                              codecCtx_->time_base);
    const int ret = avcodec_send_frame(codecCtx_, frame);
    av_frame_free(&frame);
    if (ret < 0) {
        return false;
    }
    drainPackets();
    return true;
}

void AudioEncoder::drainPackets() {
    if (!codecCtx_) {
        return;
    }
    AVPacket* packet = av_packet_alloc();
    while (true) {
        const int ret = avcodec_receive_packet(codecCtx_, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            break;
        }
        EncodedPacket out;
        out.ptsUs = av_rescale_q(packet->pts, codecCtx_->time_base,
                                 AVRational{1, 1'000'000});
        out.dtsUs = av_rescale_q(packet->dts, codecCtx_->time_base,
                                 AVRational{1, 1'000'000});
        out.durationUs = av_rescale_q(packet->duration, codecCtx_->time_base,
                                      AVRational{1, 1'000'000});
        out.keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
        out.data.assign(packet->data, packet->data + packet->size);
        av_packet_unref(packet);
        if (onPacket_) {
            onPacket_(std::move(out));
        }
    }
    av_packet_free(&packet);
}

void AudioEncoder::flush() {
    if (!codecCtx_) {
        return;
    }
    // Pad the tail with silence and encode the remainder.
    const int frameSize = codecCtx_->frame_size > 0 ? codecCtx_->frame_size : 1024;
    const size_t perFrame = static_cast<size_t>(frameSize) * options_.channels;
    if (!pending_.empty()) {
        const size_t totalFrames = ((pending_.size() + perFrame - 1) / perFrame) * frameSize;
        pending_.resize(totalFrames * options_.channels, 0.0f);
        const int64_t ptsUs =
            samplesConsumed_ * 1'000'000LL / options_.sampleRate;
        encodeBuffered(pending_.data(), static_cast<int>(totalFrames), ptsUs);
    }
    pending_.clear();
    avcodec_send_frame(codecCtx_, nullptr);
    drainPackets();
}

void AudioEncoder::close() {
    if (codecCtx_) {
        avcodec_free_context(&codecCtx_);
    }
    if (params_) {
        avcodec_parameters_free(&params_);
    }
    onPacket_ = nullptr;
    pending_.clear();
    samplesConsumed_ = 0;
    ptsOffsetUs_ = 0;
}

} // namespace nr
