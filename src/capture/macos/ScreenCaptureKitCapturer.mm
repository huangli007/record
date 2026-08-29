#include "capture/macos/ScreenCaptureKitCapturer.h"

#import <AppKit/AppKit.h>
#import <CoreMedia/CoreMedia.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <unistd.h>

#include "capture/AudioFrame.h"
#include "capture/VideoFrame.h"
#include "core/TimeBase.h"

namespace nr {

namespace {

// Fetches shareable content synchronously (10s timeout). Returns nil when
// screen recording permission is missing or the fetch fails.
SCShareableContent* fetchShareableContentSync() {
    __block SCShareableContent* content = nil;
    __block NSError* fetchError = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [SCShareableContent getShareableContentWithCompletionHandler:
        ^(SCShareableContent* c, NSError* e) {
            content = c;
            fetchError = e;
            dispatch_semaphore_signal(sem);
        }];
    dispatch_semaphore_wait(sem,
                            dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
    (void)fetchError;
    return content;
}

// Describes the outcome of parsing an SCK audio CMSampleBuffer.
struct AudioParseOutcome {
    AudioFrame frame;
    // Non-null when parsing failed and frame.samples is empty.
    const char* failure = nullptr;
};

// Converts a CMSampleBuffer to an interleaved Float32 AudioFrame. Tolerates
// both interleaved and non-interleaved float32 layouts and reports exactly why
// a buffer could not be parsed (for diagnostics).
AudioParseOutcome makeAudioFrame(CMSampleBufferRef sampleBuffer,
                                 AudioSource source,
                                 int volumePercent) {
    AudioParseOutcome outcome;
    AudioFrame& frame = outcome.frame;
    frame.source = source;

    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sampleBuffer);
    if (!format) {
        outcome.failure = "no-format";
        return outcome;
    }
    const AudioStreamBasicDescription* asbd =
        CMAudioFormatDescriptionGetStreamBasicDescription(format);
    if (!asbd) {
        outcome.failure = "no-asbd";
        return outcome;
    }

    frame.sampleRate = static_cast<int>(asbd->mSampleRate);
    frame.channels = static_cast<int>(asbd->mChannelsPerFrame);

    const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    if (CMTIME_IS_NUMERIC(pts)) {
        frame.ptsUs = TimeBase::fromSeconds(CMTimeGetSeconds(pts)).count();
    }

    const CMTime duration = CMSampleBufferGetDuration(sampleBuffer);
    if (CMTIME_IS_NUMERIC(duration) && duration.value > 0) {
        frame.durationUs = TimeBase::fromSeconds(CMTimeGetSeconds(duration)).count();
    }

    AudioBufferList* abl = nullptr;
    size_t ablSize = 0;
    CMBlockBufferRef blockBuffer = nullptr;
    CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
        sampleBuffer, &ablSize, nullptr, 0, kCFAllocatorNull, kCFAllocatorNull, 0, nullptr);
    if (ablSize == 0) {
        outcome.failure = "no-buffer-list";
        return outcome;
    }
    abl = static_cast<AudioBufferList*>(malloc(ablSize));
    const OSStatus status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
        sampleBuffer, &ablSize, abl, kCMBlockBufferAssureMemoryNowFlag,
        kCFAllocatorNull, kCFAllocatorNull, 0, &blockBuffer);
    if (status != noErr) {
        free(abl);
        if (blockBuffer) {
            CFRelease(blockBuffer);
        }
        outcome.failure = "buffer-error";
        return outcome;
    }

    // Determine channel count, tolerating ASBDs that leave it at 0.
    int channels = frame.channels;
    if (channels <= 0) {
        channels = static_cast<int>(abl->mNumberBuffers);
    }
    if (channels <= 0) {
        channels = static_cast<int>(abl->mBuffers[0].mNumberChannels);
    }
    frame.channels = channels;

