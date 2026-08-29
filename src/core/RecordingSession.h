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

    // Counters across the audio pipeline, for troubleshooting silent files.
    struct AudioDebugStats {
        long long sckAudioCallbacks = 0;   // SCK audio buffers delivered
        long long sckAudioEmptyDrops = 0;  // SCK buffers dropped by parser
        long long sckAudioSamples = 0;     // samples delivered by SCK
        long long systemAudioFrames = 0;   // frames entering the session
        long long tapFrames = 0;           // frames from the CoreAudio tap fallback
        bool tapActive = false;            // tap fallback is supplying audio
        long long mixPushed = 0;           // frames pushed to the encoder queue
        long long mixDropped = 0;          // frames dropped due to full queue
        long long encodedFrames = 0;       // frames accepted by the AAC encoder
        long long encodeFailures = 0;      // frames rejected by the encoder
        long long encodedPackets = 0;      // AAC packets produced
        long long muxedPackets = 0;        // AAC packets written to the file
        long long muxFailures = 0;         // packets the muxer rejected
        long long tapSetupErrors = 0;      // tap fallback setup failures
        std::string sckAudioError;         // SCK audio output registration error
    };
    AudioDebugStats audioDebugStats() const;

private:
    void onVideoFrame(VideoFrame frame);
    void onSystemAudio(AudioFrame frame);
    void onTapSystemAudio(AudioFrame frame);
    void onMicAudio(AudioFrame frame);
    void onVideoPacket(EncodedPacket packet);
    void onAudioPacket(EncodedPacket packet);

    void audioMixLoop();
    void encodeLoop();
    void muxLoop();
    void ensureMuxHeader();
    void writeAudioDebugLog();
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
    std::unique_ptr<AudioCapturer> tapCapturer_;
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

    std::atomic<long long> systemAudioFrames_{0};
    std::atomic<long long> mixPushed_{0};
    std::atomic<long long> mixDropped_{0};
    std::atomic<long long> encodedFrames_{0};
    std::atomic<long long> encodeFailures_{0};
    std::atomic<long long> encodedPackets_{0};
    std::atomic<long long> muxedPackets_{0};
    std::atomic<long long> muxFailures_{0};
    std::atomic<bool> tapAudioActive_{false};
    std::atomic<bool> tapStarted_{false};
    std::atomic<long long> tapFrames_{0};
};

} // namespace nr
