#include "capture/macos/CoreAudioTapCapturer.h"

#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/CATapDescription.h>
#import <CoreAudio/AudioHardwareTapping.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "capture/AudioFrame.h"
#include "core/TimeBase.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace nr {

namespace {

constexpr const char* kTapName = "NotionRecorder System Audio";

// Returns the UID of the system's default output device (the time source for
// the aggregate), or nil if unavailable.
NSString* defaultOutputDeviceUID() {
    AudioDeviceID deviceID = kAudioObjectUnknown;
    UInt32 size = sizeof(deviceID);
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
                                   &size, &deviceID) != noErr ||
        deviceID == kAudioObjectUnknown) {
        return nil;
    }
    AudioObjectPropertyAddress uidAddr = {
        kAudioDevicePropertyDeviceUID,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain};
    CFStringRef uid = nullptr;
    size = sizeof(uid);
    if (AudioObjectGetPropertyData(deviceID, &uidAddr, 0, nullptr, &size, &uid) !=
            noErr ||
        !uid) {
        return nil;
    }
    return CFBridgingRelease(uid);
}

// Builds a private aggregate-device description that routes the given tap's
// audio to the device's input stream (readable like a microphone). The default
// output device is included as the main sub-device so the aggregate has a
// clock/time source and actually produces IO.
NSDictionary* makeAggregateDescription(NSString* tapUID, NSString* uid) {
    return @{
        @(kAudioAggregateDeviceUIDKey) : uid,
        @(kAudioAggregateDeviceNameKey) : @(kTapName),
        @(kAudioAggregateDeviceIsPrivateKey) : @1,
        @(kAudioAggregateDeviceIsStackedKey) : @0,
        @(kAudioAggregateDeviceTapListKey) : @[ @{@(kAudioSubTapUIDKey) : tapUID} ],
    };
}

} // namespace

struct CoreAudioTapCapturer::Impl {
    AudioObjectID tapID = kAudioObjectUnknown;
    AudioObjectID aggregateID = kAudioObjectUnknown;
    AudioDeviceIOProcID ioProcID = nullptr;
    AudioFrameCallback onFrame;
    std::atomic<bool> paused{false};
    std::atomic<bool> started{false};
    std::atomic<float> volume{1.0f};
    std::atomic<int> sampleRate{48000};
    std::atomic<int> channels{2};
    std::atomic<int64_t> frameCounter{0};
    std::atomic<long long> frames{0};
    std::atomic<long long> samples{0};
    std::atomic<long long> setupErrors{0};
    std::atomic<long long> renderCalls{0};
    std::atomic<int> lastRenderStatus{0};
    SwrContext* swr = nullptr;
    int inputRate = 0;
    int inputChannels = 0;
    std::vector<float> inInterleaved;   // scratch: native-rate interleaved
    std::vector<float> outInterleaved;  // scratch: 48k stereo interleaved
    int outCapacityFrames = 0;