    // Determine sample count, tolerating CMSampleBufferGetNumSamples() == 0.
    CMItemCount numSamples = CMSampleBufferGetNumSamples(sampleBuffer);
    const bool interleaved =
        abl->mNumberBuffers == 1 &&
        abl->mBuffers[0].mNumberChannels == static_cast<UInt32>(channels);
    const UInt32 bytesPerSample =
        (asbd->mBitsPerChannel > 0 ? asbd->mBitsPerChannel : 32) / 8;
    if (numSamples <= 0) {
        if (interleaved) {
            numSamples = abl->mBuffers[0].mDataByteSize /
                         (static_cast<UInt32>(channels) * bytesPerSample);
        } else {
            for (UInt32 i = 0; i < abl->mNumberBuffers; ++i) {
                const UInt32 n =
                    abl->mBuffers[i].mDataByteSize / bytesPerSample;
                if (n > numSamples) {
                    numSamples = n;
                }
            }
        }
    }

    if (channels <= 0 || numSamples <= 0) {
        outcome.failure = channels <= 0 ? "no-channels" : "no-samples";
    } else {
        frame.samples.resize(static_cast<size_t>(numSamples) * channels);
        const float volume = static_cast<float>(volumePercent) / 100.0f;
        const bool isFloat =
            (asbd->mFormatFlags & kAudioFormatFlagIsFloat) != 0 ||
            asbd->mBitsPerChannel == 32;

        if (interleaved) {
            if (isFloat) {
                const float* data =
                    static_cast<const float*>(abl->mBuffers[0].mData);
                if (data) {
                    for (CMItemCount i = 0; i < numSamples * channels; ++i) {
                        frame.samples[i] = data[i] * volume;
                    }
                }
            } else {
                // SInt16 interleaved (defensive; SCK is float32).
                const int16_t* data =
                    static_cast<const int16_t*>(abl->mBuffers[0].mData);
                if (data) {
                    for (CMItemCount i = 0; i < numSamples * channels; ++i) {
                        frame.samples[i] =
                            static_cast<float>(data[i]) / 32768.0f * volume;
                    }
                }
            }
        } else {
            // Non-interleaved input: one buffer per channel.
            for (CMItemCount i = 0; i < numSamples; ++i) {
                for (int ch = 0; ch < channels; ++ch) {
                    float sample = 0.0f;
                    if (ch < static_cast<int>(abl->mNumberBuffers)) {
                        const AudioBuffer& buf = abl->mBuffers[ch];
                        if (isFloat) {
                            const float* data =
                                static_cast<const float*>(buf.mData);
                            if (data && buf.mDataByteSize >=
                                            (i + 1) * sizeof(float)) {
                                sample = data[i];
                            }
                        } else {
                            const int16_t* data =
                                static_cast<const int16_t*>(buf.mData);
                            if (data && buf.mDataByteSize >=
                                            (i + 1) * sizeof(int16_t)) {
                                sample =
                                    static_cast<float>(data[i]) / 32768.0f;
                            }
                        }
                    }
                    frame.samples[static_cast<size_t>(i) * channels + ch] =
                        sample * volume;
                }
            }
        }
    }

    free(abl);
    if (blockBuffer) {
        CFRelease(blockBuffer);
    }
    return outcome;
}

} // namespace

struct ScreenCaptureKitCapturer::Impl {
    id delegate = nil;   // NRSCKDelegate, defined below
    SCStream* stream = nil;
    dispatch_queue_t queue = nullptr;

    VideoFrameCallback onVideo;
    AudioFrameCallback onAudio;
    VideoConfig videoConfig;
    AudioConfig audioConfig;
    ScreenCaptureKitCapturer* owner = nullptr;

    std::atomic<bool> paused{false};
    std::atomic<bool> stopped{false};
    std::atomic<int> frameRate{60};
    std::atomic<int> systemVolume{100};
    std::atomic<int64_t> videoBaseUs{0};
    std::atomic<int64_t> audioBaseUs{0};
    std::atomic<long long> audioCallbacks{0};
    std::atomic<long long> audioEmptyDrops{0};
    std::atomic<long long> audioSamples{0};
    std::mutex audioSetupMutex;
    std::string audioSetupError;
    std::string audioParseReason;
    std::atomic<long long> audioParseNoFormat{0};
    std::atomic<long long> audioParseNoASBD{0};
    std::atomic<long long> audioParseNoBufferList{0};
    std::atomic<long long> audioParseBufferError{0};
    std::atomic<long long> audioParseNoChannels{0};
    std::atomic<long long> audioParseNoSamples{0};
    std::mutex errorMutex;
    std::mutex restartMutex;
    dispatch_queue_t restartQueue = nullptr;
    id wakeObserver = nil;
    id contentObserver = nil;
    std::string lastError;

