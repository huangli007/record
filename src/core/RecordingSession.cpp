#include "core/RecordingSession.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>

#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#include "capture/macos/CoreAudioCapturer.h"
#include "capture/macos/CoreAudioTapCapturer.h"
#include "capture/macos/ScreenCaptureKitCapturer.h"
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "capture/windows/DXGICapturer.h"
#include "capture/windows/WASAPICapturer.h"
#endif

#include "codec/EncoderFactory.h"
#include "core/TimeBase.h"

namespace nr {

namespace {

std::string expandPath(const std::string& path) {
    std::string result = path;
    if (!result.empty() && result[0] == '~') {
#if defined(_WIN32)
        const char* home = std::getenv("USERPROFILE");
#else
        const char* home = std::getenv("HOME");
#endif
        if (home) {
            result.replace(0, 1, home);
        }
    }
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
#elif defined(_WIN32)
    if (config_.video.mode == CaptureMode::FullScreen &&
        (config_.video.width <= 0 || config_.video.height <= 0)) {
        config_.video.width = GetSystemMetrics(SM_CXSCREEN);
        config_.video.height = GetSystemMetrics(SM_CYSCREEN);
    }
#endif
    if (config_.video.width <= 0 || config_.video.height <= 0) {
        config_.video.width = 1920;
        config_.video.height = 1080;
    }

    const std::string dir = expandPath(config_.general.outputDir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    outputPath_ = (std::filesystem::path(dir) / config_.defaultFileName()).string();

#if defined(__APPLE__)
    screenCapturer_ = std::make_unique<ScreenCaptureKitCapturer>();
    micCapturer_ =
        config_.audio.captureMicrophone ? std::make_unique<CoreAudioCapturer>() : nullptr;
    tapCapturer_ =
        config_.audio.captureSystemAudio ? std::make_unique<CoreAudioTapCapturer>() : nullptr;
#elif defined(_WIN32)
    screenCapturer_ = std::make_unique<DXGICapturer>();
    micCapturer_ =
        config_.audio.captureMicrophone ? std::make_unique<WASAPICapturer>(false) : nullptr;
    tapCapturer_ =
        config_.audio.captureSystemAudio ? std::make_unique<WASAPICapturer>(true) : nullptr;
#endif
    tapAudioActive_.store(false);
    tapStarted_.store(false);
    tapFrames_.store(0);

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

    if (!screenCapturer_->start(
            config_.video, config_.audio,
            [this](VideoFrame f) { onVideoFrame(std::move(f)); },
            [this](AudioFrame f) { onSystemAudio(std::move(f)); })) {
#if defined(__APPLE__)
        lastError_ = "无法启动屏幕采集，请检查“屏幕录制”权限（系统设置 → 隐私与安全性）";
#else
        lastError_ = "无法启动屏幕采集";
#endif
        running_.store(false);
        cleanupQueues();
        setState(State::Error);
        return false;
    }

#if defined(_WIN32)
    // On Windows the system audio loopback runs from the start (no SCK).
    if (tapCapturer_ && config_.audio.captureSystemAudio) {
        tapStarted_.store(true);
        if (tapCapturer_->start(
                config_.audio,
                [this](AudioFrame f) { onTapSystemAudio(std::move(f)); })) {
            tapAudioActive_.store(true);
        } else {
            std::fprintf(stderr, "[rec] 系统音频（WASAPI 回环）启动失败\n");
        }
    }
#endif

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
    if (tapCapturer_) {
        tapCapturer_->stop();
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
    // Ensure the file contains at least one video frame so it is always
    // playable.  When the user stops recording immediately (before the
    // encoder has produced any packets) the file would otherwise contain
    // only an empty moov and no media data.
    if (muxer_ && videoEncoder_ && !muxer_->hasWrittenPackets()) {
        const int w = config_.video.width > 0 ? config_.video.width : 1920;
        const int h = config_.video.height > 0 ? config_.video.height : 1080;
#if defined(__APPLE__)
        // On macOS, create a black CVPixelBuffer for the dummy frame.
        CVPixelBufferRef pb = nullptr;
        CVPixelBufferCreate(kCFAllocatorDefault, w, h,
                            kCVPixelFormatType_32BGRA, nullptr, &pb);
        if (pb) {
            CVPixelBufferLockBaseAddress(pb, 0);
            std::memset(CVPixelBufferGetBaseAddress(pb), 0,
                        CVPixelBufferGetBytesPerRow(pb) * h);
            CVPixelBufferUnlockBaseAddress(pb, 0);
            VideoFrame dummy(pb);
            dummy.width = w;
            dummy.height = h;
            dummy.ptsUs = 0;
            dummy.durationUs = 1;
            videoEncoder_->encode(std::move(dummy));
            CVPixelBufferRelease(pb);
        }
#else
        VideoFrame dummy{};
        dummy.width = w;
        dummy.height = h;
        dummy.bgra.resize(static_cast<size_t>(w) * h * 4, 0);
        dummy.stride = w * 4;
        dummy.ptsUs = 0;
        dummy.durationUs = 1;
        videoEncoder_->encode(std::move(dummy));
#endif
        videoEncoder_->flush();
        // Drain any packets that the encode + flush produced.
        while (auto pkt = videoPacketQueue_.pop(
                    std::chrono::milliseconds(0))) {
            muxer_->write(*pkt);
        }
    }
    if (muxer_) {
        muxer_->close();
    }
    writeAudioDebugLog();

    videoEncoder_.reset();
    audioEncoder_.reset();
    muxer_.reset();
    screenCapturer_.reset();
    micCapturer_.reset();
    tapCapturer_.reset();
    tapAudioActive_.store(false);
    tapStarted_.store(false);
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
    if (paused_.load() || tapAudioActive_.load()) {
        return;
    }
    systemAudioFrames_.fetch_add(1);
    systemAudioQueue_.push(std::move(frame), std::chrono::milliseconds(10));
}

void RecordingSession::onTapSystemAudio(AudioFrame frame) {
    if (paused_.load()) {
        return;
    }
    tapAudioActive_.store(true);
    tapFrames_.fetch_add(1);
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
    encodedPackets_.fetch_add(1);
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
    const auto startTime = std::chrono::steady_clock::now();
    while (running_.load() ||
           !systemAudioQueue_.isClosed() || !micAudioQueue_.isClosed()) {
        auto sys = systemAudioQueue_.pop(std::chrono::milliseconds(20));
        auto mic = micAudioQueue_.pop(std::chrono::milliseconds(0));

        const auto pushAudio = [this](AudioFrame frame) {
            if (audioQueue_.push(std::move(frame), std::chrono::milliseconds(20))) {
                mixPushed_.fetch_add(1);
            } else {
                mixDropped_.fetch_add(1);
            }
        };

        if (sys && mic) {
            if (std::llabs(sys->ptsUs - mic->ptsUs) < 40'000) {
                pushAudio(mixFrames(*sys, *mic));
            } else if (sys->ptsUs < mic->ptsUs) {
                pushAudio(std::move(*sys));
                micAudioQueue_.unshift(std::move(*mic));
            } else {
                pushAudio(std::move(*mic));
                systemAudioQueue_.unshift(std::move(*sys));
            }
        } else if (sys) {
            pushAudio(std::move(*sys));
        } else if (mic) {
            pushAudio(std::move(*mic));
        }

#if defined(__APPLE__)
        // Watchdog: if SCK was configured to capture system audio but has not
        // delivered a single audio buffer within 2 seconds (a known failure
        // mode on macOS 14.4+ depending on the granted privacy permission),
        // fall back to the CoreAudio process tap so recordings still have
        // sound. Once the tap is active, SCK audio frames are ignored.
        if (!tapStarted_.load() && config_.audio.captureSystemAudio && tapCapturer_ &&
            std::chrono::steady_clock::now() - startTime >
                std::chrono::seconds(2)) {
            tapStarted_.store(true);
            long long callbacks = -1;
            long long usableSamples = 0;
            if (auto* sck =
                    dynamic_cast<ScreenCaptureKitCapturer*>(screenCapturer_.get())) {
                const auto s = sck->audioDebugStats();
                callbacks = s.audioCallbacks;
                usableSamples = s.audioSamples;
            }
            // Fall back when SCK delivered no audio at all, or when it
            // delivered buffers that never parsed into usable samples.
            if (callbacks == 0 || usableSamples == 0) {
                if (tapCapturer_->start(
                        config_.audio,
                        [this](AudioFrame f) { onTapSystemAudio(std::move(f)); })) {
                    // Once the tap owns system audio, drop any SCK audio
                    // frames (they would double the recorded sound).
                    tapAudioActive_.store(true);
                    std::fprintf(stderr,
                                 "[rec] SCK 音频无数据，已切换到系统音频 Tap 采集\n");
                } else {
                    std::fprintf(stderr,
                                 "[rec] 系统音频 Tap 启动失败，保留 SCK 音频路径\n");
                }
            }
        }
#endif
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
            if (audioEncoder_->encode(*audio)) {
                encodedFrames_.fetch_add(1);
            } else {
                encodeFailures_.fetch_add(1);
            }
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
            if (muxer_->write(*audio)) {
                muxedPackets_.fetch_add(1);
            } else {
                muxFailures_.fetch_add(1);
            }
        } else if (audioPacketQueue_.isClosed()) {
            audioDone = true;
        }
    }
}

RecordingSession::AudioDebugStats RecordingSession::audioDebugStats() const {
    AudioDebugStats stats;
    stats.systemAudioFrames = systemAudioFrames_.load();
    stats.tapFrames = tapFrames_.load();
    stats.tapActive = tapAudioActive_.load();
    stats.mixPushed = mixPushed_.load();
    stats.mixDropped = mixDropped_.load();
    stats.encodedFrames = encodedFrames_.load();
    stats.encodeFailures = encodeFailures_.load();
    stats.encodedPackets = encodedPackets_.load();
    stats.muxedPackets = muxedPackets_.load();
    stats.muxFailures = muxFailures_.load();
#if defined(__APPLE__)
    if (tapCapturer_) {
        if (auto* tap = dynamic_cast<CoreAudioTapCapturer*>(tapCapturer_.get())) {
            stats.tapSetupErrors = tap->stats().setupErrors;
        }
    }
    if (auto* sck = dynamic_cast<ScreenCaptureKitCapturer*>(screenCapturer_.get())) {
        const auto s = sck->audioDebugStats();
        stats.sckAudioCallbacks = s.audioCallbacks;
        stats.sckAudioEmptyDrops = s.audioEmptyDrops;
        stats.sckAudioSamples = s.audioSamples;
        stats.sckAudioError = s.audioSetupError;
        stats.sckParseReason = s.audioParseReason;
        stats.parseNoFormat = s.parseNoFormat;
        stats.parseNoASBD = s.parseNoASBD;
        stats.parseNoBufferList = s.parseNoBufferList;
        stats.parseBufferError = s.parseBufferError;
        stats.parseNoChannels = s.parseNoChannels;
        stats.parseNoSamples = s.parseNoSamples;
    }
#endif
    return stats;
}

void RecordingSession::writeAudioDebugLog() {
    if (outputPath_.empty()) {
        return;
    }
    const std::filesystem::path dir =
        std::filesystem::path(outputPath_).parent_path();
    const std::filesystem::path logPath = dir / "audio_debug.log";
    const auto s = audioDebugStats();
    const std::string baseName =
        std::filesystem::path(outputPath_).filename().string();
    FILE* f = std::fopen(logPath.string().c_str(), "a");
    if (!f) {
        return;
    }
    std::time_t now = std::time(nullptr);
    char ts[32] = {};
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::fprintf(
        f,
        "%s | %s | sckCallbacks=%lld sckEmptyDrops=%lld sckSamples=%lld "
        "sckAudioError=\"%s\" sckParseReason=\"%s\" "
        "(noFormat=%lld noASBD=%lld noBufferList=%lld bufferError=%lld "
        "noChannels=%lld noSamples=%lld) | "
        "systemFrames=%lld tapFrames=%lld tapActive=%d "
        "tapSetupErrors=%lld | mixPushed=%lld mixDropped=%lld | "
        "encodedFrames=%lld encodeFailures=%lld | encodedPackets=%lld "
        "muxedPackets=%lld muxFailures=%lld\n",
        ts, baseName.c_str(),
        s.sckAudioCallbacks, s.sckAudioEmptyDrops, s.sckAudioSamples,
        s.sckAudioError.c_str(), s.sckParseReason.c_str(),
        s.parseNoFormat, s.parseNoASBD, s.parseNoBufferList,
        s.parseBufferError, s.parseNoChannels, s.parseNoSamples,
        s.systemAudioFrames, s.tapFrames, s.tapActive ? 1 : 0, s.tapSetupErrors,
        s.mixPushed, s.mixDropped,
        s.encodedFrames, s.encodeFailures,
        s.encodedPackets, s.muxedPackets, s.muxFailures);
    std::fclose(f);
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