    static OSStatus ioProc(AudioObjectID inDevice,
                           const AudioTimeStamp* inNow,
                           const AudioBufferList* inInputData,
                           const AudioTimeStamp* inInputTime,
                           AudioBufferList* inOutputData,
                           const AudioTimeStamp* inOutputTime,
                           void* inClientData) {
        (void)inDevice;
        (void)inNow;
        (void)inInputTime;
        (void)inOutputData;
        (void)inOutputTime;
        auto* self = static_cast<Impl*>(inClientData);
        if (!self || !inInputData || inInputData->mNumberBuffers == 0 ||
            !self->onFrame || !self->swr) {
            return noErr;
        }
        self->renderCalls.fetch_add(1);

        const int channels = self->inputChannels > 0 ? self->inputChannels : 2;
        const bool interleaved =
            inInputData->mNumberBuffers == 1 &&
            inInputData->mBuffers[0].mNumberChannels ==
                static_cast<UInt32>(channels);

        // Determine the frame count. For interleaved input the buffer holds
        // channels*N floats; for non-interleaved each buffer holds N floats.
        UInt32 frames = 0;
        if (interleaved) {
            frames = inInputData->mBuffers[0].mDataByteSize /
                     (static_cast<UInt32>(channels) * sizeof(float));
        } else {
            for (UInt32 i = 0; i < inInputData->mNumberBuffers; ++i) {
                const AudioBuffer& buf = inInputData->mBuffers[i];
                const UInt32 n = buf.mDataByteSize / sizeof(float);
                if (n > frames) {
                    frames = n;
                }
            }
        }
        if (frames == 0) {
            return noErr;
        }

        self->inInterleaved.resize(static_cast<size_t>(frames) * channels);
        float* dst = self->inInterleaved.data();
        if (interleaved) {
            std::memcpy(dst, inInputData->mBuffers[0].mData,
                        static_cast<size_t>(frames) * channels * sizeof(float));
        } else {
            // Non-interleaved: one buffer per channel.
            std::fill(dst, dst + static_cast<size_t>(frames) * channels, 0.0f);
            for (int ch = 0;
                 ch < channels && ch < static_cast<int>(inInputData->mNumberBuffers);
                 ++ch) {
                const AudioBuffer& buf = inInputData->mBuffers[ch];
                const float* src = static_cast<const float*>(buf.mData);
                if (!src) {
                    continue;
                }
                const UInt32 available = buf.mDataByteSize / sizeof(float);
                const UInt32 n = std::min(frames, available);
                for (UInt32 i = 0; i < n; ++i) {
                    dst[static_cast<size_t>(i) * channels + ch] = src[i];
                }
            }
        }

        if (self->outCapacityFrames < static_cast<int>(frames) + 64) {
            self->outInterleaved.resize(
                (static_cast<size_t>(frames) + 64) * self->channels.load());
            self->outCapacityFrames = static_cast<int>(frames) + 64;
        }

        uint8_t* inData = reinterpret_cast<uint8_t*>(dst);
        uint8_t* outData =
            reinterpret_cast<uint8_t*>(self->outInterleaved.data());
        const int converted = swr_convert(
            self->swr, &outData, self->outCapacityFrames, &inData, frames);
        if (converted <= 0) {
            return noErr;
        }

        AudioFrame frame;
        frame.source = AudioSource::System;
        frame.sampleRate = self->sampleRate.load();
        frame.channels = self->channels.load();
        frame.durationUs =
            static_cast<int64_t>(converted) * 1'000'000LL /
            (self->sampleRate.load() > 0 ? self->sampleRate.load() : 48000);
        const int64_t counter = self->frameCounter.fetch_add(converted);
        frame.ptsUs = counter * 1'000'000LL /
                      (self->sampleRate.load() > 0 ? self->sampleRate.load() : 48000);
        frame.samples.assign(
            self->outInterleaved.begin(),
            self->outInterleaved.begin() +
                static_cast<long>(converted) * self->channels.load());

        const float vol = self->paused.load() ? 0.0f : self->volume.load();
        if (vol != 1.0f) {
            for (float& s : frame.samples) {
                s *= vol;
            }
        }
        self->frames.fetch_add(1);
        self->samples.fetch_add(
            static_cast<long long>(frame.samples.size()));
        self->onFrame(std::move(frame));
        return noErr;
    }

    void destroyResources() {
        if (aggregateID != kAudioObjectUnknown && ioProcID) {
            AudioDeviceStop(aggregateID, ioProcID);
        }
        if (ioProcID) {
            AudioDeviceDestroyIOProcID(aggregateID, ioProcID);
            ioProcID = nullptr;
        }
        if (aggregateID != kAudioObjectUnknown) {
            AudioHardwareDestroyAggregateDevice(aggregateID);
            aggregateID = kAudioObjectUnknown;
        }
        if (tapID != kAudioObjectUnknown) {
            AudioHardwareDestroyProcessTap(tapID);
            tapID = kAudioObjectUnknown;
        }
        if (swr) {
            swr_free(&swr);
        }
        inInterleaved.clear();
        outInterleaved.clear();
    }
};

CoreAudioTapCapturer::CoreAudioTapCapturer()
    : impl_(std::make_unique<Impl>()) {}

CoreAudioTapCapturer::~CoreAudioTapCapturer() {
    stop();
}

