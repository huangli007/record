#pragma once

#include <cstddef>
#include <vector>

namespace nr {

// Real-time spectral-subtraction noise suppressor for one mono channel.
// Built on the Accelerate vDSP FFT; intended for microphone pre-processing
// on the capture thread (pre-allocated, no per-call heap allocation after
// construction except the fixed internal buffers).
class Denoiser {
public:
    explicit Denoiser(int sampleRate = 48000, size_t fftSize = 1024);
    ~Denoiser();

    Denoiser(const Denoiser&) = delete;
    Denoiser& operator=(const Denoiser&) = delete;

    // Consumes frameCount input samples and produces frameCount output
    // samples. Algorithmic latency is (fftSize - hop) samples.
    void process(const float* input, float* output, size_t frameCount);

private:
    void init();
    void processOneHop();

    size_t fftSize_;
    size_t hop_;
    int log2Size_;

    void* fftSetup_ = nullptr;   // vDSP_FFT_Setup
    std::vector<float> window_;
    std::vector<float> inputWindow_;   // last fftSize input samples
    std::vector<float> overlap_;       // overlap-add accumulator
    std::vector<float> spectrumReal_;
    std::vector<float> spectrumImag_;
    std::vector<float> magnitude_;
    std::vector<float> smoothedMag_;
    std::vector<float> noiseEstimate_;
    std::vector<float> gainSmooth_;
    std::vector<float> pendingOut_;    // ring buffer of produced samples
    size_t inputFill_ = 0;
    size_t pendingHead_ = 0;
    size_t pendingCount_ = 0;
    size_t hops_ = 0;                  // warm-up counter

    const float spectralFloor_ = 0.05f;
    const float noiseSmooth_ = 0.92f;
    const float magSmooth_ = 0.85f;
    const float attackCoeff_ = 0.45f;   // fast: let speech through
    const float releaseCoeff_ = 0.08f;  // slow: keep noise suppressed
};

} // namespace nr