    bool isPaused() const { return paused.load(); }
    int fps() const { return frameRate.load(); }
    int volume() const { return systemVolume.load(); }
    int64_t videoBase() const { return videoBaseUs.load(); }
    int64_t audioBase() const { return audioBaseUs.load(); }

    void markStopped(const char* message) {
        stopped.store(true);
        std::lock_guard<std::mutex> lock(errorMutex);
        if (message) {
            lastError = message;
        }
    }

    void handleSystemChange() {
        if (stopped.load() || !owner || !restartQueue) {
            return;
        }
        dispatch_async(restartQueue, ^{
            owner->restartStream();
        });
    }

    void emitVideo(VideoFrame frame) {
        if (onVideo) {
            onVideo(std::move(frame));
        }
    }

    void emitAudio(AudioFrame frame) {
        if (onAudio) {
            onAudio(std::move(frame));
        }
    }

    void recordAudioParseFailure(const char* reason) {
        {
            std::lock_guard<std::mutex> lock(audioSetupMutex);
            if (audioParseReason.empty() && reason) {
                audioParseReason = reason;
            }
        }
        if (std::strcmp(reason, "no-format") == 0) {
            audioParseNoFormat.fetch_add(1);
        } else if (std::strcmp(reason, "no-asbd") == 0) {
            audioParseNoASBD.fetch_add(1);
        } else if (std::strcmp(reason, "no-buffer-list") == 0) {
            audioParseNoBufferList.fetch_add(1);
        } else if (std::strcmp(reason, "buffer-error") == 0) {
            audioParseBufferError.fetch_add(1);
        } else if (std::strcmp(reason, "no-channels") == 0) {
            audioParseNoChannels.fetch_add(1);
        } else if (std::strcmp(reason, "no-samples") == 0) {
            audioParseNoSamples.fetch_add(1);
        }
    }
};

} // namespace nr

@interface NRSCKDelegate : NSObject <SCStreamDelegate, SCStreamOutput>
@property(nonatomic, assign) nr::ScreenCaptureKitCapturer* owner;
@end

@implementation NRSCKDelegate

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    nr::ScreenCaptureKitCapturer::Impl* impl =
        self.owner ? self.owner->impl() : nullptr;
    if (impl) {
        impl->markStopped(error.localizedDescription.UTF8String);
    }
}

- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                  ofType:(SCStreamOutputType)type {
    nr::ScreenCaptureKitCapturer::Impl* impl =
        self.owner ? self.owner->impl() : nullptr;
    if (!impl || impl->isPaused()) {
        return;
    }

    if (type == SCStreamOutputTypeScreen) {
        CVPixelBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
        if (!pixelBuffer) {
            return;
        }
        nr::VideoFrame frame(pixelBuffer);
        const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
        if (CMTIME_IS_NUMERIC(pts)) {
            const int64_t rawPts =
                nr::TimeBase::fromSeconds(CMTimeGetSeconds(pts)).count();
            if (impl->videoBase() == 0) {
                impl->videoBaseUs.store(rawPts);
            }
            frame.ptsUs = rawPts - impl->videoBase();
        }
        frame.width = CVPixelBufferGetWidth(pixelBuffer);
        frame.height = CVPixelBufferGetHeight(pixelBuffer);
        if (impl->fps() > 0) {
            frame.durationUs = 1'000'000.0 / impl->fps();
        }
        impl->emitVideo(std::move(frame));
    } else if (type == SCStreamOutputTypeAudio) {
        impl->audioCallbacks.fetch_add(1);
        nr::AudioParseOutcome parsed = nr::makeAudioFrame(
            sampleBuffer, nr::AudioSource::System, impl->volume());
        if (parsed.frame.samples.empty()) {
            impl->audioEmptyDrops.fetch_add(1);
            if (parsed.failure) {
                impl->recordAudioParseFailure(parsed.failure);
            }
            return;
        }
        impl->audioSamples.fetch_add(
            static_cast<long long>(parsed.frame.samples.size()));
        if (impl->audioBase() == 0 && parsed.frame.ptsUs != 0) {
            impl->audioBaseUs.store(parsed.frame.ptsUs);
        }
        parsed.frame.ptsUs -= impl->audioBase();
        impl->emitAudio(std::move(parsed.frame));
    }
}

