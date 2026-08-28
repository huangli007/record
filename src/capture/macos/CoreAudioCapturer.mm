#include "capture/macos/CoreAudioCapturer.h"

#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "audio/Denoiser.h"
#include "capture/AudioFrame.h"
#include "core/TimeBase.h"

namespace nr {

struct CoreAudioCapturer::Impl {
    AudioComponentInstance unit = nullptr;
    AudioFrameCallback onFrame;
    std::atomic<bool> paused{false};
    std::atomic<bool> started{false};
    std::atomic<float> volume{1.0f};
    std::atomic<int> sampleRate{48000};
    std::atomic<int> channels{2};
    std::atomic<int64_t> frameCounter{0};
    AudioConfig config;
    std::mutex restartMutex;
    dispatch_queue_t restartQueue = nullptr;
    bool listenerRegistered = false;
    std::unique_ptr<Denoiser> denoiser;
    bool denoiseEnabled = false;
    std::vector<float> monoScratch;
    std::vector<float> ch0;
    std::vector<float> ch1;
    std::vector<float> out0;
    std::vector<float> out1;

    static OSStatus deviceChangedListener(AudioObjectID /*inObjectID*/,
                                          UInt32 /*inNumberAddresses*/,
                                          const AudioObjectPropertyAddress /*inAddresses*/[],
                                          void* inClientData);

    bool startUnit() {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Output;
        desc.componentSubType = kAudioUnitSubType_HALOutput;
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;
        AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
        if (!comp) {
            return false;
        }
        OSStatus status = AudioComponentInstanceNew(comp, &unit);
        if (status != noErr) {
            return false;
        }

        UInt32 one = 1;
        UInt32 zero = 0;
        AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
                             kAudioUnitScope_Output, 0, &zero, sizeof(zero));
        AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
                             kAudioUnitScope_Input, 1, &one, sizeof(one));

        AudioDeviceID inputDevice = kAudioObjectUnknown;
        UInt32 size = sizeof(inputDevice);
        AudioObjectPropertyAddress addr = {
            kAudioHardwarePropertyDefaultInputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain};
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
                                       &size, &inputDevice) != noErr ||
            inputDevice == kAudioObjectUnknown) {
            AudioComponentInstanceDispose(unit);
            unit = nullptr;
            return false;
        }
        AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                             kAudioUnitScope_Global, 0, &inputDevice, sizeof(inputDevice));

        const int sampleRate = config.sampleRate > 0 ? config.sampleRate : 48000;
        const int channels = config.channels > 0 ? config.channels : 2;
        AudioStreamBasicDescription fmt{};
        fmt.mSampleRate = sampleRate;
        fmt.mFormatID = kAudioFormatLinearPCM;
        fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        fmt.mChannelsPerFrame = channels;
        fmt.mBitsPerChannel = 32;
        fmt.mBytesPerFrame = channels * sizeof(float);
        fmt.mBytesPerPacket = fmt.mBytesPerFrame;
        fmt.mFramesPerPacket = 1;
        AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));

        AURenderCallbackStruct callback{};
        callback.inputProc = &Impl::render;
        callback.inputProcRefCon = this;
        AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Input, 0, &callback, sizeof(callback));

        status = AudioUnitInitialize(unit);
        if (status != noErr) {
            stopUnit();
            return false;
        }
        status = AudioOutputUnitStart(unit);
        if (status != noErr) {
            stopUnit();
            return false;
        }
        return true;
    }

    void stopUnit() {
        if (unit) {
            AudioOutputUnitStop(unit);
            AudioUnitUninitialize(unit);
            AudioComponentInstanceDispose(unit);
            unit = nullptr;
        }
    }

    void handleDeviceChanged() {
        if (!started.load()) {
            return;
        }
        if (!restartQueue) {
            return;
        }
        dispatch_async(restartQueue, ^{
            std::lock_guard<std::mutex> lock(restartMutex);
            if (!started.load()) {
                return;
            }
            stopUnit();
            if (!startUnit()) {
                // No default input device right now; the next change
                // notification will retry.
                std::fprintf(stderr,
                             "[CoreAudioCapturer] 默认输入设备不可用，等待恢复\n");
            }
        });
    }

    static OSStatus render(void* inRefCon,
                           AudioUnitRenderActionFlags* ioActionFlags,
                           const AudioTimeStamp* inTimeStamp,
                           UInt32 inBusNumber,
                           UInt32 inNumberFrames,
                           AudioBufferList* ioData) {
        auto* self = static_cast<Impl*>(inRefCon);
        if (!self || !self->unit) {
            return noErr;
        }

        AudioBufferList bufferList{};
        bufferList.mNumberBuffers = 1;
        bufferList.mBuffers[0].mNumberChannels = static_cast<UInt32>(self->channels.load());
        bufferList.mBuffers[0].mDataByteSize =
            inNumberFrames * bufferList.mBuffers[0].mNumberChannels * sizeof(float);
        std::vector<float> scratch(bufferList.mBuffers[0].mDataByteSize / sizeof(float), 0.0f);
        bufferList.mBuffers[0].mData = scratch.data();

        OSStatus status = AudioUnitRender(self->unit, ioActionFlags, inTimeStamp,
                                          inBusNumber, inNumberFrames, &bufferList);
        if (status != noErr || !self->onFrame) {
            return status;
        }

        AudioFrame frame;
        frame.source = AudioSource::Microphone;
        frame.sampleRate = self->sampleRate.load();
        frame.channels = self->channels.load();
        frame.durationUs =
            static_cast<int64_t>(inNumberFrames) * 1'000'000LL / self->sampleRate.load();

        const int64_t counter = self->frameCounter.fetch_add(inNumberFrames);
        frame.ptsUs = counter * 1'000'000LL / self->sampleRate.load();

        const float vol = self->paused.load() ? 0.0f : self->volume.load();
        frame.samples.assign(scratch.begin(), scratch.end());
        if (self->denoiseEnabled && self->denoiser && !frame.samples.empty()) {
            const size_t n = inNumberFrames;
            if (frame.channels == 2) {
                for (size_t i = 0; i < n; ++i) {
                    self->ch0[i] = frame.samples[i * 2];
                    self->ch1[i] = frame.samples[i * 2 + 1];
                }
                self->denoiser->process(self->ch0.data(), self->out0.data(), n);
                self->denoiser->process(self->ch1.data(), self->out1.data(), n);
                for (size_t i = 0; i < n; ++i) {
                    frame.samples[i * 2] = self->out0[i];
                    frame.samples[i * 2 + 1] = self->out1[i];
                }
            } else if (frame.channels == 1) {
                self->denoiser->process(frame.samples.data(),
                                        self->monoScratch.data(), n);
                std::copy(self->monoScratch.begin(),
                          self->monoScratch.begin() + static_cast<long>(n),
                          frame.samples.begin());
            }
        }
        if (vol != 1.0f) {
            for (float& s : frame.samples) {
                s *= vol;
            }
        }

        self->onFrame(std::move(frame));
        return noErr;
    }
};

