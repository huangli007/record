#include "codec/VideoEncoder.h"

#include <cstring>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#if defined(__APPLE__)
#include <CoreVideo/CoreVideo.h>
#endif

namespace nr {

namespace {

#if defined(__APPLE__)
void releasePixelBuffer(void*, uint8_t* data) {
    CVPixelBufferRef pb = reinterpret_cast<CVPixelBufferRef>(data);
    CVPixelBufferRelease(pb);
}

// Maps a CVPixelBuffer's pixel format to an AV pixel format for swscale.
AVPixelFormat mapPixelFormat(OSType format) {
    switch (format) {
        case kCVPixelFormatType_32BGRA:
            return AV_PIX_FMT_BGRA;
        case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
        case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
            return AV_PIX_FMT_NV12;
        case kCVPixelFormatType_32ARGB:
            return AV_PIX_FMT_ARGB;
        default:
            return AV_PIX_FMT_NONE;
    }
}

bool pixelBufferPlanes(CVPixelBufferRef pb, AVFrame* frame) {
    const OSType format = CVPixelBufferGetPixelFormatType(pb);
    const AVPixelFormat avFormat = mapPixelFormat(format);
    if (avFormat == AV_PIX_FMT_NONE) {
        return false;
    }
    frame->format = avFormat;
    frame->width = static_cast<int>(CVPixelBufferGetWidth(pb));
    frame->height = static_cast<int>(CVPixelBufferGetHeight(pb));
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    const size_t planeCount = CVPixelBufferGetPlaneCount(pb);
    if (planeCount == 0) {
        frame->data[0] = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(pb));
        frame->linesize[0] = static_cast<int>(CVPixelBufferGetBytesPerRow(pb));
    } else {
        for (size_t i = 0; i < planeCount && i < 4; ++i) {
            frame->data[i] = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, i));
            frame->linesize[i] = static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(pb, i));
        }
    }
    return true;
}

void unlockPixelBuffer(CVPixelBufferRef pb) {
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
}

// Scales a BGRA CVPixelBuffer to an NV12 CVPixelBuffer of the given size.
// Used when the captured frame size differs from the encoder size (window
// resizes, display resolution changes).
bool scalePixelBufferToNV12(CVPixelBufferRef src, int outWidth, int outHeight,
                            CVPixelBufferRef* out) {
    if (!src || outWidth <= 0 || outHeight <= 0) {
        return false;
    }
    CVPixelBufferRef dst = nullptr;
    const CVReturn created = CVPixelBufferCreate(
        kCFAllocatorDefault, outWidth, outHeight,
        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, nullptr, &dst);
    if (created != kCVReturnSuccess || !dst) {
        return false;
    }

    CVPixelBufferLockBaseAddress(src, kCVPixelBufferLock_ReadOnly);
    CVPixelBufferLockBaseAddress(dst, 0);

    const uint8_t* srcData[4] = {
        static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(src)), nullptr, nullptr, nullptr};
    const int srcStride[4] = {
        static_cast<int>(CVPixelBufferGetBytesPerRow(src)), 0, 0, 0};
    uint8_t* dstData[4] = {
        static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(dst, 0)),
        static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(dst, 1)),
        nullptr, nullptr};
    const int dstStride[4] = {
        static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(dst, 0)),
        static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(dst, 1)),
        0, 0};

    SwsContext* sws = sws_getContext(
        static_cast<int>(CVPixelBufferGetWidth(src)),
        static_cast<int>(CVPixelBufferGetHeight(src)),
        AV_PIX_FMT_BGRA, outWidth, outHeight, AV_PIX_FMT_NV12,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (sws) {
        sws_scale(sws, srcData, srcStride, 0,
                  static_cast<int>(CVPixelBufferGetHeight(src)),
                  dstData, dstStride);
        sws_freeContext(sws);
    }

    CVPixelBufferUnlockBaseAddress(dst, 0);
    CVPixelBufferUnlockBaseAddress(src, kCVPixelBufferLock_ReadOnly);

    if (!sws) {
        CVPixelBufferRelease(dst);
        return false;
    }
    *out = dst;
    return true;
}
#endif

} // namespace

VideoEncoder::~VideoEncoder() {
    close();
}

