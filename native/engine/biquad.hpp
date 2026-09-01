// RBJ Audio EQ Cookbook biquads (public-domain formulas).
#pragma once

#include <cmath>

struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float z1 = 0, z2 = 0;

  float process(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }

  void reset() {
    z1 = 0;
    z2 = 0;
  }

  void identity() {
    b0 = 1;
    b1 = b2 = a1 = a2 = 0;
    reset();
  }

  void lowPass(float sampleRate, float freq, float q) {
    const float w0 = 2.0f * 3.14159265358979323846f * freq / sampleRate;
    const float cosw = std::cos(w0);
    const float sinw = std::sin(w0);
    const float alpha = sinw / (2.0f * q);
    const float b0n = (1.0f - cosw) * 0.5f;
    const float b1n = 1.0f - cosw;
    const float b2n = (1.0f - cosw) * 0.5f;
    const float a0n = 1.0f + alpha;
    const float a1n = -2.0f * cosw;
    const float a2n = 1.0f - alpha;
    b0 = b0n / a0n;
    b1 = b1n / a0n;
    b2 = b2n / a0n;
    a1 = a1n / a0n;
    a2 = a2n / a0n;
  }

  void highPass(float sampleRate, float freq, float q) {
    const float w0 = 2.0f * 3.14159265358979323846f * freq / sampleRate;
    const float cosw = std::cos(w0);
    const float sinw = std::sin(w0);
    const float alpha = sinw / (2.0f * q);
    const float b0n = (1.0f + cosw) * 0.5f;
    const float b1n = -(1.0f + cosw);
    const float b2n = (1.0f + cosw) * 0.5f;
    const float a0n = 1.0f + alpha;
    const float a1n = -2.0f * cosw;
    const float a2n = 1.0f - alpha;
    b0 = b0n / a0n;
    b1 = b1n / a0n;
    b2 = b2n / a0n;
    a1 = a1n / a0n;
    a2 = a2n / a0n;
  }
};

// Two cascaded Butterworth biquads = Linkwitz-Riley 4th order (-24 dB/oct).
struct LR4 {
  Biquad a, b;
  static constexpr float kButterQ = 0.707106781f;

  void lowPass(float sampleRate, float freq) {
    a.lowPass(sampleRate, freq, kButterQ);
    b.lowPass(sampleRate, freq, kButterQ);
  }

  void highPass(float sampleRate, float freq) {
    a.highPass(sampleRate, freq, kButterQ);
    b.highPass(sampleRate, freq, kButterQ);
  }

  float process(float x) { return b.process(a.process(x)); }
};
