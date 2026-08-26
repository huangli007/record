#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "capture/AudioCapturer.h"
#include "capture/AudioFrame.h"
#include "capture/ScreenCapturer.h"
#include "capture/VideoFrame.h"
#include "codec/AudioEncoder.h"
#include "codec/EncodedPacket.h"
#include "codec/VideoEncoder.h"
#include "core/Config.h"
#include "core/ThreadSafeQueue.h"
#include "mux/Muxer.h"

namespace nr {

// Orchestrates the producer-consumer recording pipeline:
//   capture producers -> raw queues -> encoder thread -> packet queues
//   -> mux thread -> file
// The state machine (control thread) is driven from the UI thread.
class RecordingSession {
public:
    enum class State {
        Idle,
        Starting,
        Recording,
        Paused,
        Stopping,
        Error,
    };

    using StateCallback = std::function<void(State)>;

    RecordingSession();
    ~RecordingSession();

    RecordingSession(const RecordingSession&) = delete;
    RecordingSession& operator=(const RecordingSession&) = delete;

    bool start(const RecordingConfig& config, StateCallback onState);
    void stop();
    void pause();
    void resume();

    State state() const;
    int64_t elapsedUs() const;
    std::string lastError() const;
    std::string outputPath() const;
    std::string encoderName() const;
    size_t videoQueueDepth() const;
    size_t encodedQueueDepth() const;

private:
    void onVideoFrame(VideoFrame frame);
    void onSystemAudio(AudioFrame frame);
    void onMicAudio(AudioFrame frame);
    void onVideoPacket(EncodedPacket packet);
    void onAudioPacket(EncodedPacket packet);

    void audioMixLoop();
    void encodeLoop();
    void muxLoop();
    void ensureMuxHeader();
    void setState(State state);
    void cleanupQueues();

    RecordingConfig config_;
    State state_ = State::Idle;
    mutable std::mutex stateMutex_;
    StateCallback onState_;
    std::string lastError_;
    std::string outputPath_;

    std::unique_ptr<ScreenCapturer> screenCapturer_;
    std::unique_ptr<AudioCapturer> micCapturer_;
    std::unique_ptr<VideoEncoder> videoEncoder_;
    std::unique_ptr<AudioEncoder> audioEncoder_;
    std::unique_ptr<Muxer> muxer_;

    ThreadSafeQueue<VideoFrame> videoQueue_{16};
    ThreadSafeQueue<AudioFrame> systemAudioQueue_{48};
    ThreadSafeQueue<AudioFrame> micAudioQueue_{48};
    ThreadSafeQueue<AudioFrame> audioQueue_{48};
    ThreadSafeQueue<EncodedPacket> videoPacketQueue_{128};
    ThreadSafeQueue<EncodedPacket> audioPacketQueue_{128};

    std::thread audioMixThread_;
    std::thread encoderThread_;
    std::thread muxThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};

    int64_t startUs_ = 0;
    int64_t pausedTotalUs_ = 0;
    int64_t pausedAtUs_ = 0;
    bool hasAudio_ = false;
};

} // namespace nr