// Called on CoreAudio's property-listener thread when the default input
// device changes (device plugged/unplugged). Rebuilds the capture unit.
OSStatus CoreAudioCapturer::Impl::deviceChangedListener(
    AudioObjectID, UInt32, const AudioObjectPropertyAddress[], void* inClientData) {
    auto* impl = static_cast<CoreAudioCapturer::Impl*>(inClientData);
    if (impl) {
        impl->handleDeviceChanged();
    }
    return noErr;
}

CoreAudioCapturer::CoreAudioCapturer() : impl_(std::make_unique<Impl>()) {}

CoreAudioCapturer::~CoreAudioCapturer() {
    stop();
}

bool CoreAudioCapturer::start(const AudioConfig& config, AudioFrameCallback onFrame) {
    if (!impl_) {
        return false;
    }
    stop();

    impl_->config = config;
    const int sampleRate = config.sampleRate > 0 ? config.sampleRate : 48000;
    const int channels = config.channels > 0 ? config.channels : 2;
    impl_->sampleRate.store(sampleRate);
    impl_->channels.store(channels);
    impl_->volume.store(static_cast<float>(config.micVolume) / 100.0f);
    impl_->paused.store(false);
    impl_->frameCounter.store(0);
    impl_->onFrame = std::move(onFrame);
    impl_->denoiseEnabled = config.denoise;
    if (impl_->denoiseEnabled) {
        impl_->denoiser = std::make_unique<Denoiser>(sampleRate);
        impl_->monoScratch.resize(4096);
        impl_->ch0.resize(4096);
        impl_->ch1.resize(4096);
        impl_->out0.resize(4096);
        impl_->out1.resize(4096);
    } else {
        impl_->denoiser.reset();
    }

    if (!impl_->restartQueue) {
        impl_->restartQueue =
            dispatch_queue_create("com.notionrecorder.coreaudio", DISPATCH_QUEUE_SERIAL);
    }

    if (!impl_->startUnit()) {
        impl_->stopUnit();
        impl_->onFrame = nullptr;
        return false;
    }
    impl_->started.store(true);

    // Watch for default input device changes (headset plug/unplug, etc.).
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDefaultInputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain};
    if (!impl_->listenerRegistered) {
        const OSStatus listenerStatus = AudioObjectAddPropertyListener(
            kAudioObjectSystemObject, &addr,
            &CoreAudioCapturer::Impl::deviceChangedListener,
            impl_.get());
        impl_->listenerRegistered = listenerStatus == noErr;
    }
    return true;
}

void CoreAudioCapturer::stop() {
    if (!impl_) {
        return;
    }
    impl_->started.store(false);
    if (impl_->listenerRegistered) {
        AudioObjectPropertyAddress addr = {
            kAudioHardwarePropertyDefaultInputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain};
        AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &addr,
                                          &CoreAudioCapturer::Impl::deviceChangedListener,
                                          impl_.get());
        impl_->listenerRegistered = false;
    }
    impl_->stopUnit();
    impl_->onFrame = nullptr;
    impl_->denoiser.reset();
    impl_->denoiseEnabled = false;
}

void CoreAudioCapturer::setPaused(bool paused) {
    if (impl_) {
        impl_->paused.store(paused);
    }
}

} // namespace nr