@end

namespace nr {

ScreenCaptureKitCapturer::ScreenCaptureKitCapturer()
    : impl_(std::make_unique<Impl>()) {}

ScreenCaptureKitCapturer::~ScreenCaptureKitCapturer() {
    stop();
}

bool ScreenCaptureKitCapturer::start(const VideoConfig& videoConfig,
                                     const AudioConfig& audioConfig,
                                     VideoFrameCallback onFrame,
                                     AudioFrameCallback onAudio) {
    if (!impl_) {
        return false;
    }
    stop();

    impl_->videoConfig = videoConfig;
    impl_->audioConfig = audioConfig;
    impl_->owner = this;
    impl_->onVideo = std::move(onFrame);
    impl_->onAudio = std::move(onAudio);
    impl_->stopped.store(false);
    if (!impl_->restartQueue) {
        impl_->restartQueue =
            dispatch_queue_create("com.notionrecorder.sck.restart",
                                  DISPATCH_QUEUE_SERIAL);
    }
    if (!startStream()) {
        return false;
    }
    registerObservers();
    return true;
}

bool ScreenCaptureKitCapturer::startStream() {
    if (!impl_) {
        return false;
    }
    const VideoConfig& videoConfig = impl_->videoConfig;
    const AudioConfig& audioConfig = impl_->audioConfig;

    SCShareableContent* content = fetchShareableContentSync();
    if (!content || content.displays.count == 0) {
        return false;
    }

    SCDisplay* display = content.displays.firstObject;
    const CGDirectDisplayID mainId = CGMainDisplayID();
    for (SCDisplay* d in content.displays) {
        if (d.displayID == mainId) {
            display = d;
            break;
        }
    }

    int frameRate = videoConfig.fps;
    if (frameRate <= 0) {
        for (NSScreen* screen in NSScreen.screens) {
            if ([[screen deviceDescription][@"NSScreenNumber"] unsignedIntValue] ==
                display.displayID) {
                frameRate = static_cast<int>(screen.maximumFramesPerSecond);
                break;
            }
        }
        if (frameRate <= 0) {
            frameRate = 60;
        }
    }
    impl_->frameRate.store(frameRate);

    SCStreamConfiguration* scConfig = [SCStreamConfiguration new];
    scConfig.minimumFrameInterval = CMTimeMake(1, frameRate);
    scConfig.queueDepth = 8;
    scConfig.showsCursor = videoConfig.captureCursor;
    scConfig.pixelFormat = kCVPixelFormatType_32BGRA;
    if (@available(macOS 15.0, *)) {
        scConfig.showMouseClicks = videoConfig.clickEffects;
    }
    scConfig.capturesAudio = audioConfig.captureSystemAudio;
    scConfig.sampleRate = audioConfig.sampleRate > 0 ? audioConfig.sampleRate : 48000;
    scConfig.channelCount = audioConfig.channels > 0 ? audioConfig.channels : 2;

    SCWindow* targetWindow = nil;
    if (videoConfig.mode == CaptureMode::Window) {
        if (videoConfig.windowId <= 0) {
            return false;  // window mode requires a picked window
        }
        for (SCWindow* w in content.windows) {
            if (w.windowID == static_cast<CGWindowID>(videoConfig.windowId)) {
                targetWindow = w;
                break;
            }
        }
        if (!targetWindow) {
            return false;  // window was closed before capture started
        }
    }

    if (videoConfig.mode == CaptureMode::Region && videoConfig.region.valid()) {
        scConfig.sourceRect = CGRectMake(videoConfig.region.x,
                                         videoConfig.region.y,
                                         videoConfig.region.width,
                                         videoConfig.region.height);
        // sourceRect is in points; the output size is in pixels, so scale by
        // the display's backing scale factor for crisp Retina captures.
        const double scale = display.width / display.frame.size.width;
        scConfig.width = static_cast<size_t>(videoConfig.region.width * scale);
        scConfig.height = static_cast<size_t>(videoConfig.region.height * scale);
    } else if (targetWindow) {
        // Capture the window at its current pixel size; the encoder scales
        // dynamically if the window is resized while recording.
        SCDisplay* winDisplay = display;
        for (SCDisplay* d in content.displays) {
            if (CGRectIntersectsRect(d.frame, targetWindow.frame)) {
                winDisplay = d;
                break;
            }
        }
        const double scale = winDisplay.width / winDisplay.frame.size.width;
        scConfig.width = static_cast<size_t>(targetWindow.frame.size.width * scale);
        scConfig.height = static_cast<size_t>(targetWindow.frame.size.height * scale);
    } else {
        scConfig.width = display.width;
        scConfig.height = display.height;
    }

    impl_->systemVolume.store(audioConfig.systemVolume);
    impl_->paused.store(false);

    SCContentFilter* filter = nil;
    if (targetWindow) {
        filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:targetWindow];
    } else {
        // Exclude our own UI (floating bar, settings, toasts) from the
        // recording, but keep the annotation overlay (identified by title)
        // so annotations drawn on screen are captured.
        NSMutableArray<SCWindow*>* exclude = [NSMutableArray array];
        const pid_t selfPid = getpid();
        for (SCWindow* w in content.windows) {
            if (w.owningApplication.processID != selfPid) {
                continue;
            }
            if ([w.title isEqualToString:@"NotionRecorder-Annotation"]) {
                continue;
            }
            [exclude addObject:w];
        }
        filter = [[SCContentFilter alloc] initWithDisplay:display
                                         excludingWindows:exclude];
    }
    if (!filter) {
        return false;
    }
    impl_->delegate = [NRSCKDelegate new];
    [impl_->delegate setOwner:this];
    impl_->queue = dispatch_queue_create("com.notionrecorder.sck",
                                         DISPATCH_QUEUE_SERIAL);