bool VideoEncoder::open(const Options& options, PacketCallback onPacket) {
    close();
    options_ = options;
    onPacket_ = std::move(onPacket);
    codecName_ = "none";

    const std::string& codec = options.codec;
    const bool wantH265 = codec == "h265";

    if (codec == "auto" || codec == "h264" || codec == "h265") {
        if (wantH265) {
#if defined(__APPLE__)
            if (openHardware("hevc_videotoolbox")) {
                return true;
            }
#else
            if (openHardware("hevc_nvenc")) {
                return true;
            }
            if (openHardware("hevc_qsv")) {
                return true;
            }
            if (openHardware("hevc_amf")) {
                return true;
            }
#endif
            if (openSoftware("libx265")) {
                return true;
            }
        } else {
#if defined(__APPLE__)
            if (openHardware("h264_videotoolbox")) {
                return true;
            }
#else
            if (openHardware("h264_nvenc")) {
                return true;
            }
            if (openHardware("h264_qsv")) {
                return true;
            }
            if (openHardware("h264_amf")) {
                return true;
            }
#endif
            if (openSoftware("libx264")) {
                return true;
            }
        }
    }
    return false;
}

bool VideoEncoder::openHardware(const std::string& encoderName) {
    const AVCodec* codec = avcodec_find_encoder_by_name(encoderName.c_str());
    if (!codec) {
        return false;
    }
    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        return false;
    }
    codecCtx_->width = options_.width;
    codecCtx_->height = options_.height;
    codecCtx_->time_base = AVRational{1, options_.fps};
    codecCtx_->framerate = AVRational{options_.fps, 1};
    codecCtx_->gop_size = options_.fps * 2;
    codecCtx_->max_b_frames = 0;
    codecCtx_->thread_count = 0;
#if defined(__APPLE__)
    codecCtx_->pix_fmt = AV_PIX_FMT_VIDEOTOOLBOX;
#else
    codecCtx_->pix_fmt = AV_PIX_FMT_NV12;
#endif
    codecCtx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    codecCtx_->bit_rate = static_cast<int64_t>(options_.bitrateKbps) * 1000;
    if (options_.bitrateMode == BitrateMode::FileSize) {
        codecCtx_->rc_min_rate = codecCtx_->bit_rate;
        codecCtx_->rc_max_rate = codecCtx_->bit_rate;
        codecCtx_->rc_buffer_size = codecCtx_->bit_rate / 2;
    }
    av_opt_set(codecCtx_->priv_data, "realtime", "1", 0);

    if (avcodec_open2(codecCtx_, codec, nullptr) < 0) {
        avcodec_free_context(&codecCtx_);
        return false;
    }
    hardware_ = true;
    codecName_ = encoderName;
    params_ = avcodec_parameters_alloc();
    avcodec_parameters_from_context(params_, codecCtx_);
    return true;
}

bool VideoEncoder::openSoftware(const std::string& encoderName) {
    const AVCodec* codec = avcodec_find_encoder_by_name(encoderName.c_str());
    if (!codec) {
        return false;
    }
    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        return false;
    }
    codecCtx_->width = options_.width;
    codecCtx_->height = options_.height;
    codecCtx_->time_base = AVRational{1, options_.fps};
    codecCtx_->framerate = AVRational{options_.fps, 1};
    codecCtx_->gop_size = options_.fps * 2;
    codecCtx_->max_b_frames = 0;
    codecCtx_->thread_count = 0;
    codecCtx_->pix_fmt = AV_PIX_FMT_YUV420P;
    codecCtx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (options_.bitrateMode == BitrateMode::FileSize) {
        codecCtx_->bit_rate = static_cast<int64_t>(options_.bitrateKbps) * 1000;
        codecCtx_->rc_min_rate = codecCtx_->bit_rate;
        codecCtx_->rc_max_rate = codecCtx_->bit_rate;
        codecCtx_->rc_buffer_size = codecCtx_->bit_rate / 2;
    } else {
        av_opt_set_int(codecCtx_->priv_data, "crf", options_.crf, 0);
    }
    av_opt_set(codecCtx_->priv_data, "preset", options_.preset.c_str(), 0);

    if (avcodec_open2(codecCtx_, codec, nullptr) < 0) {
        avcodec_free_context(&codecCtx_);
        return false;
    }
    hardware_ = false;
    codecName_ = encoderName;
    params_ = avcodec_parameters_alloc();
    avcodec_parameters_from_context(params_, codecCtx_);
    return true;
}

