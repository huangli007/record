#include "audio/Denoiser.h"

#include <algorithm>
#include <cmath>
#include <vector>

#if defined(__APPLE__)
#include <Accelerate/Accelerate.h>
#else
namespace {
struct Complex {
    float re = 0.0f;
    float im = 0.0f;
};

// Iterative radix-2 complex FFT. Forward is unscaled; inverse divides by N.
void fftRadix2(std::vector<Complex>& a, bool inverse) {
    const int n = static_cast<int>(a.size());
    if (n <= 1) {
        return;
    }
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float ang =
            (inverse ? 2.0f : -2.0f) * 3.14159265358979323846f /
            static_cast<float>(len);
        const float wRe = std::cos(ang);
        const float wIm = std::sin(ang);
        for (int i = 0; i < n; i += len) {
            float curRe = 1.0f;
            float curIm = 0.0f;
            for (int k = 0; k < len / 2; ++k) {
                const float uRe = a[i + k].re;
                const float uIm = a[i + k].im;
                const float vRe =
                    a[i + k + len / 2].re * curRe - a[i + k + len / 2].im * curIm;
                const float vIm =
                    a[i + k + len / 2].re * curIm + a[i + k + len / 2].im * curRe;
                a[i + k].re = uRe + vRe;
                a[i + k].im = uIm + vIm;
                a[i + k + len / 2].re = uRe - vRe;
                a[i + k + len / 2].im = uIm - vIm;
                const float nextRe = curRe * wRe - curIm * wIm;
                curIm = curRe * wIm + curIm * wRe;
                curRe = nextRe;
            }
        }
    }
    if (inverse) {
        for (Complex& c : a) {
            c.re /= static_cast<float>(n);
            c.im /= static_cast<float>(n);
        }
    }
}
} // namespace
#endif

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
#if defined(__APPLE__)
    if (fftSetup_) {
        vDSP_destroy_fftsetup(static_cast<FFTSetup>(fftSetup_));
    }
#endif
}

void Denoiser::init() {
#if defined(__APPLE__)
    fftSetup_ = vDSP_create_fftsetup(log2Size_, kFFTRadix2);
#endif
    window_.resize(fftSize_);
    inputWindow_.assign(fftSize_, 0.0f);
    overlap_.assign(fftSize_, 0.0f);
    spectrumReal_.assign(fftSize_ / 2, 0.0f);
    spectrumImag_.assign(fftSize_ / 2, 0.0f);
    magnitude_.assign(fftSize_ / 2, 0.0f);
    smoothedMag_.assign(fftSize_ / 2, 0.0f);
    noiseEstimate_.assign(fftSize_ / 2, 0.0f);
    gainSmooth_.assign(fftSize_ / 2, 1.0f);
    pendingOut_.assign(fftSize_ * 2, 0.0f);

    for (size_t i = 0; i < fftSize_; ++i) {
        window_[i] =
            0.5f - 0.5f * std::cos(2.0 * 3.14159265358979323846 * i /
                                   (fftSize_ - 1));
    }
}

