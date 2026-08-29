#pragma once

#include <cstdint>
#include <vector>

#if defined(__APPLE__)
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#endif

namespace nr {

// A single raw video frame. On macOS it wraps a retained CVPixelBuffer; on
// other platforms (Windows) it carries a BGRA byte buffer.
struct VideoFrame {
#if defined(__APPLE__)
    CVPixelBufferRef pixelBuffer = nullptr;
#else
    std::vector<uint8_t> bgra;  // BGRA rows, tightly packed
    int stride = 0;             // bytes per row (bgra.size() / height)
#endif
    int64_t ptsUs = 0;        // microsecond timeline (TimeBase)
    double durationUs = 0;    // expected frame duration
    int width = 0;
    int height = 0;

#if defined(__APPLE__)
    VideoFrame() = default;
    explicit VideoFrame(CVPixelBufferRef buffer)
        : pixelBuffer(buffer) {
        if (pixelBuffer) {
            CVPixelBufferRetain(pixelBuffer);
        }
    }
    VideoFrame(const VideoFrame& other)
        : pixelBuffer(other.pixelBuffer),
          ptsUs(other.ptsUs),
          durationUs(other.durationUs),
          width(other.width),
          height(other.height) {
        if (pixelBuffer) {
            CVPixelBufferRetain(pixelBuffer);
        }
    }
    VideoFrame(VideoFrame&& other) noexcept
        : pixelBuffer(other.pixelBuffer),
          ptsUs(other.ptsUs),
          durationUs(other.durationUs),
          width(other.width),
          height(other.height) {
        other.pixelBuffer = nullptr;
    }
    VideoFrame& operator=(const VideoFrame& other) {
        if (this != &other) {
            if (pixelBuffer) {
                CVPixelBufferRelease(pixelBuffer);
            }
            pixelBuffer = other.pixelBuffer;
            if (pixelBuffer) {
                CVPixelBufferRetain(pixelBuffer);
            }
            ptsUs = other.ptsUs;
            durationUs = other.durationUs;
            width = other.width;
            height = other.height;
        }
        return *this;
    }
    VideoFrame& operator=(VideoFrame&& other) noexcept {
        if (this != &other) {
            if (pixelBuffer) {
                CVPixelBufferRelease(pixelBuffer);
            }
            pixelBuffer = other.pixelBuffer;
            other.pixelBuffer = nullptr;
            ptsUs = other.ptsUs;
            durationUs = other.durationUs;
            width = other.width;
            height = other.height;
        }
        return *this;
    }
    ~VideoFrame() {
        if (pixelBuffer) {
            CVPixelBufferRelease(pixelBuffer);
        }
    }
#endif
};

} // namespace nr
