#include "capture/macos/CoreAudioCapturer.h"

#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>

#include <atomic>
#include <cmath>
#include <functional>
#include <vector>

#include "capture/AudioFrame.h"
#include "core/TimeBase.h"

namespace nr {

struct CoreAudioCapturer::Impl {
    AudioComponentInstance unit = nullptr;
    AudioFrameCallback onFrame;
    std::atomic<bool> paused{false};
    std::atomic<float> volume{1.0f};
    std::atomic<int> sampleRate{48000};
    std::atomic<int> channels{2};
    std::atomic<int64_t> frameCounter{0};
    bool started = false;

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
        if (vol != 1.0f) {
            for (float& s : frame.samples) {
                s *= vol;
            }
        }

        self->onFrame(std::move(frame));
        return noErr;
    }
};

CoreAudioCapturer::CoreAudioCapturer() : impl_(std::make_unique<Impl>()) {}

CoreAudioCapturer::~CoreAudioCapturer() {
    stop();
}

bool CoreAudioCapturer::start(const AudioConfig& config, AudioFrameCallback onFrame) {
    if (!impl_) {
        return false;
    }
    stop();

    const int sampleRate = config.sampleRate > 0 ? config.sampleRate : 48000;
    const int channels = config.channels > 0 ? config.channels : 2;
    impl_->sampleRate.store(sampleRate);
    impl_->channels.store(channels);
    impl_->volume.store(static_cast<float>(config.micVolume) / 100.0f);
    impl_->paused.store(false);
    impl_->frameCounter.store(0);
    impl_->onFrame = std::move(onFrame);

    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp) {
        return false;
    }
    OSStatus status = AudioComponentInstanceNew(comp, &impl_->unit);
    if (status != noErr) {
        return false;
    }

    // Disable output element so we only capture the microphone input.
    UInt32 one = 1;
    UInt32 zero = 0;
    AudioUnitSetProperty(impl_->unit, kAudioOutputUnitProperty_EnableIO,
                         kAudioUnitScope_Output, 0, &zero, sizeof(zero));
    AudioUnitSetProperty(impl_->unit, kAudioOutputUnitProperty_EnableIO,
                         kAudioUnitScope_Input, 1, &one, sizeof(one));

    // Default input device.
    AudioDeviceID inputDevice = kAudioObjectUnknown;
    UInt32 size = sizeof(inputDevice);
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDefaultInputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
                                   &size, &inputDevice) != noErr ||
        inputDevice == kAudioObjectUnknown) {
        AudioComponentInstanceDispose(impl_->unit);
        impl_->unit = nullptr;
        return false;
    }
    AudioUnitSetProperty(impl_->unit, kAudioOutputUnitProperty_CurrentDevice,
                         kAudioUnitScope_Global, 0, &inputDevice, sizeof(inputDevice));

    // Client format: interleaved Float32.
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = sampleRate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = channels;
    fmt.mBitsPerChannel = 32;
    fmt.mBytesPerFrame = channels * sizeof(float);
    fmt.mBytesPerPacket = fmt.mBytesPerFrame;
    fmt.mFramesPerPacket = 1;
    AudioUnitSetProperty(impl_->unit, kAudioUnitProperty_StreamFormat,
                         kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));

    AURenderCallbackStruct callback{};
    callback.inputProc = &Impl::render;
    callback.inputProcRefCon = impl_.get();
    AudioUnitSetProperty(impl_->unit, kAudioUnitProperty_SetRenderCallback,
                         kAudioUnitScope_Input, 0, &callback, sizeof(callback));

    status = AudioUnitInitialize(impl_->unit);
    if (status != noErr) {
        AudioComponentInstanceDispose(impl_->unit);
        impl_->unit = nullptr;
        return false;
    }

    status = AudioOutputUnitStart(impl_->unit);
    if (status != noErr) {
        AudioUnitUninitialize(impl_->unit);
        AudioComponentInstanceDispose(impl_->unit);
        impl_->unit = nullptr;
        return false;
    }
    impl_->started = true;
    return true;
}

void CoreAudioCapturer::stop() {
    if (!impl_) {
        return;
    }
    if (impl_->unit) {
        AudioOutputUnitStop(impl_->unit);
        AudioUnitUninitialize(impl_->unit);
        AudioComponentInstanceDispose(impl_->unit);
        impl_->unit = nullptr;
    }
    impl_->started = false;
    impl_->onFrame = nullptr;
}

void CoreAudioCapturer::setPaused(bool paused) {
    if (impl_) {
        impl_->paused.store(paused);
    }
}

} // namespace nr
