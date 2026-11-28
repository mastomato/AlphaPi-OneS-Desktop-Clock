#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "audio_resampler_coeffs.h"

namespace radio {
// 48-tap, 64-phase low-pass resampler. Input/output timing uses integers, so
// decoder frame boundaries cannot introduce drift or repeated samples.
class AudioResampler {
 public:
  void Reset(uint32_t input_rate, uint32_t output_rate = 24000, void (*yield)() = nullptr) {
    const bool cached = input_rate_ == input_rate && output_rate_ == output_rate;
    input_rate_ = input_rate;
    output_rate_ = output_rate;
    distance_ = 0;
    history_.fill(0);
    head_ = 0;
    if (input_rate == output_rate || cached) return;
    if (!input_rate || !output_rate) return;
    // Every MP3 sample rate uses one of these four filters at 24 kHz output.
    // Copy once into the resampler's working RAM instead of computing trig
    // functions on the single-core S2 during buffering/station changes.
    const int16_t (*preset)[kTaps] = nullptr;
    if (output_rate == 24000) {
      if (input_rate <= 24000) preset = resampler_coeffs::k24000;
      else if (input_rate == 32000) preset = resampler_coeffs::k32000;
      else if (input_rate == 44100) preset = resampler_coeffs::k44100;
      else if (input_rate == 48000) preset = resampler_coeffs::k48000;
    }
    if (preset) { std::memcpy(coeff_, preset, sizeof(coeff_)); return; }
    constexpr double pi = 3.14159265358979323846;
    const double cutoff = 0.45 * std::min(1.0, double(output_rate) / input_rate);
    for (int phase = 0; phase < kPhases; ++phase) {
      double weights[kTaps], sum = 0;
      for (int k = 0; k < kTaps; ++k) {
        const double x = (kTaps - 1) * 0.5 + double(phase) / kPhases - k;
        const double sinc = std::abs(x) < 1e-9 ? 2 * cutoff :
                            std::sin(2 * pi * cutoff * x) / (pi * x);
        const double window = 0.42 - 0.5 * std::cos(2 * pi * k / (kTaps - 1)) +
                              0.08 * std::cos(4 * pi * k / (kTaps - 1));
        sum += weights[k] = sinc * window;
      }
      int total = 0;
      for (int k = 0; k < kTaps; ++k) {
        total += coeff_[phase][k] = static_cast<int16_t>(std::lround(weights[k] / sum * 32768));
      }
      coeff_[phase][kTaps / 2] += 32768 - total;
      // Building coefficients uses software floating point on ESP32-S2.
      // Give the screen/controls a chance to run during a sample-rate change.
      if (yield && (phase & 7) == 7) yield();
    }
  }
  void Process(const int16_t* input, size_t count, std::vector<int16_t>& out) {
    out.clear();
    if (input_rate_ == output_rate_) { out.insert(out.end(),input,input+count); return; }
    if (!input_rate_ || !output_rate_) return;
    for (size_t i = 0; i < count; ++i) {
      head_ = (head_ + 1) & 63;
      history_[head_] = history_[head_ + 64] = input[i];
      // Bounded rational phase: equivalent to the old growing 64-bit time,
      // without a software 64-bit division for every output sample.
      while (distance_ >= 0) {
        const int phase = std::min(kPhases - 1, int(uint32_t(distance_) * kPhases / output_rate_));
        int64_t sum = 0;
        const int16_t* samples = history_.data() + head_ + 64;
        for (int k = 0; k < kTaps; ++k)
          sum += static_cast<int32_t>(samples[-k]) * coeff_[phase][k];
        const int64_t rounded = sum >= 0 ? (sum + 16384) / 32768 : -((-sum + 16384) / 32768);
        out.push_back(static_cast<int16_t>(std::clamp<int64_t>(rounded,-32768,32767)));
        distance_ -= static_cast<int32_t>(input_rate_);
      }
      distance_ += static_cast<int32_t>(output_rate_);
    }
  }
 private:
  static constexpr int kTaps = 48, kPhases = 64;
  uint32_t input_rate_ = 0, output_rate_ = 24000;
  int32_t distance_ = 0;
  int head_ = 0;
  std::array<int16_t,128> history_{};
  int16_t coeff_[kPhases][kTaps]{};
};
}

