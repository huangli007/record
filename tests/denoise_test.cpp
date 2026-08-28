#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "audio/Denoiser.h"

using namespace nr;

namespace {

constexpr int kRate = 48000;
constexpr size_t kFftSize = 1024;
constexpr size_t kLatency = kFftSize - 1;  // algorithmic delay (samples)

double rms(const float* data, size_t count) {
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) {
        sum += static_cast<double>(data[i]) * data[i];
    }
    return std::sqrt(sum / static_cast<double>(count));
}

double snrDb(const float* clean, const float* processed, size_t count) {
    double signal = 0.0;
    double error = 0.0;
    for (size_t i = 0; i < count; ++i) {
        signal += static_cast<double>(clean[i]) * clean[i];
        const double e = static_cast<double>(processed[i]) - clean[i];
        error += e * e;
    }
    if (signal <= 0.0 || error <= 0.0) {
        return 0.0;
    }
    return 10.0 * std::log10(signal / error);
}

// Best SNR over a small lag window around the algorithmic latency.
double bestSnrDb(const std::vector<float>& out, const std::vector<float>& clean,
                 size_t start, size_t count) {
    double best = -1e9;
    for (int lag = -static_cast<int>(kLatency) - 256;
         lag <= -static_cast<int>(kLatency) + 256; lag += 4) {
        const long from = static_cast<long>(start) + lag;
        if (from < 0 || from + static_cast<long>(count) >
                            static_cast<long>(clean.size())) {
            continue;
        }
        const double snr = snrDb(clean.data() + from, out.data() + start, count);
        if (snr > best) {
            best = snr;
        }
    }
    return best;
}

} // namespace

int main() {
    constexpr double seconds = 2.0;
    constexpr size_t total = static_cast<size_t>(kRate * seconds);
    constexpr double freq = 1000.0;
    constexpr size_t noiseOnly = static_cast<size_t>(kRate * 0.5);  // warm-up

    // ---------- Part 1: steady noise attenuation ----------
    {
        std::vector<float> noise(total);
        std::mt19937 rng(7);
        std::normal_distribution<float> dist(0.0f, 0.5f);
        for (auto& v : noise) {
            v = dist(rng);
        }
        std::vector<float> out(total);
        Denoiser denoiser(kRate);
        denoiser.process(noise.data(), out.data(), total);

        const size_t s = total - kRate / 2;
        const double in = rms(noise.data() + s, kRate / 2);
        const double outRms = rms(out.data() + s, kRate / 2);
        const double att = 20.0 * std::log10(in / std::max(outRms, 1e-6));
        std::printf("noise attenuation: %.1f dB\n", att);
        if (att < 12.0) {
            std::fprintf(stderr, "noise attenuation too weak\n");
            return 1;
        }
    }

    // ---------- Part 2: signal preservation ----------
    {
        std::vector<float> clean(total), tone(total), out(total);
        std::mt19937 rng(9);
        std::normal_distribution<float> tiny(0.0f, 0.02f);
        for (size_t i = 0; i < total; ++i) {
            if (i >= noiseOnly) {
                clean[i] = 0.5f * std::sin(2.0 * M_PI * freq * (i - noiseOnly) / kRate);
            }
            tone[i] = clean[i] + tiny(rng);
        }
        Denoiser denoiser(kRate);
        denoiser.process(tone.data(), out.data(), total);

        const size_t s = total - kRate / 2;
        const double ratio =
            rms(out.data() + s, kRate / 2) /
            std::max(rms(clean.data() + s, kRate / 2), 1e-6);
        std::printf("tone preservation ratio: %.2f\n", ratio);
        if (ratio < 0.5 || ratio > 1.5) {
            std::fprintf(stderr, "signal not preserved\n");
            return 1;
        }
    }

    // ---------- Part 3: SNR improvement on tone + noise ----------
    {
        std::vector<float> clean(total), noisy(total), out(total);
        std::mt19937 rng(42);
        std::normal_distribution<float> dist(0.0f, 0.5f);
        for (size_t i = 0; i < total; ++i) {
            if (i >= noiseOnly) {
                clean[i] = 0.5f * std::sin(2.0 * M_PI * freq * (i - noiseOnly) / kRate);
            }
            noisy[i] = clean[i] + dist(rng);
        }

        const double inSnr = snrDb(clean.data(), noisy.data(), total);
        Denoiser denoiser(kRate);
        denoiser.process(noisy.data(), out.data(), total);

        const size_t s = total - kRate / 2;
        const double outSnr = bestSnrDb(out, clean, s, kRate / 2);
        std::printf("SNR in=%7.1f dB  out=%7.1f dB  improvement=%5.1f dB\n",
                    inSnr, outSnr, outSnr - inSnr);

        for (size_t i = 0; i < total; ++i) {
            if (!std::isfinite(out[i])) {
                std::fprintf(stderr, "non-finite sample at %zu\n", i);
                return 1;
            }
        }
        if (outSnr - inSnr < 8.0) {
            std::fprintf(stderr, "denoise test FAILED\n");
            return 1;
        }
    }

    std::printf("OK\n");
    return 0;
}
