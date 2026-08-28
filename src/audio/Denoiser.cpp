#include "audio/Denoiser.h"

#include <algorithm>
#include <cmath>

#include <Accelerate/Accelerate.h>

namespace nr {

namespace {
constexpr size_t kWarmupHops = 16;
}

Denoiser::Denoiser(int sampleRate, size_t fftSize)
    : fftSize_(fftSize), hop_(fftSize / 2) {
    (void)sampleRate;
    log2Size_ = static_cast<int>(std::log2(static_cast<double>(fftSize_)));
    init();
}

Denoiser::~Denoiser() {
    if (fftSetup_) {
        vDSP_destroy_fftsetup(static_cast<FFTSetup>(fftSetup_));
    }
}

void Denoiser::init() {
    fftSetup_ = vDSP_create_fftsetup(log2Size_, kFFTRadix2);
    window_.resize(fftSize_);
    inputWindow_.assign(fftSize_, 0.0f);
    overlap_.assign(fftSize_, 0.0f);
    spectrumReal_.assign(fftSize_ / 2, 0.0f);
    spectrumImag_.assign(fftSize_ / 2, 0.0f);
    magnitude_.assign(fftSize_ / 2, 0.0f);
    smoothedMag_.assign(fftSize_ / 2, 0.0f);
    noiseEstimate_.assign(fftSize_ / 2, 0.0f);  // warm-up mean accumulation
    gainSmooth_.assign(fftSize_ / 2, 1.0f);
    pendingOut_.assign(fftSize_ * 2, 0.0f);

    // Hann window.
    for (size_t i = 0; i < fftSize_; ++i) {
        window_[i] = 0.5f - 0.5f * std::cos(2.0 * M_PI * i / (fftSize_ - 1));
    }
}

void Denoiser::process(const float* input, float* output, size_t frameCount) {
    for (size_t i = 0; i < frameCount; ++i) {
        inputWindow_[inputFill_++] = input[i];
        if (inputFill_ == fftSize_) {
            processOneHop();
            // Keep the tail of the window for the next hop.
            std::copy(inputWindow_.begin() + hop_, inputWindow_.end(),
                      inputWindow_.begin());
            inputFill_ = hop_;
        }
        if (pendingCount_ > 0) {
            output[i] = pendingOut_[pendingHead_];
            pendingHead_ = (pendingHead_ + 1) % pendingOut_.size();
            --pendingCount_;
        } else {
            output[i] = 0.0f;  // startup latency
        }
    }
}

void Denoiser::processOneHop() {
    // Windowed analysis. vDSP_fft_zrip packs the real input as N/2 complex
    // samples: realp[i] = x[2i], imagp[i] = x[2i+1].
    for (size_t i = 0; i < fftSize_ / 2; ++i) {
        spectrumReal_[i] = window_[2 * i] * inputWindow_[2 * i];
        spectrumImag_[i] = window_[2 * i + 1] * inputWindow_[2 * i + 1];
    }

    DSPSplitComplex split{spectrumReal_.data(), spectrumImag_.data()};
    vDSP_fft_zrip(static_cast<FFTSetup>(fftSetup_), &split, 1, log2Size_,
                  FFT_FORWARD);

    // Per-bin magnitude and temporal smoothing (reduces musical noise).
    vDSP_zvabs(&split, 1, magnitude_.data(), 1, fftSize_ / 2);
    const float smooth = 1.0f - magSmooth_;
    vDSP_vsmul(smoothedMag_.data(), 1, &magSmooth_, smoothedMag_.data(), 1,
               fftSize_ / 2);
    vDSP_vsma(magnitude_.data(), 1, &smooth, smoothedMag_.data(), 1,
              smoothedMag_.data(), 1, fftSize_ / 2);  // mag*0.5 + prev*0.5

    ++hops_;
    const bool warmup = hops_ <= kWarmupHops;
    const bool finalizeWarmup = hops_ == kWarmupHops + 1;
    for (size_t i = 0; i < fftSize_ / 2; ++i) {
        const float mag = smoothedMag_[i];
        float& noise = noiseEstimate_[i];
        if (warmup) {
            noise += magnitude_[i];  // accumulate raw magnitudes; averaged later
            continue;
        }
        if (finalizeWarmup) {
            noise = std::max(noise / static_cast<float>(kWarmupHops), 1e-6f);
        }
        if (mag < noise) {
            noise = noiseSmooth_ * noise + (1.0f - noiseSmooth_) * mag;
        } else if (mag < 3.0f * noise) {
            // Slow upward tracking toward the local noise level; bins at or
            // above 3x the floor are treated as speech and left untouched.
            noise += 0.01f * (mag - noise);
        }
        // Noise gate with a soft transition band:
        //   mag < 1.5*noise  -> spectral floor
        //   mag > 3.0*noise  -> pass through
        //   in between       -> linear ramp
        float target;
        const float ratio = mag / std::max(noise, 1e-6f);
        if (ratio < 1.5f) {
            target = spectralFloor_;
        } else if (ratio < 3.0f) {
            target = spectralFloor_ + (1.0f - spectralFloor_) * (ratio - 1.5f) / 1.5f;
        } else {
            target = 1.0f;
        }
        // Asymmetric smoothing: fast attack, slow release.
        float& g = gainSmooth_[i];
        const float coeff = target > g ? attackCoeff_ : releaseCoeff_;
        g += coeff * (target - g);
        spectrumReal_[i] *= g;
        spectrumImag_[i] *= g;
    }

    vDSP_fft_zrip(static_cast<FFTSetup>(fftSetup_), &split, 1, log2Size_,
                  FFT_INVERSE);

    // Inverse output is scaled by 0.5 by vDSP; unnormalised DFT round-trip
    // leaves an extra factor of N, and Hann 50%-overlap adds 0.75.
    const float scale = 0.5f / (0.75f * static_cast<float>(fftSize_));
    vDSP_vsmul(spectrumReal_.data(), 1, &scale, spectrumReal_.data(), 1,
               fftSize_ / 2);
    vDSP_vsmul(spectrumImag_.data(), 1, &scale, spectrumImag_.data(), 1,
               fftSize_ / 2);

    // Synthesis window + overlap-add; emit one hop.
    // Unpack the inverse output: time[2i] = realp[i], time[2i+1] = imagp[i].
    std::vector<float> time(fftSize_);
    for (size_t i = 0; i < fftSize_ / 2; ++i) {
        time[2 * i] = spectrumReal_[i];
        time[2 * i + 1] = spectrumImag_[i];
    }
    vDSP_vmul(time.data(), 1, window_.data(), 1, time.data(), 1, fftSize_);
    vDSP_vadd(overlap_.data(), 1, time.data(), 1, overlap_.data(), 1, fftSize_);

    const size_t cap = pendingOut_.size();
    for (size_t i = 0; i < hop_; ++i) {
        pendingOut_[(pendingHead_ + pendingCount_ + i) % cap] = overlap_[i];
    }
    pendingCount_ += hop_;
    std::copy(overlap_.begin() + hop_, overlap_.end(), overlap_.begin());
    std::fill(overlap_.begin() + (fftSize_ - hop_), overlap_.end(), 0.0f);
}

} // namespace nr
