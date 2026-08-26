#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "codec/AudioEncoder.h"
#include "codec/VideoEncoder.h"
#include "mux/Muxer.h"

extern "C" {
#include <libavutil/avutil.h>
}

#include <CoreVideo/CoreVideo.h>

using namespace nr;

namespace {

CVPixelBufferRef makeTestBuffer(int width, int height, int frameIndex) {
    CVPixelBufferRef pb = nullptr;
    const OSType format = kCVPixelFormatType_32BGRA;
    CVPixelBufferCreate(kCFAllocatorDefault, width, height, format, nullptr, &pb);
    CVPixelBufferLockBaseAddress(pb, 0);
    uint8_t* base = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(pb));
    const size_t bytesPerRow = CVPixelBufferGetBytesPerRow(pb);
    const float t = static_cast<float>(frameIndex) / 30.0f;
    for (int y = 0; y < height; ++y) {
        uint8_t* row = base + y * bytesPerRow;
        for (int x = 0; x < width; ++x) {
            const float nx = static_cast<float>(x) / width;
            const float ny = static_cast<float>(y) / height;
            const uint8_t b = static_cast<uint8_t>(128 + 120 * std::sin(nx * 6.28f + t));
            const uint8_t g = static_cast<uint8_t>(128 + 120 * std::sin(ny * 6.28f + t * 2));
            const uint8_t r = static_cast<uint8_t>(128 + 120 * std::sin((nx + ny) * 4.18f + t));
            row[x * 4 + 0] = b;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = r;
            row[x * 4 + 3] = 255;
        }
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    return pb;
}

} // namespace

int main(int argc, char** argv) {
    const std::string outPath = argc > 1 ? argv[1] : "/tmp/nr_pipeline_test.mp4";
    const int width = 640;
    const int height = 360;
    const int fps = 30;
    const int durationSeconds = 3;

    VideoEncoder videoEncoder;
    AudioEncoder audioEncoder;
    Muxer muxer;

    const int totalFrames = fps * durationSeconds;

    if (!videoEncoder.open(
            VideoEncoder::Options{width, height, fps, "auto",
                                  BitrateMode::FileSize, 1500, 18, "medium"},
            [&](EncodedPacket packet) {
                videoEncoder.refreshParameters();
                if (!muxer.headerWritten()) {
                    muxer.updateStreamParameters(0, videoEncoder.codecParameters());
                    muxer.updateStreamParameters(1, audioEncoder.codecParameters());
                    muxer.writeHeader();
                }
                packet.streamIndex = 0;
                muxer.write(packet);
            })) {
        std::fprintf(stderr, "video encoder open failed\n");
        return 1;
    }

    if (!audioEncoder.open(
            AudioEncoder::Options{48000, 2, 128},
            [&](EncodedPacket packet) {
                if (!muxer.headerWritten()) {
                    videoEncoder.refreshParameters();
                    muxer.updateStreamParameters(0, videoEncoder.codecParameters());
                    muxer.updateStreamParameters(1, audioEncoder.codecParameters());
                    muxer.writeHeader();
                }
                packet.streamIndex = 1;
                muxer.write(packet);
            })) {
        std::fprintf(stderr, "audio encoder open failed\n");
        return 1;
    }

    if (!muxer.open(outPath, "mp4", videoEncoder.codecParameters(),
                    audioEncoder.codecParameters(), fps, 48000)) {
        std::fprintf(stderr, "muxer open failed\n");
        return 1;
    }

    for (int i = 0; i < totalFrames; ++i) {
        CVPixelBufferRef pb = makeTestBuffer(width, height, i);
        VideoFrame frame(pb);
        frame.ptsUs = static_cast<int64_t>(i) * 1'000'000 / fps;
        frame.width = width;
        frame.height = height;
        videoEncoder.encode(std::move(frame));
        CVPixelBufferRelease(pb);
        if (i == 0) {
            const AVCodecParameters* par = videoEncoder.codecParameters();
            std::printf("DEBUG after first encode: w=%d h=%d extradata=%d codec=%s\n",
                        par->width, par->height,
                        par->extradata_size ? par->extradata_size : 0,
                        videoEncoder.codecName());
        }
    }

    // 3 seconds of 440 Hz sine, stereo Float32 interleaved, 1024-sample chunks.
    constexpr int sampleRate = 48000;
    constexpr int chunk = 1024;
    const int totalChunks = sampleRate * durationSeconds / chunk;
    double phase = 0.0;
    for (int c = 0; c < totalChunks; ++c) {
        AudioFrame frame;
        frame.sampleRate = sampleRate;
        frame.channels = 2;
        frame.source = AudioSource::System;
        frame.ptsUs = static_cast<int64_t>(c) * chunk * 1'000'000 / sampleRate;
        frame.durationUs = chunk * 1'000'000 / sampleRate;
        frame.samples.resize(chunk * 2);
        for (int i = 0; i < chunk; ++i) {
            const float s = 0.25f * static_cast<float>(std::sin(phase));
            frame.samples[i * 2 + 0] = s;
            frame.samples[i * 2 + 1] = s;
            phase += 2.0 * M_PI * 440.0 / sampleRate;
        }
        audioEncoder.encode(frame);
    }

    videoEncoder.flush();
    audioEncoder.flush();
    muxer.close();

    std::error_code ec;
    const auto size = std::filesystem::file_size(outPath, ec);
    if (ec || size == 0) {
        std::fprintf(stderr, "output file missing or empty\n");
        return 1;
    }
    std::printf("OK: %s (%zu bytes, %d video frames, %d audio chunks)\n",
                outPath.c_str(), static_cast<size_t>(size), totalFrames, totalChunks);
    return 0;
}
