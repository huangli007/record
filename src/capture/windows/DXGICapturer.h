#pragma once

#include <memory>
#include <vector>

#include "capture/ScreenCapturer.h"

namespace nr {

// Windows screen capturer. Uses DXGI Desktop Duplication for the primary
// display (works with hardware-accelerated content) and falls back to GDI
// BitBlt in environments where duplication is unavailable (RDP, VMs).
class DXGICapturer final : public ScreenCapturer {
public:
    DXGICapturer();
    ~DXGICapturer() override;

    bool start(const VideoConfig& videoConfig,
               const AudioConfig& audioConfig,
               VideoFrameCallback onFrame,
               AudioFrameCallback onAudio) override;
    void stop() override;
    void setPaused(bool paused) override;

    static std::vector<WindowInfo> listWindows();
    static double displayScale();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nr