    impl_->stream = [[SCStream alloc] initWithFilter:filter
                                       configuration:scConfig
                                            delegate:impl_->delegate];
    NSError* outputError = nil;
    if (![impl_->stream addStreamOutput:impl_->delegate
                                   type:SCStreamOutputTypeScreen
                       sampleHandlerQueue:impl_->queue
                                    error:&outputError]) {
        impl_->stream = nil;
        impl_->delegate = nil;
        impl_->queue = nullptr;
        return false;
    }
    if (audioConfig.captureSystemAudio) {
        NSError* audioError = nil;
        const BOOL audioOk = [impl_->stream
            addStreamOutput:impl_->delegate
                       type:SCStreamOutputTypeAudio
         sampleHandlerQueue:impl_->queue
                      error:&audioError];
        if (!audioOk) {
            std::lock_guard<std::mutex> lock(impl_->audioSetupMutex);
            impl_->audioSetupError =
                audioError ? audioError.localizedDescription.UTF8String
                           : "addStreamOutput(audio) failed";
            std::fprintf(stderr, "[SCK] 音频输出注册失败: %s\n",
                         impl_->audioSetupError.c_str());
        }
    }

    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block BOOL ok = NO;
    [impl_->stream startCaptureWithCompletionHandler:^(NSError* err) {
        ok = (err == nil);
        dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));

    if (!ok) {
        [impl_->stream stopCaptureWithCompletionHandler:nil];
        impl_->stream = nil;
        impl_->delegate = nil;
        impl_->queue = nullptr;
        impl_->onVideo = nullptr;
        return false;
    }
    return true;
}

void ScreenCaptureKitCapturer::restartStream() {
    if (!impl_ || impl_->stopped.load()) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->restartMutex);
    if (impl_->stream) {
        impl_->videoBaseUs.store(0);
        impl_->audioBaseUs.store(0);
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [impl_->stream stopCaptureWithCompletionHandler:^(NSError*) {
            dispatch_semaphore_signal(sem);
        }];
        dispatch_semaphore_wait(sem,
                                dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
        impl_->stream = nil;
        impl_->delegate = nil;
        impl_->queue = nullptr;
    }
    if (!startStream()) {
        impl_->markStopped("屏幕内容变化后自动恢复失败");
    }
}

