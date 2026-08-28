#include "capture/macos/ScreenCaptureKitCapturer.h"

#import <AppKit/AppKit.h>
#import <CoreMedia/CoreMedia.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

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

// Converts a CMSampleBuffer to an interleaved Float32 AudioFrame.
AudioFrame makeAudioFrame(CMSampleBufferRef sampleBuffer,
                          AudioSource source,
                          int volumePercent) {
    AudioFrame frame;
    frame.source = source;

    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sampleBuffer);
    if (!format) {
        return frame;
    }
    const AudioStreamBasicDescription* asbd =
        CMAudioFormatDescriptionGetStreamBasicDescription(format);
    if (!asbd) {
        return frame;
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
        return frame;
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
        return frame;
    }

    const CMItemCount numSamples = CMSampleBufferGetNumSamples(sampleBuffer);
    const int channels = frame.channels;
    if (channels > 0 && numSamples > 0) {
        frame.samples.resize(static_cast<size_t>(numSamples) * channels);
        const float volume = static_cast<float>(volumePercent) / 100.0f;

        if (abl->mNumberBuffers == 1 &&
            abl->mBuffers[0].mNumberChannels == channels) {
            // Interleaved input.
            const float* data = static_cast<const float*>(abl->mBuffers[0].mData);
            if (data) {
                for (CMItemCount i = 0; i < numSamples * channels; ++i) {
                    frame.samples[i] = data[i] * volume;
                }
            }
        } else {
            // Non-interleaved input: one buffer per channel.
            for (CMItemCount i = 0; i < numSamples; ++i) {
                for (int ch = 0; ch < channels; ++ch) {
                    float sample = 0.0f;
                    if (ch < static_cast<int>(abl->mNumberBuffers)) {
                        const AudioBuffer& buf = abl->mBuffers[ch];
                        const float* data = static_cast<const float*>(buf.mData);
                        if (data && buf.mDataByteSize >= (i + 1) * sizeof(float)) {
                            sample = data[i];
                        }
                    }
                    frame.samples[static_cast<size_t>(i) * channels + ch] = sample * volume;
                }
            }
        }
    }

    free(abl);
    if (blockBuffer) {
        CFRelease(blockBuffer);
    }
    return frame;
}

} // namespace

struct ScreenCaptureKitCapturer::Impl {
    id delegate = nil;   // NRSCKDelegate, defined below
    SCStream* stream = nil;
    dispatch_queue_t queue = nullptr;

    VideoFrameCallback onVideo;
    AudioFrameCallback onAudio;

    std::atomic<bool> paused{false};
    std::atomic<bool> stopped{false};
    std::atomic<int> frameRate{60};
    std::atomic<int> systemVolume{100};
    std::atomic<int64_t> videoBaseUs{0};
    std::atomic<int64_t> audioBaseUs{0};
    std::mutex errorMutex;
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
        nr::AudioFrame audio =
            nr::makeAudioFrame(sampleBuffer, nr::AudioSource::System, impl->volume());
        if (audio.samples.empty()) {
            return;
        }
        if (impl->audioBase() == 0 && audio.ptsUs != 0) {
            impl->audioBaseUs.store(audio.ptsUs);
        }
        audio.ptsUs -= impl->audioBase();
        impl->emitAudio(std::move(audio));
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
                                     VideoFrameCallback onFrame) {
    if (!impl_) {
        return false;
    }
    stop();

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
        scConfig.width = static_cast<size_t>(videoConfig.region.width);
        scConfig.height = static_cast<size_t>(videoConfig.region.height);
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

    impl_->onVideo = std::move(onFrame);
    impl_->systemVolume.store(audioConfig.systemVolume);
    impl_->paused.store(false);
    impl_->stopped.store(false);
    impl_->videoBaseUs.store(0);
    impl_->audioBaseUs.store(0);

    SCContentFilter* filter = nil;
    if (targetWindow) {
        filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:targetWindow];
    } else {
        filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
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
        [impl_->stream addStreamOutput:impl_->delegate
                                  type:SCStreamOutputTypeAudio
                    sampleHandlerQueue:impl_->queue
                                 error:&audioError];
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

void ScreenCaptureKitCapturer::stop() {
    if (!impl_ || !impl_->stream) {
        return;
    }
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
