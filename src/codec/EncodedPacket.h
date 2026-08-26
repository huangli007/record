#pragma once

#include <cstdint>
#include <vector>

namespace nr {

struct EncodedPacket {
    std::vector<uint8_t> data;
    int64_t ptsUs = 0;
    int64_t dtsUs = 0;
    int64_t durationUs = 0;
    bool keyframe = false;
    int streamIndex = 0;
};

} // namespace nr