void ScreenCaptureKitCapturer::registerObservers() {
    if (!impl_ || impl_->wakeObserver || impl_->contentObserver) {
        return;
    }
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    impl_->wakeObserver = [center
        addObserverForName:NSWorkspaceDidWakeNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification*) {
                    impl_->handleSystemChange();
                }];
    impl_->contentObserver = [center
        addObserverForName:NSApplicationDidChangeScreenParametersNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification*) {
                    impl_->handleSystemChange();
                }];
}

void ScreenCaptureKitCapturer::unregisterObservers() {
    if (!impl_) {
        return;
    }
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    if (impl_->wakeObserver) {
        [center removeObserver:impl_->wakeObserver];
        impl_->wakeObserver = nil;
    }
    if (impl_->contentObserver) {
        [center removeObserver:impl_->contentObserver];
        impl_->contentObserver = nil;
    }
}

std::vector<WindowInfo> ScreenCaptureKitCapturer::listWindows() {
    std::vector<WindowInfo> result;
    SCShareableContent* content = fetchShareableContentSync();
    if (!content || content.windows.count == 0) {
        return result;
    }
    for (SCWindow* w in content.windows) {
        if (!w.onScreen) {
            continue;
        }
        WindowInfo info;
        info.id = static_cast<int>(w.windowID);
        info.title = w.title ? w.title.UTF8String : "";
        info.application =
            w.owningApplication ? w.owningApplication.applicationName.UTF8String : "";
        for (SCDisplay* d in content.displays) {
            if (CGRectIntersectsRect(d.frame, w.frame)) {
                const double scale = d.width / d.frame.size.width;
                info.width = static_cast<int>(w.frame.size.width * scale);
                info.height = static_cast<int>(w.frame.size.height * scale);
                break;
            }
        }
        result.push_back(std::move(info));
    }
    return result;
}

double ScreenCaptureKitCapturer::displayScale() {
    NSScreen* screen = NSScreen.mainScreen;
    return screen ? screen.backingScaleFactor : 1.0;
}

ScreenCaptureKitCapturer::AudioDebugStats
ScreenCaptureKitCapturer::audioDebugStats() const {
    AudioDebugStats stats;
    if (impl_) {
        stats.audioCallbacks = impl_->audioCallbacks.load();
        stats.audioEmptyDrops = impl_->audioEmptyDrops.load();
        stats.audioSamples = impl_->audioSamples.load();
        {
            std::lock_guard<std::mutex> lock(impl_->audioSetupMutex);
            stats.audioSetupError = impl_->audioSetupError;
            stats.audioParseReason = impl_->audioParseReason;
        }
        stats.parseNoFormat = impl_->audioParseNoFormat.load();
        stats.parseNoASBD = impl_->audioParseNoASBD.load();
        stats.parseNoBufferList = impl_->audioParseNoBufferList.load();
        stats.parseBufferError = impl_->audioParseBufferError.load();
        stats.parseNoChannels = impl_->audioParseNoChannels.load();
        stats.parseNoSamples = impl_->audioParseNoSamples.load();
    }
    return stats;
}

void ScreenCaptureKitCapturer::stop() {
    if (!impl_ || !impl_->stream) {
        unregisterObservers();
        impl_->owner = nullptr;
        impl_->onVideo = nullptr;
        impl_->onAudio = nullptr;
        impl_->stopped.store(true);
        return;
    }
    unregisterObservers();
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [impl_->stream stopCaptureWithCompletionHandler:^(NSError*) {
        dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
    impl_->stream = nil;
    impl_->delegate = nil;
    impl_->queue = nullptr;
    impl_->onVideo = nullptr;
    impl_->onAudio = nullptr;
    impl_->owner = nullptr;
    impl_->stopped.store(true);
}

void ScreenCaptureKitCapturer::setPaused(bool paused) {
    if (impl_) {
        impl_->paused.store(paused);
    }
}

void ScreenCaptureKitCapturer::setSystemAudioCallback(AudioFrameCallback onAudio) {
    if (impl_) {
        impl_->onAudio = std::move(onAudio);
    }
}

} // namespace nr