bool VideoEncoder::encode(VideoFrame&& frame) {
#if defined(__APPLE__)
    if (!codecCtx_ || !frame.pixelBuffer) {
        return false;
    }
#else
    if (!codecCtx_ || frame.bgra.empty()) {
        return false;
    }
#endif

    AVFrame* avFrame = av_frame_alloc();
    if (!avFrame) {
        return false;
    }
    avFrame->pts = frameIndex_++;

#if defined(__APPLE__)
    if (hardware_) {
        CVPixelBufferRef pb = frame.pixelBuffer;
        CVPixelBufferRef scaled = nullptr;
        const int srcW = static_cast<int>(CVPixelBufferGetWidth(pb));
        const int srcH = static_cast<int>(CVPixelBufferGetHeight(pb));
        if (srcW != options_.width || srcH != options_.height) {
            // Window resized / display changed: scale into a fixed-size
            // NV12 pixel buffer before handing it to VideoToolbox.
            if (!scalePixelBufferToNV12(pb, options_.width, options_.height, &scaled)) {
                av_frame_free(&avFrame);
                return false;
            }
            pb = scaled;
        }
        CVPixelBufferRetain(pb);
        AVBufferRef* buffer = av_buffer_create(
            reinterpret_cast<uint8_t*>(pb), 1, releasePixelBuffer, nullptr, 0);
        avFrame->format = AV_PIX_FMT_VIDEOTOOLBOX;
        avFrame->width = options_.width;
        avFrame->height = options_.height;
        // FFmpeg >= 9 reads the CVPixelBuffer from data[3] for
        // AV_PIX_FMT_VIDEOTOOLBOX frames; keep data[0] for older versions.
        avFrame->data[0] = reinterpret_cast<uint8_t*>(pb);
        avFrame->data[3] = reinterpret_cast<uint8_t*>(pb);
        avFrame->linesize[0] = 0;
        avFrame->buf[0] = buffer;
        if (scaled) {
            // The wrapping buffer now owns the only reference we keep.
            CVPixelBufferRelease(scaled);
        }
    } else {
        // Software path: convert CVPixelBuffer -> YUV420P via swscale.
        CVPixelBufferRef pb = frame.pixelBuffer;
        if (!pixelBufferPlanes(pb, avFrame)) {
            av_frame_free(&avFrame);
            return false;
        }
        SwsContext* sws = sws_getContext(
            avFrame->width, avFrame->height,
            static_cast<AVPixelFormat>(avFrame->format),
            options_.width, options_.height, AV_PIX_FMT_YUV420P,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) {
            unlockPixelBuffer(pb);
            av_frame_free(&avFrame);
            return false;
        }
        AVFrame* yuv = av_frame_alloc();
        yuv->format = AV_PIX_FMT_YUV420P;
        yuv->width = options_.width;
        yuv->height = options_.height;
        yuv->pts = avFrame->pts;
        av_frame_get_buffer(yuv, 32);
        sws_scale(sws, avFrame->data, avFrame->linesize, 0, avFrame->height,
                  yuv->data, yuv->linesize);
        unlockPixelBuffer(pb);
        av_frame_free(&avFrame);
        avFrame = yuv;
    }
#else
    // Windows path: raw BGRA rows -> NV12 (hardware) or YUV420P (software).
    if (frame.bgra.empty() || frame.height <= 0) {
        av_frame_free(&avFrame);
        return false;
    }
    const int srcStride =
        frame.stride > 0 ? frame.stride
                         : static_cast<int>(frame.bgra.size()) / frame.height;
    const AVPixelFormat dstFmt =
        static_cast<AVPixelFormat>(codecCtx_->pix_fmt);
    SwsContext* sws = sws_getContext(
        frame.width, frame.height, AV_PIX_FMT_BGRA,
        options_.width, options_.height, dstFmt,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        av_frame_free(&avFrame);
        return false;
    }
    AVFrame* converted = av_frame_alloc();
    converted->format = dstFmt;
    converted->width = options_.width;
    converted->height = options_.height;
    converted->pts = avFrame->pts;
    av_frame_get_buffer(converted, 32);

    const uint8_t* srcData[4] = {frame.bgra.data(), nullptr, nullptr, nullptr};
    const int srcLinesize[4] = {srcStride, 0, 0, 0};
    sws_scale(sws, srcData, srcLinesize, 0, frame.height,
              converted->data, converted->linesize);
    sws_freeContext(sws);
    av_frame_free(&avFrame);
    avFrame = converted;
#endif

    const int ret = avcodec_send_frame(codecCtx_, avFrame);
    av_frame_free(&avFrame);
    if (ret < 0) {
        return false;
    }
    drainPackets();
    return true;
}

void VideoEncoder::drainPackets() {
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

void VideoEncoder::flush() {
    if (!codecCtx_) {
        return;
    }
    avcodec_send_frame(codecCtx_, nullptr);
    drainPackets();
}

void VideoEncoder::close() {
    if (codecCtx_) {
        avcodec_free_context(&codecCtx_);
    }
    if (params_) {
        avcodec_parameters_free(&params_);
    }
    onPacket_ = nullptr;
    frameIndex_ = 0;
    hardware_ = false;
    codecName_ = "none";
}

void VideoEncoder::refreshParameters() {
    if (codecCtx_ && params_) {
        avcodec_parameters_from_context(params_, codecCtx_);
    }
}

} // namespace nr