bool CoreAudioTapCapturer::start(const AudioConfig& config,
                                 AudioFrameCallback onFrame) {
    if (!impl_) {
        return false;
    }
    stop();

    if (@available(macOS 14.2, *)) {
        impl_->sampleRate.store(config.sampleRate > 0 ? config.sampleRate : 48000);
        impl_->channels.store(config.channels > 0 ? config.channels : 2);
        impl_->volume.store(static_cast<float>(config.systemVolume) / 100.0f);
        impl_->paused.store(false);
        impl_->frameCounter.store(0);
        impl_->frames.store(0);
        impl_->samples.store(0);
        impl_->onFrame = std::move(onFrame);

        // Tap every process's audio output, mixed to stereo (system audio).
        CATapDescription* desc =
            [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:@[]];
        desc.name = @(kTapName);
        desc.privateTap = YES;

        AudioObjectID tapID = kAudioObjectUnknown;
        OSStatus status = AudioHardwareCreateProcessTap(desc, &tapID);
        if (status != noErr || tapID == kAudioObjectUnknown) {
            impl_->setupErrors.fetch_add(1);
            std::fprintf(stderr, "[CoreAudioTapCapturer] 创建系统音频 Tap 失败: %d\n",
                         static_cast<int>(status));
            impl_->onFrame = nullptr;
            return false;
        }
        impl_->tapID = tapID;

        // The tap's persistent UID is needed to attach it to the aggregate.
        NSString* tapUID = nil;
        {
            AudioObjectPropertyAddress addr = {
                kAudioTapPropertyUID,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain};
            CFStringRef uid = nullptr;
            UInt32 size = sizeof(uid);
            status = AudioObjectGetPropertyData(tapID, &addr, 0, nullptr, &size, &uid);
            if (status == noErr && uid) {
                tapUID = CFBridgingRelease(uid);
            }
        }
        if (!tapUID) {
            impl_->setupErrors.fetch_add(1);
            std::fprintf(stderr, "[CoreAudioTapCapturer] 无法读取 Tap UID\n");
            impl_->destroyResources();
            impl_->onFrame = nullptr;
            return false;
        }

        NSString* aggregateUID =
            [NSString stringWithFormat:@"com.notionrecorder.systemaudio.%@",
                                      [NSUUID UUID].UUIDString];
        NSDictionary* aggregateDesc = makeAggregateDescription(tapUID, aggregateUID);
        AudioObjectID aggregateID = kAudioObjectUnknown;
        status = AudioHardwareCreateAggregateDevice(
            (__bridge CFDictionaryRef)aggregateDesc, &aggregateID);
        if (status != noErr || aggregateID == kAudioObjectUnknown) {
            impl_->setupErrors.fetch_add(1);
            std::fprintf(stderr,
                         "[CoreAudioTapCapturer] 创建聚合设备失败: %d\n",
                         static_cast<int>(status));
            impl_->destroyResources();
            impl_->onFrame = nullptr;
            return false;
        }
        impl_->aggregateID = aggregateID;

        // The stream format as delivered by the aggregate device's input
        // (follows the output device's actual rate, e.g. 44.1k/48k/96k).
        // We resample to the configured 48k stereo below.
        AudioStreamBasicDescription tapFormat{};
        {
            AudioObjectPropertyAddress fmtAddr = {
                kAudioDevicePropertyStreamFormat,
                kAudioObjectPropertyScopeInput,
                1};
            UInt32 fmtSize = sizeof(tapFormat);
            status = AudioObjectGetPropertyData(aggregateID, &fmtAddr, 0, nullptr,
                                                &fmtSize, &tapFormat);
            if (status != noErr || tapFormat.mSampleRate <= 0) {
                impl_->setupErrors.fetch_add(1);
                std::fprintf(stderr,
                             "[CoreAudioTapCapturer] 读取聚合输入格式失败: %d\n",
                             static_cast<int>(status));
                impl_->destroyResources();
                impl_->onFrame = nullptr;
                return false;
            }
        }
        impl_->inputRate = static_cast<int>(tapFormat.mSampleRate);
        impl_->inputChannels =
            static_cast<int>(tapFormat.mChannelsPerFrame > 0
                                 ? tapFormat.mChannelsPerFrame
                                 : 2);

        // FLT interleaved (native rate/channels) -> FLT interleaved (48k, 2ch).
        AVChannelLayout inLayout;
        AVChannelLayout outLayout;
        av_channel_layout_default(&inLayout, impl_->inputChannels);
        av_channel_layout_default(&outLayout, impl_->channels.load());
        const int allocRet = swr_alloc_set_opts2(
            &impl_->swr, &outLayout, AV_SAMPLE_FMT_FLT, impl_->sampleRate.load(),
            &inLayout, AV_SAMPLE_FMT_FLT, impl_->inputRate, 0, nullptr);
        if (allocRet < 0 || !impl_->swr || swr_init(impl_->swr) < 0) {
            impl_->setupErrors.fetch_add(1);
            std::fprintf(stderr,
                         "[CoreAudioTapCapturer] 初始化音频重采样失败\n");
            impl_->destroyResources();
            impl_->onFrame = nullptr;
            return false;
        }
        impl_->outCapacityFrames = 4096;
        impl_->outInterleaved.resize(
            static_cast<size_t>(impl_->outCapacityFrames) *
            impl_->channels.load());

        status = AudioDeviceCreateIOProcID(aggregateID, &Impl::ioProc,
                                           impl_.get(), &impl_->ioProcID);
        if (status != noErr || !impl_->ioProcID) {
            impl_->setupErrors.fetch_add(1);
            std::fprintf(stderr,
                         "[CoreAudioTapCapturer] 注册 IO 回调失败: %d\n",
                         static_cast<int>(status));
            impl_->destroyResources();
            impl_->onFrame = nullptr;
            return false;
        }
        status = AudioDeviceStart(aggregateID, impl_->ioProcID);
        if (status != noErr) {
            impl_->setupErrors.fetch_add(1);
            std::fprintf(stderr,
                         "[CoreAudioTapCapturer] 启动聚合设备失败: %d\n",
                         static_cast<int>(status));
            impl_->destroyResources();
            impl_->onFrame = nullptr;
            return false;
        }

        // Diagnostics: confirm the tap is attached to the aggregate.
        AudioObjectPropertyAddress tapListAddr = {
            kAudioAggregateDevicePropertySubTapList,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain};
        UInt32 tapListSize = 0;
        OSStatus tapListStatus = AudioObjectGetPropertyDataSize(
            aggregateID, &tapListAddr, 0, nullptr, &tapListSize);
        if (tapListStatus == noErr && tapListSize > 0) {
            std::vector<AudioObjectID> tapList(tapListSize / sizeof(AudioObjectID));
            AudioObjectGetPropertyData(aggregateID, &tapListAddr, 0, nullptr,
                                       &tapListSize, tapList.data());
            std::fprintf(stderr,
                         "[CoreAudioTapCapturer] 聚合设备挂载 %zu 个 tap\n",
                         tapList.size());
        } else {
            std::fprintf(stderr,
                         "[CoreAudioTapCapturer] 聚合设备 tap 列表查询失败: %d\n",
                         static_cast<int>(tapListStatus));
        }

        impl_->started.store(true);
        std::fprintf(stderr,
                     "[CoreAudioTapCapturer] 系统音频 Tap 已启动 "
                     "(native %dHz %dch -> %dHz %dch)\n",
                     impl_->inputRate, impl_->inputChannels,
                     impl_->sampleRate.load(), impl_->channels.load());
        return true;
    }

    impl_->onFrame = nullptr;
    return false;
}

void CoreAudioTapCapturer::stop() {
    if (!impl_) {
        return;
    }
    impl_->started.store(false);
    impl_->destroyResources();
    impl_->onFrame = nullptr;
}

void CoreAudioTapCapturer::setPaused(bool paused) {
    if (impl_) {
        impl_->paused.store(paused);
    }
}

CoreAudioTapCapturer::Stats CoreAudioTapCapturer::stats() const {
    Stats s;
    if (impl_) {
        s.frames = impl_->frames.load();
        s.samples = impl_->samples.load();
        s.setupErrors = impl_->setupErrors.load();
        s.started = impl_->started.load();
    }
    return s;
}

} // namespace nr