void Denoiser::process(const float* input, float* output, size_t frameCount) {
    for (size_t i = 0; i < frameCount; ++i) {
        inputWindow_[inputFill_++] = input[i];
        if (inputFill_ == fftSize_) {
            processOneHop();
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

#if defined(__APPLE__)
void Denoiser::processOneHop() {
    for (size_t i = 0; i < fftSize_ / 2; ++i) {
        spectrumReal_[i] = window_[2 * i] * inputWindow_[2 * i];
        spectrumImag_[i] = window_[2 * i + 1] * inputWindow_[2 * i + 1];
    }

    DSPSplitComplex split{spectrumReal_.data(), spectrumImag_.data()};
    vDSP_fft_zrip(static_cast<FFTSetup>(fftSetup_), &split, 1, log2Size_,
                  FFT_FORWARD);

    vDSP_zvabs(&split, 1, magnitude_.data(), 1, fftSize_ / 2);
    const float smooth = 1.0f - magSmooth_;
    vDSP_vsmul(smoothedMag_.data(), 1, &magSmooth_, smoothedMag_.data(), 1,
               fftSize_ / 2);
    vDSP_vsma(magnitude_.data(), 1, &smooth, smoothedMag_.data(), 1,
              smoothedMag_.data(), 1, fftSize_ / 2);

    ++hops_;
    const bool warmup = hops_ <= kWarmupHops;
    const bool finalizeWarmup = hops_ == kWarmupHops + 1;
    for (size_t i = 0; i < fftSize_ / 2; ++i) {
        const float mag = smoothedMag_[i];
        float& noise = noiseEstimate_[i];
        if (warmup) {
            noise += magnitude_[i];
            continue;
        }
        if (finalizeWarmup) {
            noise = std::max(noise / static_cast<float>(kWarmupHops), 1e-6f);
        }
        if (mag < noise) {
            noise = noiseSmooth_ * noise + (1.0f - noiseSmooth_) * mag;
        } else if (mag < 3.0f * noise) {
            noise += 0.01f * (mag - noise);
        }
        float target;
        const float ratio = mag / std::max(noise, 1e-6f);
        if (ratio < 1.5f) {
            target = spectralFloor_;
        } else if (ratio < 3.0f) {
            target = spectralFloor_ + (1.0f - spectralFloor_) * (ratio - 1.5f) / 1.5f;
        } else {
            target = 1.0f;
        }
        float& g = gainSmooth_[i];
        const float coeff = target > g ? attackCoeff_ : releaseCoeff_;
        g += coeff * (target - g);
        spectrumReal_[i] *= g;
        spectrumImag_[i] *= g;
    }

    vDSP_fft_zrip(static_cast<FFTSetup>(fftSetup_), &split, 1, log2Size_,
                  FFT_INVERSE);

    const float scale = 0.5f / (0.75f * static_cast<float>(fftSize_));
    vDSP_vsmul(spectrumReal_.data(), 1, &scale, spectrumReal_.data(), 1,
               fftSize_ / 2);
    vDSP_vsmul(spectrumImag_.data(), 1, &scale, spectrumImag_.data(), 1,
               fftSize_ / 2);

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
#else
void Denoiser::processOneHop() {
    std::vector<Complex> spectrum(fftSize_);
    for (size_t i = 0; i < fftSize_; ++i) {
        spectrum[i] = {window_[i] * inputWindow_[i], 0.0f};
    }
    fftRadix2(spectrum, false);

    const size_t half = fftSize_ / 2;
    for (size_t i = 0; i < half; ++i) {
        magnitude_[i] =
            std::sqrt(spectrum[i].re * spectrum[i].re +
                      spectrum[i].im * spectrum[i].im);
        smoothedMag_[i] =
            magSmooth_ * smoothedMag_[i] + (1.0f - magSmooth_) * magnitude_[i];
    }

    ++hops_;
    const bool warmup = hops_ <= kWarmupHops;
    const bool finalizeWarmup = hops_ == kWarmupHops + 1;
    for (size_t i = 0; i < half; ++i) {
        const float mag = smoothedMag_[i];
        float& noise = noiseEstimate_[i];
        if (warmup) {
            noise += magnitude_[i];
            continue;
        }
        if (finalizeWarmup) {
            noise = std::max(noise / static_cast<float>(kWarmupHops), 1e-6f);
        }
        if (mag < noise) {
            noise = noiseSmooth_ * noise + (1.0f - noiseSmooth_) * mag;
        } else if (mag < 3.0f * noise) {
            noise += 0.01f * (mag - noise);
        }
        float target;
        const float ratio = mag / std::max(noise, 1e-6f);
        if (ratio < 1.5f) {
            target = spectralFloor_;
        } else if (ratio < 3.0f) {
            target = spectralFloor_ + (1.0f - spectralFloor_) * (ratio - 1.5f) / 1.5f;
        } else {
            target = 1.0f;
        }
        float& g = gainSmooth_[i];
        const float coeff = target > g ? attackCoeff_ : releaseCoeff_;
        g += coeff * (target - g);
        spectrum[i].re *= g;
        spectrum[i].im *= g;
    }
    // Keep the spectrum conjugate-symmetric (real time-domain signal).
    for (size_t i = 1; i < half; ++i) {
        spectrum[fftSize_ - i] = {spectrum[i].re, -spectrum[i].im};
    }
    spectrum[half].im = 0.0f;
    fftRadix2(spectrum, true);

    for (size_t i = 0; i < fftSize_; ++i) {
        overlap_[i] += window_[i] * spectrum[i].re;
    }

    const size_t cap = pendingOut_.size();
    for (size_t i = 0; i < hop_; ++i) {
        pendingOut_[(pendingHead_ + pendingCount_ + i) % cap] = overlap_[i];
    }
    pendingCount_ += hop_;
    std::copy(overlap_.begin() + hop_, overlap_.end(), overlap_.begin());
    std::fill(overlap_.begin() + (fftSize_ - hop_), overlap_.end(), 0.0f);
}
#endif

} // namespace nr
