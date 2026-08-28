#include "core/RecordingSession.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>

#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#endif

#include "capture/macos/CoreAudioCapturer.h"
#include "capture/macos/ScreenCaptureKitCapturer.h"
#include "codec/EncoderFactory.h"
#include "core/TimeBase.h"

namespace nr {

namespace {

std::string expandPath(const std::string& path) {
    std::string result = path;
#if !defined(_WIN32)
    if (!result.empty() && result[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) {
            result.replace(0, 1, home);
        }
    }
#endif
    return result;
}

// Sums two interleaved Float32 frames (same rate/channels) and clamps to
// [-1, 1]. Returns a frame covering the common duration.
AudioFrame mixFrames(const AudioFrame& primary, const AudioFrame& secondary) {
    AudioFrame out = primary;
    if (primary.sampleRate != secondary.sampleRate ||
        primary.channels != secondary.channels) {
        return out;
    }
    const size_t common = std::min(primary.samples.size(), secondary.samples.size());
    for (size_t i = 0; i < common; ++i) {
        out.samples[i] =
            std::clamp(primary.samples[i] + secondary.samples[i], -1.0f, 1.0f);
    }
    return out;
}

} // namespace

RecordingSession::RecordingSession() = default;

RecordingSession::~RecordingSession() {
    const State s = state();
    if (s == State::Recording || s == State::Paused) {
        stop();
    }
}

bool RecordingSession::start(const RecordingConfig& config, StateCallback onState) {
    if (state() != State::Idle) {
        return false;
    }
    setState(State::Starting);
    config_ = config;
    onState_ = std::move(onState);
    lastError_.clear();
    pausedTotalUs_ = 0;
    pausedAtUs_ = 0;
    if (config_.video.fps <= 0) {
        config_.video.fps = 60;
    }

#if defined(__APPLE__)
    if (config_.video.mode == CaptureMode::Region && config_.video.region.valid()) {
        // Region is expressed in points; encode at Retina pixel resolution.
        const double scale = ScreenCaptureKitCapturer::displayScale();
        config_.video.width = static_cast<int>(config_.video.region.width * scale);
        config_.video.height = static_cast<int>(config_.video.region.height * scale);
    } else if (config_.video.mode == CaptureMode::FullScreen &&
               (config_.video.width <= 0 || config_.video.height <= 0)) {
        const CGRect bounds = CGDisplayBounds(CGMainDisplayID());
        config_.video.width = static_cast<int>(bounds.size.width);
        config_.video.height = static_cast<int>(bounds.size.height);
    }
    // CaptureMode::Window keeps config_.video.width/height as set by the
    // window picker (the capturer re-verifies the window at start).
#endif
    if (config_.video.width <= 0 || config_.video.height <= 0) {
        config_.video.width = 1920;
        config_.video.height = 1080;
    }

    const std::string dir = expandPath(config_.general.outputDir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    outputPath_ = (std::filesystem::path(dir) / config_.defaultFileName()).string();

    screenCapturer_ = std::make_unique<ScreenCaptureKitCapturer>();
    micCapturer_ =
        config_.audio.captureMicrophone ? std::make_unique<CoreAudioCapturer>() : nullptr;

    videoEncoder_ = EncoderFactory::createVideoEncoder(config_.video);
    if (!videoEncoder_->open(
            VideoEncoder::Options{config_.video.width, config_.video.height,
                                  config_.video.fps, config_.video.codec,
                                  config_.video.bitrateMode, config_.video.bitrateKbps,
                                  config_.video.crf, config_.video.preset},
            [this](EncodedPacket p) { onVideoPacket(std::move(p)); })) {
        lastError_ = "无法初始化视频编码器（请确认 FFmpeg 已安装且编码器可用）";
        setState(State::Error);
        return false;
    }

    hasAudio_ = config_.audio.captureSystemAudio || config_.audio.captureMicrophone;
    if (hasAudio_) {
        audioEncoder_ = EncoderFactory::createAudioEncoder(config_.audio);
        if (!audioEncoder_->open(
                AudioEncoder::Options{config_.audio.sampleRate, config_.audio.channels, 192},
                [this](EncodedPacket p) { onAudioPacket(std::move(p)); })) {
            lastError_ = "无法初始化音频编码器（AAC）";
            setState(State::Error);
            return false;
        }
    }

    muxer_ = std::make_unique<Muxer>();
    if (!muxer_->open(outputPath_, config_.general.format,
                      videoEncoder_->codecParameters(),
                      hasAudio_ ? audioEncoder_->codecParameters() : nullptr,
                      config_.video.fps,
                      hasAudio_ ? config_.audio.sampleRate : 0)) {
        lastError_ = "无法打开输出文件：" + outputPath_;
        setState(State::Error);
        return false;
    }

    running_.store(true);
    paused_.store(false);
    // Start the mixer whenever any audio source is enabled. Without it,
    // system-audio frames would pile up unconsumed and the file would be
    // silent (default config: system audio on, mic off).
    if (hasAudio_) {
        audioMixThread_ = std::thread([this] { audioMixLoop(); });
    }
    encoderThread_ = std::thread([this] { encodeLoop(); });
    muxThread_ = std::thread([this] { muxLoop(); });

    if (!screenCapturer_->start(config_.video, config_.audio,
                                [this](VideoFrame f) { onVideoFrame(std::move(f)); })) {
        lastError_ = "无法启动屏幕采集，请检查“屏幕录制”权限（系统设置 → 隐私与安全性）";
        running_.store(false);
        cleanupQueues();
        setState(State::Error);
        return false;
    }
    screenCapturer_->setSystemAudioCallback(
        [this](AudioFrame f) { onSystemAudio(std::move(f)); });

    if (micCapturer_) {
        if (!micCapturer_->start(config_.audio,
                                 [this](AudioFrame f) { onMicAudio(std::move(f)); })) {
            lastError_ = "无法启动麦克风采集，请检查“麦克风”权限";
            running_.store(false);
            screenCapturer_->stop();
            cleanupQueues();
            setState(State::Error);
            return false;
        }
    }

    startUs_ = TimeBase::now().count();
    setState(State::Recording);
    return true;
}

void RecordingSession::stop() {
    const State current = state();
    if (current != State::Recording && current != State::Paused &&
        current != State::Starting && current != State::Error) {
        return;
    }
    setState(State::Stopping);
    running_.store(false);
    paused_.store(false);

    if (screenCapturer_) {
        screenCapturer_->stop();
    }
    if (micCapturer_) {
        micCapturer_->stop();
    }

    videoQueue_.close();
    systemAudioQueue_.close();
    micAudioQueue_.close();
    if (audioMixThread_.joinable()) {
        audioMixThread_.join();
    }
    audioQueue_.close();

    if (encoderThread_.joinable()) {
        encoderThread_.join();
    }
    if (videoEncoder_) {
        videoEncoder_->flush();
    }
    if (audioEncoder_) {
        audioEncoder_->flush();
    }
    videoPacketQueue_.close();
    audioPacketQueue_.close();
    if (muxThread_.joinable()) {
        muxThread_.join();
    }
    if (muxer_) {
        muxer_->close();
    }

    videoEncoder_.reset();
    audioEncoder_.reset();
    muxer_.reset();
    screenCapturer_.reset();
    micCapturer_.reset();
    setState(State::Idle);
    outputPath_.clear();
}

void RecordingSession::pause() {
    if (state() != State::Recording) {
        return;
    }
    paused_.store(true);
    if (screenCapturer_) {
        screenCapturer_->setPaused(true);
    }
    if (micCapturer_) {
        micCapturer_->setPaused(true);
    }
    pausedAtUs_ = TimeBase::now().count();
    setState(State::Paused);
}

void RecordingSession::resume() {
    if (state() != State::Paused) {
        return;
    }
    paused_.store(false);
    if (screenCapturer_) {
        screenCapturer_->setPaused(false);
    }
    if (micCapturer_) {
        micCapturer_->setPaused(false);
    }
    pausedTotalUs_ += TimeBase::now().count() - pausedAtUs_;
    pausedAtUs_ = 0;
    setState(State::Recording);
}

RecordingSession::State RecordingSession::state() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return state_;
}

int64_t RecordingSession::elapsedUs() const {
    const int64_t now = TimeBase::now().count();
    switch (state()) {
        case State::Recording:
            return now - startUs_ - pausedTotalUs_;
        case State::Paused:
            return pausedAtUs_ - startUs_ - pausedTotalUs_;
        default:
            return 0;
    }
}

std::string RecordingSession::lastError() const {
    return lastError_;
}

std::string RecordingSession::outputPath() const {
    return outputPath_;
}

std::string RecordingSession::encoderName() const {
    return videoEncoder_ ? videoEncoder_->codecName() : "";
}

size_t RecordingSession::videoQueueDepth() const {
    return videoQueue_.size();
}

size_t RecordingSession::encodedQueueDepth() const {
    return videoPacketQueue_.size() + audioPacketQueue_.size();
}

void RecordingSession::onVideoFrame(VideoFrame frame) {
    if (paused_.load()) {
        return;
    }
    videoQueue_.push(std::move(frame), std::chrono::milliseconds(10));
}

void RecordingSession::onSystemAudio(AudioFrame frame) {
    if (paused_.load()) {
        return;
    }
    systemAudioQueue_.push(std::move(frame), std::chrono::milliseconds(10));
}

void RecordingSession::onMicAudio(AudioFrame frame) {
    if (paused_.load()) {
        return;
    }
    micAudioQueue_.push(std::move(frame), std::chrono::milliseconds(10));
}

void RecordingSession::onVideoPacket(EncodedPacket packet) {
    ensureMuxHeader();
    packet.streamIndex = 0;
    videoPacketQueue_.push(std::move(packet), std::chrono::milliseconds(50));
}

void RecordingSession::onAudioPacket(EncodedPacket packet) {
    ensureMuxHeader();
    packet.streamIndex = 1;
    audioPacketQueue_.push(std::move(packet), std::chrono::milliseconds(50));
}

void RecordingSession::ensureMuxHeader() {
    if (!muxer_ || muxer_->headerWritten()) {
        return;
    }
    if (videoEncoder_) {
        videoEncoder_->refreshParameters();
        muxer_->updateStreamParameters(0, videoEncoder_->codecParameters());
    }
    if (hasAudio_ && audioEncoder_) {
        muxer_->updateStreamParameters(1, audioEncoder_->codecParameters());
    }
    muxer_->writeHeader();
}

void RecordingSession::audioMixLoop() {
    while (running_.load() ||
           !systemAudioQueue_.isClosed() || !micAudioQueue_.isClosed()) {
        auto sys = systemAudioQueue_.pop(std::chrono::milliseconds(20));
        auto mic = micAudioQueue_.pop(std::chrono::milliseconds(0));

        if (sys && mic) {
            if (std::llabs(sys->ptsUs - mic->ptsUs) < 40'000) {
                audioQueue_.push(mixFrames(*sys, *mic), std::chrono::milliseconds(20));
            } else if (sys->ptsUs < mic->ptsUs) {
                audioQueue_.push(std::move(*sys), std::chrono::milliseconds(20));
                micAudioQueue_.unshift(std::move(*mic));
            } else {
                audioQueue_.push(std::move(*mic), std::chrono::milliseconds(20));
                systemAudioQueue_.unshift(std::move(*sys));
            }
        } else if (sys) {
            audioQueue_.push(std::move(*sys), std::chrono::milliseconds(20));
        } else if (mic) {
            audioQueue_.push(std::move(*mic), std::chrono::milliseconds(20));
        }
    }
}

void RecordingSession::encodeLoop() {
    while (running_.load() ||
           !videoQueue_.isClosed() || !audioQueue_.isClosed()) {
        auto video = videoQueue_.pop(std::chrono::milliseconds(5));
        if (video && videoEncoder_) {
            videoEncoder_->encode(std::move(*video));
        }
        auto audio = audioQueue_.pop(std::chrono::milliseconds(0));
        if (audio && audioEncoder_) {
            audioEncoder_->encode(*audio);
        }
    }
}

void RecordingSession::muxLoop() {
    bool videoDone = false;
    bool audioDone = !hasAudio_;
    while (!videoDone || !audioDone) {
        auto video = videoPacketQueue_.pop(std::chrono::milliseconds(10));
        if (video && muxer_) {
            muxer_->write(*video);
        } else if (videoPacketQueue_.isClosed()) {
            videoDone = true;
        }
        auto audio = audioPacketQueue_.pop(std::chrono::milliseconds(0));
        if (audio && muxer_) {
            muxer_->write(*audio);
        } else if (audioPacketQueue_.isClosed()) {
            audioDone = true;
        }
    }
}

void RecordingSession::setState(State state) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        state_ = state;
    }
    if (onState_) {
        onState_(state);
    }
}

void RecordingSession::cleanupQueues() {
    videoQueue_.close();
    systemAudioQueue_.close();
    micAudioQueue_.close();
    audioQueue_.close();
    videoPacketQueue_.close();
    audioPacketQueue_.close();
    if (audioMixThread_.joinable()) {
        audioMixThread_.join();
    }
    if (encoderThread_.joinable()) {
        encoderThread_.join();
    }
    if (muxThread_.joinable()) {
        muxThread_.join();
    }
}

} // namespace nr
