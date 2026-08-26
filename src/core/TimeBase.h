#pragma once

#include <chrono>

namespace nr {

// Unified high-resolution monotonic clock used by all capture / encode / mux
// modules so every frame carries a comparable timestamp (microseconds).
class TimeBase {
public:
    using Duration = std::chrono::microseconds;

    static Duration now() {
        return std::chrono::duration_cast<Duration>(
            std::chrono::steady_clock::now().time_since_epoch());
    }

    static Duration since(Duration start) { return now() - start; }

    // Convert seconds (e.g. from CMTime) to our microsecond timeline.
    static Duration fromSeconds(double seconds) {
        return Duration(static_cast<int64_t>(seconds * 1'000'000.0));
    }
};

} // namespace nr
